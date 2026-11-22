//! Security-scoped bookmarks for the library's folders.
//!
//! The picker's URL grants access for this launch only, and the path it
//! carries changes with the container's UUID. A bookmark is what survives:
//! created when the folder is picked, resolved on every launch, and
//! recreated when iOS calls the stored one stale.
//!
//! The access a resolution opens is process-wide and must outlive the call:
//! the core reads the folder's files from its own threads. [`HELD`] keeps
//! each open URL until the root leaves the library, and no copy of the path
//! is persisted — only the bookmark.

use std::sync::Mutex;

use objc2::rc::Retained;
use objc2::runtime::Bool;
use objc2::AnyThread;
use objc2_foundation::{
    NSData, NSDataBase64DecodingOptions, NSDataBase64EncodingOptions, NSString, NSURL,
    NSURLBookmarkCreationOptions, NSURLBookmarkResolutionOptions,
};

/// A root resolved from its bookmark.
pub struct Resolved {
    /// The folder's absolute path now. In memory only: it changes with the
    /// app container's UUID between launches.
    pub path: String,
    /// The bookmark to store: the stored one, or a fresh one when iOS said
    /// it was stale.
    pub bookmark: String,
    /// Whether the stored bookmark was stale (the path was still usable).
    pub stale: bool,
}

/// One open access: the URL, and whether this module started it (stopping
/// one that was already open, or that iOS refused, would unbalance the
/// system's count).
struct Open {
    bookmark: String,
    url: Retained<NSURL>,
    started: bool,
}

static HELD: Mutex<Vec<Open>> = Mutex::new(Vec::new());

/// Makes the bookmark of `url`, a folder the picker just returned, as the
/// base64 text `Storage` keeps. On iOS, `MinimalBookmark` is the
/// security-scoped one; `WithSecurityScope` is a macOS-only flag.
pub fn create(url: &NSURL) -> Result<String, String> {
    // SAFETY: the URL of a picked folder, read only.
    let data = url
        .bookmarkDataWithOptions_includingResourceValuesForKeys_relativeToURL_error(
            NSURLBookmarkCreationOptions::MinimalBookmark,
            None,
            None,
        )
        .map_err(|error| format!("E_BOOKMARK: 书签没有建立：{error}"))?;
    let text = data.base64EncodedStringWithOptions(NSDataBase64EncodingOptions(0));
    Ok(text.to_string())
}

/// Resolves `bookmark` to the folder it names and opens the app's access to
/// it. A stale bookmark is recreated and returned in its place; one that no
/// longer resolves is an error (the caller asks the user to pick again).
pub fn resolve(bookmark: &str) -> Result<Resolved, String> {
    let string = NSString::from_str(bookmark);
    // SAFETY: alloc + the documented initializer; `None` when the text is no
    // base64 data.
    let data = NSData::initWithBase64EncodedString_options(
        NSData::alloc(),
        &string,
        NSDataBase64DecodingOptions(0),
    )
    .ok_or_else(|| "E_BOOKMARK: 书签不是 base64 数据".to_owned())?;
    let mut stale = Bool::NO;
    // SAFETY: the bookmark data this module wrote; `stale` is a valid slot.
    let url = unsafe {
        NSURL::URLByResolvingBookmarkData_options_relativeToURL_bookmarkDataIsStale_error(
            &data,
            NSURLBookmarkResolutionOptions(0),
            None,
            &mut stale,
        )
    }
    .map_err(|error| format!("E_BOOKMARK: 书签无法解析：{error}"))?;
    let path = url
        .path()
        .ok_or_else(|| "E_BOOKMARK: 书签没有路径".to_owned())?
        .to_string();
    hold(&url, bookmark);
    let mut stored = bookmark.to_owned();
    if stale.as_bool() {
        stored = create(&url)?;
        // The old key keeps the open access; the new bookmark is what
        // Storage has from now on, so releasing it must find the entry.
        rekey(bookmark, &stored);
    }
    Ok(Resolved {
        path,
        bookmark: stored,
        stale: stale.as_bool(),
    })
}

/// Stops the access `resolve` opened for `bookmark` (a root that left the
/// library). Releasing one that is not held is not an error: a hot update
/// may resolve a bookmark it never saw.
pub fn release(bookmark: &str) {
    let mut held = HELD.lock().unwrap_or_else(|poison| poison.into_inner());
    if let Some(index) = held.iter().position(|open| open.bookmark == bookmark) {
        let open = held.remove(index);
        if open.started {
            // SAFETY: balanced with the start in `hold`.
            unsafe { open.url.stopAccessingSecurityScopedResource() };
        }
    }
}

/// Opens the app's access to `url`, once per bookmark.
fn hold(url: &NSURL, bookmark: &str) {
    let mut held = HELD.lock().unwrap_or_else(|poison| poison.into_inner());
    if held.iter().any(|open| open.bookmark == bookmark) {
        return;
    }
    // SAFETY: a URL from the picker or a resolved bookmark; iOS pairs the
    // call with `stopAccessingSecurityScopedResource` in `release`.
    let started = unsafe { url.startAccessingSecurityScopedResource() };
    if !started {
        // The app's own container (or one iOS already opened) is readable
        // without a scope; the URL is still kept for a balanced release.
        eprintln!("neiro: 安全作用域没有打开：{bookmark}");
    }
    held.push(Open {
        bookmark: bookmark.to_owned(),
        url: url.into(),
        started,
    });
}

fn rekey(old: &str, new: &str) {
    let mut held = HELD.lock().unwrap_or_else(|poison| poison.into_inner());
    if let Some(index) = held.iter().position(|open| open.bookmark == old) {
        held[index].bookmark = new.to_owned();
    }
}
