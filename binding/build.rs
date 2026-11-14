// Tells cargo where to find the engine's shared library, and — more importantly
// — when the C++ side changed, so that editing nae.cpp forces a rebuild here too.
//
// Why an import library at all: linking against `libnae.dll.a` makes the Rust
// linker resolve the NAE_* symbols at *compile* time. If the C++ side drops or
// renames an export, this crate stops building instead of failing at runtime
// with an unresolved-procedure error. That check is worth the small extra setup.
//
// On macOS the same role is played by the dylib itself: there is no import
// library, and `cargo:rustc-link-lib=dylib=nae` resolves against `libnae.dylib`
// in the search path. The two paths differ in three places — the build
// directory, the artifact the linker consumes, and the artifact the loader
// needs beside the executable — and nowhere else.

use std::path::{Path, PathBuf};

/// The engine's build directory for this target.
///
/// The CMake presets put Windows in `engine/build` and macOS in
/// `engine/build-macos`; a script that assumed the first would look for a
/// Windows artifact on a Mac and report a missing import library as if the
/// engine had not been built at all.
fn build_dir() -> PathBuf {
    let engine = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("crate lives inside the repository")
        .join("engine");
    if cfg!(target_os = "windows") {
        engine.join("build")
    } else {
        engine.join("build-macos")
    }
}

/// The file the linker needs, which is not the file the loader needs.
fn link_artifact(build_dir: &Path) -> PathBuf {
    if cfg!(target_os = "windows") {
        build_dir.join("libnae.dll.a")
    } else {
        build_dir.join("libnae.dylib")
    }
}

/// The file that has to sit beside the executable at run time.
fn runtime_artifact(build_dir: &Path) -> PathBuf {
    if cfg!(target_os = "windows") {
        build_dir.join("nae.dll")
    } else {
        build_dir.join("libnae.dylib")
    }
}

fn main() {
    let build_dir = build_dir();

    let link = link_artifact(&build_dir);
    if !link.exists() {
        panic!(
            "the engine's link artifact is not at {}\n\
             Build the engine first, from engine/:\n  \
             cmake --preset {} && cmake --build --preset {}\n\
             (Windows needs MinGW on PATH; macOS needs Xcode and cmake)",
            link.display(),
            if cfg!(target_os = "windows") { "windows" } else { "macos" },
            if cfg!(target_os = "windows") { "windows" } else { "macos" },
        );
    }

    println!("cargo:rustc-link-search=native={}", build_dir.display());
    // The import library is called libnae.dll.a but the DLL it resolves against
    // is nae.dll, so the -l name has to be given explicitly. On macOS the file
    // is already named for the -l flag.
    println!("cargo:rustc-link-lib=dylib=nae");

    // macOS: `libnae.dylib` has an install name of `@rpath/libnae.dylib`, and a
    // crate that links it gets no `LC_RPATH` of its own. Without this the
    // dylib loads nothing beside the engine's build directory — the host
    // reports `Library not loaded: @rpath/libnae.dylib, no LC_RPATH's found`,
    // which reads like a missing file rather than a missing search path. The
    // packaged app rewrites its own copies to `@loader_path`; this is the
    // build-tree case.
    if cfg!(target_os = "macos") {
        println!(
            "cargo:rustc-link-arg=-Wl,-rpath,{}",
            build_dir.display()
        );
    }

    // Put the freshly built library next to the executable that will load it.
    //
    // Without this, the copy in the application's target directory is whatever
    // someone copied there last, and nothing notices when the engine moves on. The
    // failure is not a compile error and not a message: the host links against the
    // *new* import library, so it references an export the *old* DLL does not
    // have, and Windows terminates the process at load time with
    // STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139). No output, no log line, no window
    // — an application that simply does not start, after a change that compiled
    // cleanly. That is how an hour goes into looking at the wrong layer.
    //
    // The destination is derived from OUT_DIR rather than passed in, because this
    // script cannot know the consuming crate's layout and asking for it would be a
    // setting someone has to remember to update. OUT_DIR is
    // <target>/<profile>/build/<crate>-<hash>/out, so three levels up is
    // <target>/<profile>, which is where the executable and its DLLs live.
    if let Ok(out_dir) = std::env::var("OUT_DIR") {
        let profile_dir = PathBuf::from(&out_dir)
            .ancestors()
            .nth(3)
            .map(Path::to_path_buf);
        let built = runtime_artifact(&build_dir);
        if let Some(profile_dir) = profile_dir {
            if built.exists() {
                let dest = profile_dir.join(built.file_name().expect("a file name"));
                let _ = std::fs::copy(&built, &dest);
            }
        }
    }

    // Rebuild this crate whenever the C++ engine or its exported surface change.
    let engine_dir = build_dir.parent().expect("engine dir");
    for file in ["nae.h", "nae.cpp", "nae.map", "nae.def"] {
        println!("cargo:rerun-if-changed={}", engine_dir.join(file).display());
    }
    // The built library, as an input to this script rather than only as its output.
    //
    // Without this the copy above is a one-shot. Declaring only sources means a
    // change that alters the DLL without altering a source -- editing the version
    // script and relinking, which is exactly how a new export gets added -- does
    // not rerun this script, so the host keeps loading the previous DLL while
    // linking against the new import library. The result is
    // STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) with no message, which is the
    // failure this script's own comment above describes and then does not
    // prevent.
    let built = runtime_artifact(&build_dir);
    if built.exists() {
        println!("cargo:rerun-if-changed={}", built.display());
    }
}
