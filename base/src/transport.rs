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
        serve_client(&handle, pipe);
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
fn serve_client(handle: &BaseHandle, pipe: HANDLE) {
    let (ticket, events) = handle.subscribe();
    let mut reader = BufReader::new(PipeRead(pipe));
    let mut writer = PipeWrite(pipe);
    let mut pending: Vec<String> = Vec::new();

    loop {
        // Whatever the client has sent, without blocking.
        loop {
            let mut line = String::new();
            match read_line_if_ready(&mut reader, &mut line) {
                Ok(true) => {
                    if !line.trim().is_empty() {
                        pending.push(line);
                    }
                }
                Ok(false) => break,
                // The client is gone.
                Err(_) => {
                    let _ = handle.unsubscribe(ticket);
                    return;
                }
            }
        }

        // Commands first: a client that sent one is waiting for its effect.
        while let Some(line) = pending.first().cloned() {
            pending.remove(0);
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

/// Read one line only if the pipe already holds something.
///
/// `PeekNamedPipe` reports how many bytes are waiting; reading without checking
/// blocks, and a blocking read in this loop would stop the server writing events
/// while it waited for a command.
fn read_line_if_ready(
    reader: &mut BufReader<PipeRead>,
    line: &mut String,
) -> std::io::Result<bool> {
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
        return Ok(false);
    }
    reader.read_line(line)?;
    Ok(true)
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
