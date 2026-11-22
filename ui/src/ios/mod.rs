//! The host's iOS-only pieces.
//!
//! NSH's own picker hands back copies (and, for a folder, a path that is not
//! valid after the next launch). The library keeps a folder the way iOS
//! expects: `UIDocumentPickerViewController` opens it, and a security-scoped
//! bookmark keeps the app's access to it across launches.

pub mod bookmarks;
pub mod picker;
