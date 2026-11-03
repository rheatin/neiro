//! Error type for the audio engine boundary.

use std::fmt;

use crate::ffi;

/// Anything that can go wrong when talking to the engine.
#[derive(Debug)]
pub enum EngineError {
    /// The engine returned a non-zero `NAE_*` code.
    ///
    /// `hresult` is the last HRESULT the engine saw, which distinguishes "this
    /// device cannot do 96 kHz" from "this device is already in use".
    ///
    /// `message` is the engine's own words when it had something to say, and
    /// empty otherwise. The code names a category; only the message says which
    /// file was unreadable or that another program is holding the output. Every
    /// error surfaced to a user goes through here, so this field is what the UI
    /// ends up showing.
    Code {
        code: i32,
        hresult: u32,
        message: String,
    },

    /// A returned string was not valid UTF-8, so it could not be handed to Rust.
    NotUtf8,

    /// The engine returned a null pointer where a value was required.
    NullPointer,

    /// The device list did not parse as the expected JSON shape.
    BadPayload(String),

    /// The output device went away mid-playback: unplugged, disabled, or
    /// preempted by another application holding it exclusively.
    ///
    /// Its own variant rather than a `Code` to be pattern-matched on, because the
    /// handling is not a variation on error handling -- it is the opposite. Every
    /// other error ends the stream; this one pauses it. The file is fine, the
    /// decoder is fine, the position is intact, and the same handle can carry on
    /// once the caller has a device to give it.
    ///
    /// It is deliberately *not* automatically retried on another device. Silently
    /// moving a hi-fi listener off the DAC they selected, because something took
    /// the endpoint for a moment, is worse than stopping: they would be hearing
    /// a different output path than the one the application says is active, and
    /// on a path that is not bit-perfect, without being told.
    DeviceLost {
        /// The engine's own words, which name the situation for the user.
        message: String,
        /// The HRESULT that identified this, kept so the report still says *how*
        /// the device went.
        ///
        /// The three codes are different situations and only two of them are the
        /// one being tested: AUDCLNT_E_DEVICE_INVALIDATED is an unplug,
        /// AUDCLNT_E_RESOURCES_INVALIDATED is a device disabled in the Sound
        /// control panel, and AUDCLNT_E_SERVICE_NOT_RUNNING is the audio service
        /// going down. Without this, a bug report said "code 10" and nothing
        /// more, and the difference between "their cable came loose" and "they
        /// disabled a device in settings" had to be guessed at.
        hresult: u32,
    },

    /// The DLL could not be loaded at runtime.
    LoadFailed(String),
}

impl EngineError {
    /// Turns a raw `NAE_*` code into an error, pulling in the HRESULT.
    ///
    /// The HRESULT has to be read *before* anything else calls into the engine,
    /// because it is per-thread and the next call would overwrite it.
    pub fn from_code(code: i32) -> Self {
        EngineError::Code {
            code,
            hresult: unsafe { ffi::NAE_GetLastHresult() as u32 },
            message: String::new(),
        }
    }

