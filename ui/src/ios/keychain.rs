//! The iOS Keychain: where a remote server's secret lives.
//!
//! The roster in `Storage` (`remote:servers`) keeps only what the interface
//! shows — the label, the URL, the user name and whether a secret was saved.
//! The secret itself (the Jellyfin password or API token, the Navidrome or
//! WebDAV password) is one generic-password item per server: service
//! `app.neiro.ui.remote`, account `remote:secret:{server}`.
//!
//! Accessibility is `AfterFirstUnlock`: playback that is already running
//! continues while the device is locked, and a sign-in to a server is only
//! attempted after the first unlock since boot anyway.
//!
//! A restore that does not bring the Keychain along (a different device)
//! leaves the roster's `hasSecret` pointing at nothing; [`get`] answering
//! `None` is what sends the user back to the dialog to re-enter it.

use std::ptr;

use objc2_core_foundation::{
    kCFBooleanTrue, CFBoolean, CFData, CFDictionary, CFRetained, CFString, CFType,
};
use objc2_security::{
    errSecItemNotFound, errSecSuccess, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock,
    kSecAttrAccount, kSecAttrService, kSecClass, kSecClassGenericPassword, kSecMatchLimit,
    kSecMatchLimitOne, kSecReturnData, kSecValueData, SecItemAdd, SecItemCopyMatching,
    SecItemDelete, SecItemUpdate,
};

/// The Keychain service all of this app's remote items share.
const SERVICE: &str = "app.neiro.ui.remote";

/// One server's account name. The server id is the roster's id, made when the
/// row is created; it never contains a secret.
fn account(server: &str) -> String {
    format!("remote:secret:{server}")
}

// CoreFoundation's dictionaries are typed in Rust but not in the C calls, so
// these three turn a concrete CF value into the `&CFType` slot it occupies.

fn cf(value: &CFString) -> &CFType {
    value
}

fn cfd(value: &CFData) -> &CFType {
    value
}

fn cfb(value: &CFBoolean) -> &CFType {
    value
}

/// The class + service + account triple that identifies one item.
fn item(server: &str) -> (CFRetained<CFString>, CFRetained<CFString>) {
    (CFString::from_str(SERVICE), CFString::from_str(&account(server)))
}

/// The secret saved for `server`, or `None` when there is none (or the item
/// is unreadable right now, which the caller treats the same: ask again).
pub fn get(server: &str) -> Option<String> {
    let (service, account) = item(server);
    // SAFETY: the Security/RunLoop constants are immutable process statics
    // published by the framework; reading them is the SDK's documented use.
    let truth = unsafe { kCFBooleanTrue }?;
    let keys: [&CFString; 5] = unsafe {
        [kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit]
    };
    let values: [&CFType; 5] = unsafe {
        [
            cf(kSecClassGenericPassword),
            cf(&service),
            cf(&account),
            cfb(truth),
            cf(kSecMatchLimitOne),
        ]
    };
    let query = CFDictionary::<CFString, CFType>::from_slices(&keys, &values);

    let mut result: *const CFType = ptr::null();
    // SAFETY: the dictionary holds CoreFoundation keys and values, and the
    // result slot is a living local; a +1 object comes back on success.
    let status = unsafe { SecItemCopyMatching(query.as_ref(), &mut result) };
    if status != errSecSuccess || result.is_null() {
        return None;
    }
    // SAFETY: `result` is the +1 CFData the call returned.
    let retained = unsafe { CFRetained::<CFType>::from_raw(std::ptr::NonNull::new_unchecked(result as *mut CFType)) };
    // SAFETY: a generic-password item with `kSecReturnData` answers data;
    // the dictionary is released only after the text is copied out.
    let data = unsafe { CFRetained::cast_unchecked::<CFData>(retained) };
    // SAFETY: the CFData is immutable while this borrow lives.
    let bytes = unsafe { data.as_bytes_unchecked() };
    std::str::from_utf8(bytes).ok().map(str::to_owned)
}

/// Saves `value` as `server`'s secret, replacing whatever was there.
pub fn set(server: &str, value: &str) -> Result<(), String> {
    let data = CFData::from_bytes(value.as_bytes());
    let (service, account) = item(server);

    // Update first: the usual case is a saved server whose credential changed.
    let changed_keys: [&CFString; 1] = unsafe { [kSecValueData] };
    let changed_values: [&CFType; 1] = [cfd(&data)];
    let changed = CFDictionary::<CFString, CFType>::from_slices(&changed_keys, &changed_values);
    let query_keys: [&CFString; 3] = unsafe { [kSecClass, kSecAttrService, kSecAttrAccount] };
    let query_values: [&CFType; 3] = unsafe {
        [
            cf(kSecClassGenericPassword),
            cf(&service),
            cf(&account),
        ]
    };
    let query = CFDictionary::<CFString, CFType>::from_slices(&query_keys, &query_values);
    // SAFETY: both dictionaries hold CoreFoundation types.
    let updated = unsafe { SecItemUpdate(query.as_ref(), changed.as_ref()) };
    if updated == errSecSuccess {
        return Ok(());
    }
    if updated != errSecItemNotFound {
        return Err(status_message("更新", updated));
    }

    // No item yet: add one with the accessibility class the app wants.
    let add_keys: [&CFString; 5] = unsafe {
        [
            kSecClass,
            kSecAttrService,
            kSecAttrAccount,
            kSecValueData,
            kSecAttrAccessible,
        ]
    };
    let add_values: [&CFType; 5] = unsafe {
        [
            cf(kSecClassGenericPassword),
            cf(&service),
            cf(&account),
            cfd(&data),
            cf(kSecAttrAccessibleAfterFirstUnlock),
        ]
    };
    let item = CFDictionary::<CFString, CFType>::from_slices(&add_keys, &add_values);
    // SAFETY: as above; the result slot is not requested.
    let added = unsafe { SecItemAdd(item.as_ref(), ptr::null_mut()) };
    if added == errSecSuccess {
        Ok(())
    } else {
        Err(status_message("保存", added))
    }
}

/// Forgets `server`'s secret. A missing item is already the goal.
pub fn delete(server: &str) -> Result<(), String> {
    let (service, account) = item(server);
    let keys: [&CFString; 3] = unsafe { [kSecClass, kSecAttrService, kSecAttrAccount] };
    let values: [&CFType; 3] = unsafe {
        [
            cf(kSecClassGenericPassword),
            cf(&service),
            cf(&account),
        ]
    };
    let query = CFDictionary::<CFString, CFType>::from_slices(&keys, &values);
    // SAFETY: the dictionary holds CoreFoundation types.
    let status = unsafe { SecItemDelete(query.as_ref()) };
    if status == errSecSuccess || status == errSecItemNotFound {
        Ok(())
    } else {
        Err(status_message("删除", status))
    }
}

fn status_message(action: &str, status: i32) -> String {
    format!("E_KEYCHAIN: {action}凭据失败（OSStatus {status}）")
}
