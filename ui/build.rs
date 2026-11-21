//! Links the iOS core artifact.
//!
//! Desktop does nothing here: its core is opened at run time by
//! `engine/desktop.rs`. On iOS the core is part of the binary, so this script
//! selects the xcframework slice from the Rust target and emits the link
//! flags. The slice layout and the symbol set are normative in
//! `docs/ios/core-contract.md`.
//!
//! A missing artifact fails this build with a message that names it. A
//! warning is swallowed by `nsh`, and the alternative — a link failure later
//! — reports symbols instead of the cause.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=NEIRO_BASE_IOS_XC");

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("ios") {
        return;
    }

    let target = std::env::var("TARGET").unwrap_or_default();
    // Cargo's iOS targets end in `-ios` (device) or `-ios-sim` (simulator);
    // the xcframework names the slices by platform, not by Rust triple.
    let slice = if target.ends_with("-ios-sim") {
        "ios-arm64-simulator"
    } else {
        "ios-arm64"
    };

    let xcframework = std::env::var_os("NEIRO_BASE_IOS_XC")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("vendor")
                .join("neiro_base.xcframework")
        });
    println!("cargo:rerun-if-changed={}", xcframework.display());

    let slice_dir = xcframework.join(slice);
    let core = slice_dir.join("libneiro_base.a");
    if !core.is_file() {
        panic!(
            "neiro-ui: iOS core artifact missing at {}. Run scripts/fetch-core-ios.sh, \
             or point NEIRO_BASE_IOS_XC at a xcframework that contains a {} slice.",
            core.display(),
            slice
        );
    }

    println!("cargo:rustc-link-search=native={}", slice_dir.display());
    println!("cargo:rustc-link-lib=static=neiro_base");

    // FFmpeg ships beside the engine as a dynamic framework, for the LGPL
    // reason in the contract. A slice without it links only the engine.
    if slice_dir.join("FFmpeg.framework").is_dir() {
        println!("cargo:rustc-link-lib=framework=FFmpeg");
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path/Frameworks");
    }
}
