//! iOS audio session ownership.
//!
//! The host, not the core, owns the session: it sets the category and
//! activates it before the engine opens its render unit, per
//! `docs/ios/core-contract.md` section 4. It also translates the system's
//! interruptions into playback commands, because an interruption is a session
//! event -- the engine never sees it as an error of its own.
//!
//! The decision table lives here without any Apple types, so it is tested on
//! the machine that builds the app rather than only on a device.

/// What the system did to the session.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Interruption {
    /// Another app or the system took the audio; the engine must stop
    /// rendering.
    Began,
    /// The other audio is gone. `should_resume` is the system's hint that
    /// playback may continue where it stopped: a hint, not an instruction.
    Ended { should_resume: bool },
}

/// The format iOS granted the session, in the terms the engine's reinitialize
/// takes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Granted {
    pub sample_rate: u32,
    pub channels: u16,
}

/// One thing the host does about an interruption. The order of a slice is the
/// order it is done in: a resume against a session iOS has deactivated renders
/// nothing, so the session is activated first, and the engine rebuilds its
/// output side before the resume reaches it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    Pause,
    Activate,
    Reinitialize,
    Resume,
}

/// What an interruption beginning implies. Pausing a player that is not
/// playing would turn "stopped" into "paused" and make the interruption
/// visible in the UI as a state change the user never caused.
pub fn begun_steps(playing: bool) -> &'static [Step] {
    if playing {
        &[Step::Pause]
    } else {
        &[]
    }
}

/// What an interruption ending implies. Without `should_resume` the system is
/// saying the user moved on -- another player keeps the audio -- so the track
/// stays paused even though it was playing before the interruption.
pub fn ended_steps(should_resume: bool, was_playing: bool) -> &'static [Step] {
    if should_resume && was_playing {
        &[Step::Activate, Step::Reinitialize, Step::Resume]
    } else {
        &[]
    }
}

#[cfg(target_os = "ios")]
mod platform {
    use super::{Granted, Interruption};

    use std::ptr::NonNull;
    use std::sync::{Arc, Mutex, Once};

    use block2::RcBlock;
    use objc2_avf_audio::{
        AVAudioSession, AVAudioSessionCategoryOptions, AVAudioSessionCategoryPlayback,
        AVAudioSessionInterruptionNotification, AVAudioSessionInterruptionOptionKey,
        AVAudioSessionInterruptionOptions, AVAudioSessionInterruptionType,
        AVAudioSessionInterruptionTypeKey, AVAudioSessionModeDefault,
    };
    use objc2_foundation::{NSNotification, NSNotificationCenter, NSNumber, NSString};

    /// What `install` was given. A later install replaces it: Base can be
    /// constructed again (a hot update does), and the notification handler
    /// must then command the new engine, not the one it replaced.
    type Handler = dyn Fn(Interruption) + Send + Sync;

    static HANDLER: Mutex<Option<Arc<Handler>>> = Mutex::new(None);
    static REGISTER: Once = Once::new();

