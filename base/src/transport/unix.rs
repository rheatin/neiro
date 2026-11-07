// The transport on Unix: a Unix domain socket carrying the same protocol.
//
// Same properties as the Windows named pipe, and chosen for the same reasons: a
// filesystem path needs no port number, cannot be reached from another machine,
// and the file's permissions are the access control (the Windows pipe uses the
// caller's credentials; here it is mode 0600 in the user's temp directory).
//
// The wire format is identical -- one JSON command per line in, one JSON event
// per line out -- because the protocol is the contract and the transport is not
// allowed to have an opinion about it. A host that speaks to Base on Windows
// speaks to it here unchanged.
//
// This file is the counterpart of `mod.rs`, not a fork of it: `mod.rs` is Windows
// and is compiled only there. The serve loop is written again here rather than
// shared, because the Windows version is built around a non-blocking
// `PeekNamedPipe` pump and this one blocks on the socket -- the two have to
// differ at the read, and duplicating sixty lines of loop is less risky than
// threading a trait through a working pipe implementation to save them.
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread::{self, JoinHandle};

use crate::protocol::{Command, Event};
use crate::BaseHandle;

const MAX_LINE_BYTES: usize = 4096;

/// Where a client connects for the default instance.
///
/// The user's temp directory rather than `/tmp`: it is per-user on macOS, so the
/// socket inherits a directory nobody else can write to.
pub fn default_pipe_name() -> String {
    sock_path("neiro-base").to_string_lossy().into_owned()
}

/// How many servers are bound to each path, in this process.
///
/// Two servers for one name is a mistake, but it happens cheaply: `server =
/// Transport::start(...)` binds the new socket before the old value is dropped,
/// and the old `stop` then unlinks the path the new one is listening on. The
/// file belongs to whoever bound it last, so an unlink is only correct when no
/// server still holds the path.
fn live_paths() -> &'static Mutex<HashMap<PathBuf, usize>> {
    static LIVE: OnceLock<Mutex<HashMap<PathBuf, usize>>> = OnceLock::new();
    LIVE.get_or_init(|| Mutex::new(HashMap::new()))
}

fn claim(path: &Path) {
    let mut live = live_paths().lock().unwrap_or_else(|e| e.into_inner());
    *live.entry(path.to_path_buf()).or_insert(0) += 1;
}

/// Returns true when this was the last holder, so the file may be removed.
fn release(path: &Path) -> bool {
    let mut live = live_paths().lock().unwrap_or_else(|e| e.into_inner());
    match live.get_mut(path) {
        Some(count) => {
            *count -= 1;
            if *count == 0 {
                live.remove(path);
                true
            } else {
                false
            }
        }
        None => true,
    }
}

fn sock_path(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!("{name}.sock"))
}

/// A running server. Dropping it stops the listener and disconnects the client.
pub struct Transport {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
    name: String,
}

impl Transport {
    pub fn start(handle: BaseHandle, name: &str) -> std::io::Result<Self> {
        let path = PathBuf::from(name);
        // A socket left by a previous run would make `bind` fail with EADDRINUSE.
        // Removing it is safe: a live server would have been found by `connect`
        // before this ran, and a stale file is not a server.
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path)?;
        claim(&path);
        listener.set_nonblocking(true)?;

        let stop = Arc::new(AtomicBool::new(false));
        let thread_stop = stop.clone();
        let thread = thread::Builder::new()
            .name("neiro-base-transport".into())
            .spawn(move || serve(handle, listener, thread_stop))?;
        Ok(Self {
            stop,
            thread: Some(thread),
            name: name.to_string(),
        })
    }

    pub fn pipe_name(&self) -> &str {
        &self.name
    }

    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        // The accept loop is non-blocking, so the flag is seen within one poll
        // interval; there is nothing to wake.
        //
        // The join is also the guard: `Drop` calls this, so a server stopped
        // explicitly and then dropped would otherwise release its path twice and
        // unlink the socket of whichever server bound it next.
        let Some(thread) = self.thread.take() else {
            return;
        };
        let _ = thread.join();
        let path = PathBuf::from(&self.name);
        if release(&path) {
            let _ = std::fs::remove_file(&path);
        }
    }
}

impl Drop for Transport {
    fn drop(&mut self) {
        self.stop();
    }
}

