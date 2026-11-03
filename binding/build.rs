// Tells cargo where to find nae.dll's import library, and — more importantly —
// when the C++ side changed, so that editing nae.cpp forces a rebuild here too.
//
// Why an import library at all: linking against `libnae.dll.a` makes the Rust
// linker resolve the nine NAE_* symbols at *compile* time. If the C++ side
// drops or renames an export, this crate stops building instead of failing at
// runtime with an unresolved-procedure error. That check is worth the small
// extra setup.

use std::path::{Path, PathBuf};

fn main() {
    // <repo>/engine/build: this crate lives at <repo>/binding, and the C++
    // sources and their build directory are one level across in engine/.
    let build_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("crate lives inside the repository")
        .join("engine")
        .join("build");

    let import_lib = build_dir.join("libnae.dll.a");
    if !import_lib.exists() {
        panic!(
            "nae.dll's import library not found at {}\n\
             Build the engine first:\n  \
             cmake -B build -G Ninja && cmake --build build\n\
             (from engine, with MinGW on PATH)",
            import_lib.display()
        );
    }

    println!("cargo:rustc-link-search=native={}", build_dir.display());
    // The import library is called libnae.dll.a but the DLL it resolves against
    // is nae.dll, so the -l name has to be given explicitly.
    println!("cargo:rustc-link-lib=dylib=nae");

    // Put the freshly built DLL next to the executable that will load it.
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
        if let Some(profile_dir) = profile_dir {
            let dll = build_dir.join("nae.dll");
            if dll.exists() {
                let dest = profile_dir.join("nae.dll");
                // Only copy when it differs, so an unchanged engine does not
                // rewrite the file and churn the next link.
                let needs_copy = match (dll.metadata(), dest.metadata()) {
                    (Ok(a), Ok(b)) => a.len() != b.len() || a.modified().ok() != b.modified().ok(),
                    _ => true,
                };
                if needs_copy {
                    if let Err(why) = std::fs::copy(&dll, &dest) {
                        println!(
                            "cargo:warning=could not copy {} to {}: {why}",
                            dll.display(),
                            dest.display()
                        );
                    }
                }
            }
        }
    }

    // Rebuild this crate whenever the C++ engine or its exported surface change.
    for file in ["nae.h", "nae.cpp", "nae.map", "nae.def"] {
        let path = build_dir.parent().expect("engine dir").join(file);
        println!("cargo:rerun-if-changed={}", path.display());
    }
    // The built DLL, as an input to this script rather than only as its output.
    //
    // Without this the copy above is a one-shot. Declaring only sources means a
    // change that alters the DLL without altering a source -- editing the version
    // script and relinking, which is exactly how a new export gets added -- does
    // not rerun this script, so the host keeps loading the previous DLL while
    // linking against the new import library. The result is
    // STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) with no message, which is the
    // failure this script's own comment above describes and then does not
    // prevent.
    let built_dll = build_dir.join("nae.dll");
    if built_dll.exists() {
        println!("cargo:rerun-if-changed={}", built_dll.display());
    }
}