    /// Activates the playback category and reports the format the session
    /// grants. Calling this again is valid: the same category and activation
    /// are idempotent. iOS also deactivates the session when an interruption
    /// begins, so resuming after one without calling this again would render
    /// into a session the system has turned off.
    pub fn activate() -> Result<Granted, String> {
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
                .setCategory_mode_options_error(category, &mode, AVAudioSessionCategoryOptions(0))
                .map_err(|error| format!("{error}"))?;
            session
                .setActive_error(true)
                .map_err(|error| format!("{error}"))?;
            Ok(Granted {
                sample_rate: session.sampleRate().max(0.0) as u32,
                channels: session.outputNumberOfChannels().clamp(0, u16::MAX as isize) as u16,
            })
        }
    }

    /// Routes every interruption to `handler` from now on.
    ///
    /// The observer is registered once per process and never removed: there is
    /// one shared session, the app runs until it is killed, and a handler that
    /// outlives Base is harmless because it only reaches the engine through
    /// the shared handle Base left behind.
    pub fn install<F>(handler: F) -> Result<(), String>
    where
        F: Fn(Interruption) + Send + Sync + 'static,
    {
        let mut slot = HANDLER
            .lock()
            .map_err(|_| "E_AUDIO: 中断处理器锁已中毒".to_owned())?;
        *slot = Some(Arc::new(handler));
        drop(slot);
        REGISTER.call_once(register);
        Ok(())
    }

    fn register() {
        // Safety: the notification center and the avf-audio constants are
        // process-wide objects that iOS defines before `main` runs; the block
        // only reads them.
        unsafe {
            let Some(name) = AVAudioSessionInterruptionNotification else {
                return;
            };
            let block = RcBlock::new(|notification: NonNull<NSNotification>| {
                // Nothing here may panic: a panic across the Objective-C
                // frame is undefined behaviour, and a notification with an
                // unexpected shape is not worth terminating the app over.
                let Some(event) = parse(notification.as_ref()) else {
                    return;
                };
                let handler = HANDLER.lock().ok().and_then(|slot| slot.clone());
                if let Some(handler) = handler {
                    handler(event);
                }
            });
            // The returned observer is deliberately dropped: the center keeps
            // the block and the observation for the life of the process, and
            // keeping the token would make this module own a non-Send object
            // only to hand it back at shutdown for no gain.
            let _observer = NSNotificationCenter::defaultCenter()
                .addObserverForName_object_queue_usingBlock(Some(name), None, None, &block);
        }
    }

    /// Reads the notification's userInfo. Missing keys, or a type iOS has not
    /// documented, mean "ignore": the alternative is guessing at playback.
    unsafe fn parse(notification: &NSNotification) -> Option<Interruption> {
        let info = notification.userInfo()?;
        let number = |key: &NSString| -> Option<u64> {
            let value = info.objectForKey(key)?;
            let number = value.downcast::<NSNumber>().ok()?;
            Some(number.unsignedIntegerValue() as u64)
        };

        let kind = number(AVAudioSessionInterruptionTypeKey?)?;
        if kind == AVAudioSessionInterruptionType::Began.0 as u64 {
            return Some(Interruption::Began);
        }
        if kind != AVAudioSessionInterruptionType::Ended.0 as u64 {
            return None;
        }
        // The option key is absent when the interruption ended for a reason
        // that does not suggest resuming (Siri took the audio, the user
        // started music elsewhere).
        let should_resume = AVAudioSessionInterruptionOptionKey
            .and_then(|key| number(key))
            .map(|options| {
                AVAudioSessionInterruptionOptions::from_bits_retain(options as usize)
                    .contains(AVAudioSessionInterruptionOptions::ShouldResume)
            })
            .unwrap_or(false);
        Some(Interruption::Ended { should_resume })
    }
}

#[cfg(target_os = "ios")]
pub use platform::{activate, install};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_interruption_begins_only_pauses_audio_that_was_playing() {
        assert_eq!(begun_steps(true), &[Step::Pause]);
        assert_eq!(begun_steps(false), &[] as &[Step]);
    }

    #[test]
    fn an_interruption_ends_with_activation_before_the_resume() {
        assert_eq!(
            ended_steps(true, true),
            &[Step::Activate, Step::Reinitialize, Step::Resume],
            "iOS deactivates the session when the interruption begins, and the \
             engine has to rebuild its output side before it is resumed"
        );
    }

    #[test]
    fn an_interruption_that_says_not_to_resume_does_nothing() {
        assert_eq!(ended_steps(false, true), &[] as &[Step]);
    }

    #[test]
    fn an_interruption_ending_after_silence_does_not_start_playback() {
        assert_eq!(ended_steps(true, false), &[] as &[Step]);
        assert_eq!(ended_steps(false, false), &[] as &[Step]);
    }
}