fn serve(handle: BaseHandle, listener: UnixListener, stop: Arc<AtomicBool>) {
    loop {
        if stop.load(Ordering::SeqCst) {
            return;
        }
        match listener.accept() {
            Ok((stream, _)) => {
                serve_client(&handle, stream, &stop);
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(std::time::Duration::from_millis(20));
            }
            Err(_) => return,
        }
    }
}

fn serve_client(handle: &BaseHandle, stream: UnixStream, stop: &Arc<AtomicBool>) {
    let (ticket, events) = handle.subscribe();
    let write_half = match stream.try_clone() {
        Ok(s) => s,
        Err(_) => {
            let _ = handle.unsubscribe(ticket);
            return;
        }
    };
    // The reader blocks; the stop flag is checked between commands. A server
    // that must also be interruptible while no command is arriving sets
    // `set_read_timeout`, which is what makes `stop` prompt rather than waiting
    // for the host to disconnect.
    let _ = stream.set_read_timeout(Some(std::time::Duration::from_millis(50)));
    let mut reader = BufReader::new(stream);
    let mut writer = write_half;

    loop {
        if stop.load(Ordering::SeqCst) {
            let _ = handle.unsubscribe(ticket);
            return;
        }
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) => {
                // The client hung up.
                let _ = handle.unsubscribe(ticket);
                return;
            }
            Ok(_) => {}
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                // Nothing arrived within the timeout; loop so the stop flag is
                // seen. Events still get flushed below.
            }
            Err(_) => {
                let _ = handle.unsubscribe(ticket);
                return;
            }
        }

        let trimmed = line.trim();
        if !trimmed.is_empty() {
            if trimmed.len() > MAX_LINE_BYTES {
                let event = Event::Error {
                    op: "transport".into(),
                    message: format!("a command line longer than {MAX_LINE_BYTES} bytes was dropped"),
                };
                if write_event(&mut writer, &event).is_err() {
                    let _ = handle.unsubscribe(ticket);
                    return;
                }
            } else {
                match serde_json::from_str::<Command>(trimmed) {
                    Ok(command) => {
                        if handle.dispatch(command).is_err() {
                            let _ = handle.unsubscribe(ticket);
                            return;
                        }
                    }
                    Err(e) => {
                        // Answer garbage rather than ignoring it: a host that can
                        // see the reason is one that can fix its own encoding.
                        let event = Event::Error {
                            op: "transport".into(),
                            message: format!("could not read the command: {e}"),
                        };
                        if write_event(&mut writer, &event).is_err() {
                            let _ = handle.unsubscribe(ticket);
                            return;
                        }
                    }
                }
            }
        }

        // Drain whatever the control thread has produced.
        loop {
            match events.try_recv() {
                Ok(event) => {
                    if write_event(&mut writer, &event).is_err() {
                        let _ = handle.unsubscribe(ticket);
                        return;
                    }
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => break,
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    let _ = handle.unsubscribe(ticket);
                    return;
                }
            }
        }
    }
}

fn write_event(writer: &mut UnixStream, event: &Event) -> std::io::Result<()> {
    let json = serde_json::to_string(event)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    writeln!(writer, "{json}")
}

/// Connect to a running server.
pub fn connect(name: &str) -> std::io::Result<Client> {
    let stream = UnixStream::connect(name)?;
    let reader = stream.try_clone()?;
    Ok(Client {
        writer: stream,
        reader: Some(BufReader::new(reader)),
    })
}

pub struct Client {
    writer: UnixStream,
    /// One buffered reader for the client's lifetime. A fresh one per `recv`
    /// would drop whatever it had already read, so two events arriving together
    /// would deliver the first and discard the second.
    reader: Option<BufReader<UnixStream>>,
}

impl Client {
    pub fn send(&mut self, command: &Command) -> std::io::Result<()> {
        let json = serde_json::to_string(command)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
        writeln!(self.writer, "{json}")
    }

    /// Send raw text, for a check that needs to send something malformed.
    pub fn send_raw(&mut self, line: &str) -> std::io::Result<()> {
        self.writer.write_all(line.as_bytes())
    }

    /// Read one event, blocking.
    pub fn recv(&mut self) -> std::io::Result<Option<Event>> {
        let reader = self
            .reader
            .get_or_insert_with(|| BufReader::new(self.writer.try_clone().expect("clone socket")));
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) => Ok(None),
            Ok(_) => serde_json::from_str(line.trim())
                .map(Some)
                .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e)),
            Err(e) => Err(e),
        }
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        // Closing the socket is what tells the server the client hung up.
        let _ = self.writer.shutdown(std::net::Shutdown::Both);
    }
}
