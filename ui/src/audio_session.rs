//! iOS audio session ownership: the host sets category and activation before
//! the core opens its render unit, per `docs/ios/core-contract.md` section 4.

use objc2_avf_audio::{
    AVAudioSession, AVAudioSessionCategoryOptions, AVAudioSessionCategoryPlayback,
    AVAudioSessionModeDefault,
};

/// Activates the playback category and reports the granted output format.
/// Calling this again is valid: the same category and activation are idempotent.
pub fn activate() -> Result<String, String> {
    let session = unsafe { AVAudioSession::sharedInstance() };
    let (category, mode) = unsafe {
        (
            AVAudioSessionCategoryPlayback
                .expect("AVAudioSessionCategoryPlayback is defined by iOS"),
            AVAudioSessionModeDefault.expect("AVAudioSessionModeDefault is defined by iOS"),
        )
    };
    unsafe {
        session
            .setCategory_mode_options_error(
                category,
                &mode,
                AVAudioSessionCategoryOptions(0),
            )
            .map_err(|error| format!("{error}"))?;
        session
            .setActive_error(true)
            .map_err(|error| format!("{error}"))?;
        let sample_rate = session.sampleRate();
        let channels = session.outputNumberOfChannels();
        Ok(format!(
            "AVAudioSession active: sample_rate={sample_rate:.0}, channels={channels}"
        ))
    }
}
