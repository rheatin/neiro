//! Check tag read/write against real files.
//!
//! The write path is the one thing here that can destroy a listener's audio, so
//! this probe measures consequences rather than a call having returned:
//!
//!   * the samples are decoded before and after and compared byte for byte;
//!   * the text that went in is read back out, including text ID3v1 cannot hold;
//!   * the temp file the write goes through is gone afterwards;
//!   * a policy naming a family the container cannot hold is refused before
//!     anything is touched.
//!
//! Usage: `tag_check <file> [<file> ...]`, each one an existing audio file that
//! this probe may modify. Copy the fixtures first if the originals matter.

use std::collections::BTreeSet;
use std::path::Path;
use std::process::Command;

use lofty::tag::ItemKey;

use neiro_base::tag::{
    read_tags, writable_families, writable_keys, write_tags, Id3v2Encoding, TagError, TagFamily,
    TagField, TagPolicy,
};

fn fail(why: &str) -> ! {
    eprintln!("FAIL: {why}");
    std::process::exit(1);
}

fn ok(what: &str) {
    println!("  ok   {what}");
}

/// A digest of the decoded samples. Any change the tag write made to the audio
/// moves it, which is the only thing worth asserting about the samples.
fn pcm_digest(path: &Path) -> String {
    let out = Command::new("ffmpeg")
        .args([
            "-v", "error", "-i", &path.to_string_lossy(),
            "-f", "s16le", "-ac", "2", "-ar", "48000", "-",
        ])
        .output()
        .unwrap_or_else(|e| fail(&format!("ffmpeg: {e}")));
    if !out.status.success() {
        fail(&format!("could not decode {}", path.display()));
    }
    if out.stdout.is_empty() {
        fail(&format!("decoded no samples from {}", path.display()));
    }
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in &out.stdout {
        h ^= *b as u64;
        h = h.wrapping_mul(0x1000_0000_01b3);
    }
    format!("{h:016x}/{}", out.stdout.len())
}

/// Leftovers from a crashed run, and from this one if cleanup failed.
fn assert_no_temp_files(dir: &Path) {
    for entry in std::fs::read_dir(dir).unwrap_or_else(|e| fail(&format!("read_dir: {e}"))) {
        let name = entry.unwrap().file_name().to_string_lossy().into_owned();
        if name.contains(".neiro-tmp-") {
            fail(&format!("temp file left behind: {name}"));
        }
    }
}

fn main() {
    let paths: Vec<String> = std::env::args().skip(1).collect();
    if paths.is_empty() {
        eprintln!("usage: tag_check <file> [<file> ...]");
        std::process::exit(2);
    }

    let fields = [
        TagField::text(ItemKey::TrackTitle, "你的背包"),
        TagField::text(ItemKey::TrackArtist, "陈奕迅"),
        TagField::text(ItemKey::AlbumTitle, "Special Thanks To..."),
        TagField::text(ItemKey::Year, "2003"),
    ];

    for path in &paths {
        let p = Path::new(path);
        println!("\n=== {path} ===");

        let before = read_tags(p).unwrap_or_else(|e| fail(&format!("read: {e}")));
        println!("  families before: {:?}", before.families);

        let writable = writable_families(p).unwrap_or_else(|e| fail(&format!("{e}")));
        let names: Vec<&str> = writable.iter().map(|f| f.as_str()).collect();
        println!("  writable: {names:?}");
        if writable.is_empty() {
            fail("no writable family");
        }
        for family in &writable {
            let keys = writable_keys(*family);
            println!("    {}: {} fields", family.as_str(), keys.len());
            if keys.is_empty() {
                fail(&format!("{} offers no fields", family.as_str()));
            }
        }

        // A policy the container cannot satisfy is refused, not silently
        // dropped: reporting success for a write that never happened is the
        // failure this project treats as worse than an error.
        let impossible = writable
            .iter()
            .find(|f| **f != TagFamily::Id3v2)
            .copied()
            .unwrap_or(TagFamily::Id3v1);
        let mut refused = TagPolicy::default();
        refused.write = BTreeSet::from([match impossible {
            TagFamily::VorbisComments => TagFamily::Id3v1,
            _ => TagFamily::VorbisComments,
        }]);
        match write_tags(p, &fields, &refused) {
            Err(TagError::FamilyNotSupported { .. }) => ok("refused an impossible family"),
            Err(e) => println!("  ..   refused with a different error: {e}"),
            Ok(()) => fail("accepted a policy the container cannot satisfy"),
        }

        let pcm_before = pcm_digest(p);

        // Write into every family the container can take, so one run exercises
        // the whole matrix rather than the first entry.
        let mut policy = TagPolicy::default();
        policy.write = writable.iter().copied().collect();
        policy.id3v2 = Id3v2Encoding::Utf8;
        write_tags(p, &fields, &policy).unwrap_or_else(|e| fail(&format!("write: {e}")));
        ok("write returned");

        let pcm_after = pcm_digest(p);
        if pcm_before != pcm_after {
            fail(&format!("audio changed: {pcm_before} -> {pcm_after}"));
        }
        ok(&format!("audio unchanged ({pcm_after})"));

        let after = read_tags(p).unwrap_or_else(|e| fail(&format!("reread: {e}")));
        println!("  families after: {:?}", after.families);
        for (key, want) in [
            (ItemKey::TrackTitle, "你的背包"),
            (ItemKey::TrackArtist, "陈奕迅"),
            (ItemKey::AlbumTitle, "Special Thanks To..."),
        ] {
            match after.get_text(key) {
                Some(got) if got == want => ok(&format!("{key:?} = {got:?}")),
                Some(got) => fail(&format!("{key:?} = {got:?}, wanted {want:?}")),
                None => fail(&format!("{key:?} missing after write")),
            }
        }

        if let Some(dir) = p.parent() {
            assert_no_temp_files(if dir.as_os_str().is_empty() { Path::new(".") } else { dir });
            ok("no temp file left behind");
        }
    }

    println!("\nTAG CHECKS PASSED ({} file(s))", paths.len());
}