    /// Attaches whatever the engine last said about the failure, if anything.
    ///
    /// The engine keeps one message per thread, so this must be called straight
    /// after the failing call and before anything else touches the engine —
    /// `from_code` already grabs the HRESULT for the same reason.
    ///
    /// An empty engine message leaves the error untouched rather than replacing
    /// the code with an empty string. A generic message derived from the code is
    /// still better than a blank one, and that is the job of `describe`.
    pub fn with_engine_message(self) -> Self {
        let Self::Code { code, hresult, .. } = self else {
            return self;
        };
        // SAFETY: NAE_GetLastErrorText writes at most `buf.len() - 1` bytes plus a
        // NUL into a buffer we own, and returns the count written. The engine
        // cannot write past the length it was given.
        let mut buf = [0i8; 512];
        let written = unsafe { ffi::NAE_GetLastErrorText(buf.as_mut_ptr(), buf.len() as i32) };
        if written <= 0 {
            return EngineError::Code {
                code,
                hresult,
                message: String::new(),
            };
        }
        let bytes =
            unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, written as usize) };
        let message = String::from_utf8_lossy(bytes).into_owned();
        // A lost device gets its own variant on the way out, so that every caller
        // downstream can react to it by matching rather than by reading the
        // message or knowing NAE_ERR_DEVICE_LOST's number. Splitting it here
        // rather than at each call site means the split is made once, next to the
        // code that defines it.
        if code == ffi::NAE_ERR_DEVICE_LOST {
            return EngineError::DeviceLost { message, hresult };
        }
        EngineError::Code {
            code,
            hresult,
            message,
        }
    }

    /// The `NAE_*` code behind this error, or −1 when it did not come from one.
    ///
    /// [`DeviceLost`](Self::DeviceLost) reports its own real code rather than −1.
    /// That matters because the number is what the UI switches on: a message
    /// saying "the output device was removed" is a string the frontend has to
    /// trust, while `code == 10` is a fact it can test. Reporting −1 for the one
    /// error the UI is specifically meant to recognise would leave it with
    /// nothing to branch on except the text.
    pub fn code(&self) -> i32 {
        match self {
            Self::Code { code, .. } => *code,
            Self::DeviceLost { .. } => ffi::NAE_ERR_DEVICE_LOST,
            _ => -1,
        }
    }

    /// The last HRESULT the engine saw, or 0 when none applies.
    pub fn hresult(&self) -> u32 {
        match self {
            Self::Code { hresult, .. } => *hresult,
            Self::DeviceLost { hresult, .. } => *hresult,
            _ => 0,
        }
    }
}

impl fmt::Display for EngineError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            EngineError::Code {
                code,
                hresult,
                message,
            } => {
                // The engine's own words first, when it had any. They are the part
                // a user can act on; the code and HRESULT are for a bug report.
                if message.is_empty() {
                    write!(
                        f,
                        "{} (code {code}, hresult 0x{hresult:08X})",
                        describe(*code)
                    )
                } else {
                    write!(f, "{message} (code {code}, hresult 0x{hresult:08X})")
                }
            }
            EngineError::DeviceLost { message, hresult } => {
                if message.is_empty() {
                    write!(
                        f,
                        "the output device was removed or is in use elsewhere (hresult 0x{hresult:08X})"
                    )
                } else {
                    write!(f, "{message} (hresult 0x{hresult:08X})")
                }
            }
            EngineError::NotUtf8 => write!(f, "engine returned invalid UTF-8"),
            EngineError::NullPointer => write!(f, "engine returned a null pointer"),
            EngineError::BadPayload(why) => write!(f, "unusable engine payload: {why}"),
            EngineError::LoadFailed(why) => write!(f, "could not load nae.dll: {why}"),
        }
    }
}

impl std::error::Error for EngineError {}

/// Human-readable name for a `NAE_*` code, kept in sync with `NAE_GetLastError`.
fn describe(code: i32) -> &'static str {
    match code {
        ffi::NAE_OK => "success",
        ffi::NAE_ERR_COM => "COM could not be initialised on this thread",
        ffi::NAE_ERR_NO_DEVICE => "no such render endpoint",
        ffi::NAE_ERR_NO_NATIVE_FORMAT => "endpoint did not report a usable native format",
        ffi::NAE_ERR_ACTIVATE => "the audio client could not be created",
        ffi::NAE_ERR_INITIALIZE => "the stream could not be initialised at the requested format",
        ffi::NAE_ERR_NO_MEMORY => "the engine ran out of memory",
        ffi::NAE_ERR_NOT_OPEN => "the stream is not open",
        ffi::NAE_ERR_INVALID_ARGUMENT => "invalid argument",
        ffi::NAE_ERR_CANNOT_OPEN => "the file could not be read",
        ffi::NAE_ERR_DEVICE_LOST => "the output device was removed or is in use elsewhere",
        _ => "unknown engine error",
    }
}

pub type Result<T> = std::result::Result<T, EngineError>;
