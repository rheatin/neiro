//! Rust bindings for the Neiro audio engine (`nae.dll`).
//!
//! The engine is a C++ WASAPI implementation behind a C ABI. This crate is the
//! only place Rust is allowed to touch it; everything above works with the safe
//! types in [`engine`].
//!
//! # Division of responsibility
//!
//! Audio samples never leave the DLL. Decode and resample live on the C++ side,
//! because bit-perfect output depends on the samples staying in one memory
//! domain — a hop through JavaScript or through a shared serialisable format is
//! enough to lose it. Rust owns lifecycle, device selection, queueing and IPC;
//! the frontend owns pixels.
//!
//! This paragraph used to list "EQ and the spectrum tap" as well. **Neither
//! exists.** `nae.h` exports twenty-four functions and none of them is an EQ
//! control or a spectrum read, so the sentence described two capabilities the
//! engine does not have.
//!
//! It is recorded here because of what it cost. On the exclusive route the
//! samples never enter the webview, so the player's spectrum and its tempo
//! detection have no data source at all — and the only reason that looked like
//! a frontend bug is that a doc comment said the engine had a tap. A missing
//! capability, described in a comment as a present one, is worse than a missing
//! capability: the second one gets built, and the first one gets looked for in
//! the wrong place.
//!
//! # Threading
//!
//! The engine is COM-based, and an exclusive stream belongs to the thread whose
//! apartment created it. [`engine::Stream`] is deliberately `!Send`: moving one
//! to another thread is the apartment-lifetime bug this code was written to
//! avoid. Production playback runs on a dedicated audio thread, created and torn
//! down together with the stream.

pub mod engine;
pub mod error;
pub mod ffi;
pub mod output;
pub mod session;

pub use engine::{
    analyze_file, analyze_file_cancellable, analyzer_version, capabilities, compute_peaks,
    default_device, default_device_id,
    enumerate_devices, last_error, open_exclusive, open_file, open_file_shared,
    open_file_with_share, open_with, shutdown, Analysis, Device, Envelope, LiveSignalPath, Peaks,
    SeamDistribution, ShareMode, SignalPath, Spectrum, Stream, Tags, TrackInfo,
};
pub use error::{EngineError, Result};
pub use output::{OutputMode, Quality};
pub use session::{
    CrossfadeParams, CrossfeedParams, ConvolverParams, EqBand, EqParams, ReplayGainParams,
    Session, SessionPhase, SessionSettings, SessionState, ShareKind, CROSSFADE_EQUAL_POWER,
    CROSSFADE_LINEAR, EQ_HIGH_PASS, EQ_HIGH_SHELF, EQ_LOW_PASS, EQ_LOW_SHELF, EQ_MAX_BANDS,
    EQ_NOTCH, EQ_PEAKING, EQ_PHASE_LINEAR, EQ_PHASE_MINIMUM, RG_ALBUM, RG_OFF, RG_TRACK,
};
