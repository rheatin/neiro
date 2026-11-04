//! An out-of-process transport: one JSON object per line over a named pipe.
//!
//! The protocol types were made serializable so a host in another process could
//! carry exactly the same values. This is that host's side of the wire.
//!
//! # Why a named pipe
//!
//! A TCP socket needs a port, is reachable from another machine, and has no
//! notion of who is calling. A named pipe on Windows applies the caller's
//! credentials, needs no port, and is not reachable off the machine. For a
//! local UI that is the whole access-control story.
//!
//! # The shape
//!
//! Line-delimited JSON, one object per line, both directions. A client sends
//! `Command` values and receives `Event` values on the same connection, so there
//! is one stream to read rather than two to correlate.
//!
//! The server is single-client: the control thread owns the session, and two
//! clients issuing commands would be two owners of one queue.

#![cfg(windows)]

use std::io::{BufRead, BufReader, Write};
use std::sync::mpsc::TryRecvError;
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use windows_sys::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_GENERIC_READ, FILE_GENERIC_WRITE,
    FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING, PIPE_ACCESS_DUPLEX,
};
use windows_sys::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, PIPE_READMODE_BYTE,
    PIPE_TYPE_BYTE, PIPE_WAIT,
};

use crate::protocol::{Command, Event};
use crate::BaseHandle;

/// The pipe a client connects to for the default instance.
///
/// `\\.\pipe\` is the local namespace; there is no remote form of this path.
pub fn default_pipe_name() -> String {
    r"\\.\pipe\neiro-base".to_string()
}

/// A running server. Dropping it stops the listener and disconnects the client.
pub struct Transport {
    stop: Arc<Mutex<bool>>,
    thread: Option<JoinHandle<()>>,
    pipe_name: String,
}

impl Transport {
    /// Start serving `handle` on `pipe_name`.
    ///
    /// The listener blocks on a client, so a host that wants to know whether
    /// anyone connected waits for [`first_client`](Self::first_client) rather
    /// than for this to return -- which it does immediately.
    pub fn start(handle: BaseHandle, pipe_name: &str) -> std::io::Result<Self> {
        let stop = Arc::new(Mutex::new(false));
        let thread_stop = stop.clone();
        let name = pipe_name.to_string();
        let thread_name = name.clone();
        let thread = thread::Builder::new()
            .name("neiro-base-transport".into())
            .spawn(move || serve(handle, &thread_name, thread_stop))?;
        Ok(Self {
            stop,
            thread: Some(thread),
            pipe_name: name,
        })
    }

    pub fn pipe_name(&self) -> &str {
        &self.pipe_name
    }

