//! The host's iOS-only pieces.
//!
//! NSH's own picker hands back copies (and, for a folder, a path that is not
//! valid after the next launch). The library keeps a folder the way iOS
//! expects: `UIDocumentPickerViewController` opens it, and a security-scoped
//! bookmark keeps the app's access to it across launches.
//!
//! Two more things only iOS needs live here: the sandbox home (the app's
//! container, which the desktop branch of `data_root()` does not know about)
//! and the file protection class on the host's own data.

use std::path::Path;

use objc2::runtime::AnyObject;
use objc2_foundation::{
    NSFileProtectionCompleteUnlessOpen, NSHomeDirectory, NSString, NSURL, NSURLFileProtectionKey,
};

pub mod bookmarks;
pub mod keychain;
pub mod picker;

/// The app container's home. `NSHomeDirectory()` is the sandbox root; the
/// `HOME` environment variable is not guaranteed on a device.
pub fn home() -> std::path::PathBuf {
    std::path::PathBuf::from(NSHomeDirectory().to_string())
}

/// Applies `CompleteUnlessOpen` to `path`. The library database is opened at
/// launch, so a playback that is already running keeps reading it while the
/// device is locked; a handle that is not open is sealed until the next
/// unlock. Files created later inside a protected folder inherit the class.
pub fn protect(path: &Path) {
    if !path.exists() {
        return;
    }
    let text = NSString::from_str(&path.to_string_lossy());
    let url = NSURL::fileURLWithPath(&text);
    // SAFETY: both are framework constants; setting a resource value on a
    // file URL the app owns is the documented call.
    let result = unsafe {
        let value: Option<&AnyObject> = Some(NSFileProtectionCompleteUnlessOpen);
        url.setResourceValue_forKey_error(value, NSURLFileProtectionKey)
    };
    if let Err(error) = result {
        // Not fatal: the launch continues, and the data is merely at the
        // system's default protection class.
        eprintln!("neiro: 无法设置文件保护（{}）：{error}", path.display());
    }
}