    pub fn stop(&mut self) {
        if let Ok(mut guard) = self.stop.lock() {
            *guard = true;
        }
        // Wake the listener: a blocking ConnectNamedPipe would otherwise keep
        // the thread alive until someone connected.
        poke(&self.pipe_name);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

impl Drop for Transport {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Connect once to unblock a listener that is waiting. The connection is
/// discarded; its only purpose is to let the thread notice the stop flag.
fn poke(pipe_name: &str) {
    if let Some(handle) = open_client(pipe_name) {
        unsafe { CloseHandle(handle) };
    }
}

fn wide(text: &str) -> Vec<u16> {
    let mut units: Vec<u16> = text.encode_utf16().collect();
    units.push(0);
    units
}

fn open_client(pipe_name: &str) -> Option<HANDLE> {
    let name = wide(pipe_name);
    let handle = unsafe {
        CreateFileW(
            name.as_ptr(),
            FILE_GENERIC_READ | FILE_GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            std::ptr::null(),
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            std::ptr::null_mut(),
        )
    };
    if handle == INVALID_HANDLE_VALUE {
        None
    } else {
        Some(handle)
    }
}

fn serve(handle: BaseHandle, pipe_name: &str, stop: Arc<Mutex<bool>>) {
    let name = wide(pipe_name);
    loop {
        if stop.lock().map(|g| *g).unwrap_or(true) {
            return;
        }
        let pipe = unsafe {
            CreateNamedPipeW(
                name.as_ptr(),
                // Duplex: commands in, events out, on one connection. The
                // earlier version passed the pipe *mode* into the open-mode
                // slot, which is why the call failed with ERROR_INVALID_PARAMETER
                // -- both type and read-mode are 0 (byte), so the value was a
                // valid pipe mode and an invalid open mode at once.
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,          // one instance: one client owns the session
                4096,       // outbound buffer
                4096,       // inbound buffer
                0,          // default timeout
                std::ptr::null(),
            )
        };
        if pipe == INVALID_HANDLE_VALUE {
            return;
        }
        // Blocks until a client connects. A zero return is not a failure here:
        // ERROR_PIPE_CONNECTED means the client arrived before this call, which
        // is a normal race and still a real connection.
        let _ = unsafe { ConnectNamedPipe(pipe, std::ptr::null_mut()) };
        if stop.lock().map(|g| *g).unwrap_or(true) {
            unsafe {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
            return;
        }
        // `ConnectNamedPipe` returns 0 when a client connects after the call
        // began; the error is ERROR_PIPE_CONNECTED in that case and the
        // connection is real. Either way, serve it and see what happens.
        serve_client(&handle, pipe, &stop);
        unsafe {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }
}

/// One client's lifetime.
///
/// One thread reads and writes, using `PeekNamedPipe` so a read never blocks.
/// The earlier version put reads on their own thread and shared the handle: two
/// threads on one pipe is a race the pipe does not arbitrate, and the client's
/// first command was consumed by whichever got there first -- which is why the
/// check hung after connecting.
fn serve_client(handle: &BaseHandle, pipe: HANDLE, stop: &Arc<Mutex<bool>>) {
    let (ticket, events) = handle.subscribe();
    let mut reader = BufReader::new(PipeRead(pipe));
    let mut writer = PipeWrite(pipe);
    let mut buffer = LineBuffer {
        bytes: Vec::new(),
        overlong: false,
    };
    // Bounded, and a VecDeque so taking the front is O(1): a `Vec` with
    // `remove(0)` made a burst O(n^2), which is how a client sending faster than
    // the control thread consumes turns into a stall rather than a slow reply.
    let mut pending: std::collections::VecDeque<String> = std::collections::VecDeque::new();
    let mut overflowed = false;

    loop {
        // `Transport::stop` has to be able to end this loop while a client is
        // connected. Without the check the join in `stop` waited for the client
        // to disconnect, which never happens for a UI that is still running --
        // so shutting Base down hung.
        if stop.lock().map(|g| *g).unwrap_or(true) {
            let _ = handle.unsubscribe(ticket);
            return;
        }
        // Whatever the client has sent, without blocking.
        loop {
            match poll_line(&mut reader, &mut buffer) {
                Ok(Some(line)) => {
                    if line.is_empty() && buffer.overlong {
                        continue;
                    }
                    if line.is_empty() {
                        // An overlong line was discarded. Tell the client once
                        // per burst rather than once per line: a flood of them
                        // would otherwise be answered with a flood of errors.
                        if !overflowed {
                            overflowed = true;
                            let event = Event::Error {
                                op: "transport".into(),
                                message: format!(
                                    "a command line longer than {MAX_LINE_BYTES} bytes was dropped"
                                ),
                            };
                            if write_event(&mut writer, &event).is_err() {
                                let _ = handle.unsubscribe(ticket);
                                return;
                            }
                        }
                        continue;
                    }
                    if pending.len() >= MAX_PENDING_COMMANDS {
                        // The client is outrunning the control thread, which
                        // consumes one command per poll. Dropping the newest is
                        // the same rule the event bus uses: the oldest is the
                        // one already in flight.
                        let event = Event::Error {
                            op: "transport".into(),
                            message: format!(
                                "more than {MAX_PENDING_COMMANDS} commands are queued; dropping the newest"
                            ),
                        };
                        if write_event(&mut writer, &event).is_err() {
                            let _ = handle.unsubscribe(ticket);
                            return;
                        }
                        continue;
                    }
                    if !line.trim().is_empty() {
                        pending.push_back(line);
                    }
                }
                Ok(None) => break,
                // The client is gone.
                Err(_) => {
                    let _ = handle.unsubscribe(ticket);
                    return;
                }
            }
        }

        // Commands first: a client that sent one is waiting for its effect.
        while let Some(line) = pending.pop_front() {
            let trimmed = line.trim();
            if trimmed.is_empty() {
                continue;
            }
            match serde_json::from_str::<Command>(trimmed) {
                Ok(command) => {
                    if handle.dispatch(command).is_err() {
                        let _ = handle.unsubscribe(ticket);
                        return;
                    }
                }
                Err(e) => {
                    // A malformed command is reported rather than dropped: a
                    // client that sent garbage would otherwise see silence and
                    // assume the server had died.
                    let event = Event::Error {
                        op: "transport".into(),
                        message: format!("could not parse a command: {e}"),
                    };
                    if write_event(&mut writer, &event).is_err() {
                        let _ = handle.unsubscribe(ticket);
                        return;
                    }
                }
            }
        }

        // Then whatever events are waiting.
        loop {
            match events.try_recv() {
                Ok(event) => {
                    if write_event(&mut writer, &event).is_err() {
                        let _ = handle.unsubscribe(ticket);
                        return;
                    }
                }
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    let _ = handle.unsubscribe(ticket);
                    return;
                }
            }
        }

        // Nothing to do: sleep briefly so the loop does not spin.
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
}

/// How much a client may send before it is told to slow down.
///
/// A command is a JSON object of a few hundred bytes; 4 KiB is generous. The
/// limit matters because the alternative is unbounded: a client that sends
/// without ever ending a line would otherwise accumulate bytes until the process
/// runs out of memory, and a client that sends faster than the control thread
/// consumes would grow `pending` and the command channel without limit.
const MAX_LINE_BYTES: usize = 4096;

/// How many commands may be waiting for the control thread.
///
/// The control thread consumes one command per poll (30 ms), so this is roughly
/// ten seconds of burst. Beyond it the client is not a UI sending clicks, and
/// buffering further would only delay the reply it is waiting for.
const MAX_PENDING_COMMANDS: usize = 256;

/// A partial command: bytes read so far, and whether they overflowed the limit.
struct LineBuffer {
    bytes: Vec<u8>,
    overlong: bool,
}

/// Read whatever the pipe holds, up to a newline, without blocking.
///
/// Returns a completed line when one arrives. `PeekNamedPipe` reports how many
/// bytes are waiting, and only that many are read -- a `read_line` would block
/// on a client that sent bytes without a newline, and during that block the
/// single-client loop would serve no events at all.
///
/// A line longer than `MAX_LINE_BYTES` is dropped rather than buffered: the
/// client is told once, and its bytes are discarded until the newline so the
/// stream stays aligned.
fn poll_line(
    reader: &mut BufReader<PipeRead>,
    buffer: &mut LineBuffer,
) -> std::io::Result<Option<String>> {
    use windows_sys::Win32::System::Pipes::PeekNamedPipe;
    let mut available: u32 = 0;
    let ok = unsafe {
        PeekNamedPipe(
            reader.get_ref().0,
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            &mut available,
            std::ptr::null_mut(),
        )
    };
    if ok == 0 {
        return Err(std::io::Error::last_os_error());
    }
    if available == 0 {
        return Ok(None);
    }

    // Read exactly what is there, no more: `fill_buf` on a `BufReader` would
    // try to refill and block once the available bytes are consumed.
    let take = (available as usize).min(MAX_LINE_BYTES * 4);
    let mut chunk = vec![0u8; take];
    let read = std::io::Read::read(reader.get_mut(), &mut chunk)?;
    chunk.truncate(read);

    for &byte in &chunk {
        if byte == b'\n' {
            let line = String::from_utf8_lossy(&buffer.bytes).into_owned();
            buffer.bytes.clear();
            let overlong = buffer.overlong;
            buffer.overlong = false;
            // An overlong line yields nothing: the client is told once, and the
            // bytes up to the newline are discarded so the stream realigns.
            return Ok(if overlong { Some(String::new()) } else { Some(line) });
        }
        if buffer.overlong {
            continue;
        }
        if buffer.bytes.len() >= MAX_LINE_BYTES {
            buffer.overlong = true;
            buffer.bytes.clear();
            continue;
        }
        buffer.bytes.push(byte);
    }
    Ok(None)
}

fn write_event(writer: &mut PipeWrite, event: &Event) -> std::io::Result<()> {
    let json = serde_json::to_string(event)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    writeln!(writer, "{json}")
}

/// Reads from a pipe handle.
struct PipeRead(HANDLE);

impl std::io::Read for PipeRead {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        use windows_sys::Win32::Storage::FileSystem::ReadFile;
        let mut read: u32 = 0;
        let ok = unsafe {
            ReadFile(
                self.0,
                buf.as_mut_ptr(),
                buf.len() as u32,
                &mut read,
                std::ptr::null_mut(),
            )
        };
        if ok == 0 {
            return Err(std::io::Error::last_os_error());
        }
        Ok(read as usize)
    }
}

/// Writes to a pipe handle.
struct PipeWrite(HANDLE);

impl Write for PipeWrite {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        use windows_sys::Win32::Storage::FileSystem::WriteFile;
        let mut written: u32 = 0;
        let ok = unsafe {
            WriteFile(
                self.0,
                buf.as_ptr(),
                buf.len() as u32,
                &mut written,
                std::ptr::null_mut(),
            )
        };
        if ok == 0 {
            return Err(std::io::Error::last_os_error());
        }
        Ok(written as usize)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

// SAFETY: a pipe handle is an opaque kernel object; it is valid to move one
// between threads, which is what the reader thread does. It is not a Rust
// reference and carries no borrow.
unsafe impl Send for PipeRead {}
unsafe impl Send for PipeWrite {}

/// Connect to a running server. Used by the check and by a host.
pub fn connect(pipe_name: &str) -> std::io::Result<Client> {
    let handle = open_client(pipe_name).ok_or_else(std::io::Error::last_os_error)?;
    Ok(Client {
        pipe: handle,
        reader: None,
    })
}

/// A client of a running server.
pub struct Client {
    pipe: HANDLE,
    /// One buffered reader for the client's lifetime. A fresh one per `recv`
    /// drops whatever it had already read.
    reader: Option<BufReader<PipeRead>>,
}

impl Client {
    pub fn send(&mut self, command: &Command) -> std::io::Result<()> {
        let json = serde_json::to_string(command)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
        let mut writer = PipeWrite(self.pipe);
        writeln!(writer, "{json}")
    }

    /// Send raw text, for a check that needs to send something malformed.
    ///
    /// A host never does this; the point is that the server's answer to garbage
    /// is observable rather than silent.
    pub fn send_raw(&mut self, line: &str) -> std::io::Result<()> {
        let mut writer = PipeWrite(self.pipe);
        writer.write_all(line.as_bytes())
    }

    /// Read one event, blocking.
    ///
    /// The reader is built once per call, which loses anything a `BufReader`
    /// had already pulled off the pipe: two events arriving together would
    /// deliver the first and discard the second. A host that sends a command
    /// and then reads one event would miss the answer whenever the server
    /// emitted a second one first -- which it does routinely.
    pub fn recv(&mut self) -> std::io::Result<Option<Event>> {
        // Reuse the buffered reader across calls, so a line pulled from the
        // pipe but not yet consumed survives.
        let reader = self
            .reader
            .get_or_insert_with(|| BufReader::new(PipeRead(self.pipe)));
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
        unsafe { CloseHandle(self.pipe) };
    }
}

// SAFETY: same as the pipe handle above.
unsafe impl Send for Client {}
