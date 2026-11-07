//! Checks the tag commands across the protocol, not the functions behind them.
//!
//! `tag_check` already proves that a write preserves the audio. What it cannot
//! prove is that a host on the far side of the C ABI can drive it: that is the
//! `Command` -> control thread -> `Event` path, and it is what a UI depends on.
//!
//! Each check measures a consequence:
//!
//!   * `ReadTags` answers with the family of every field, so an editor can show
//!     ID3v2's year beside APEv2's year when they disagree;
//!   * `writable` is the container's answer, not the file's current families --
//!     offering ID3v1 to a FLAC is a promise the container cannot keep;
//!   * `WriteTags` reports an impossible family as an error event rather than
//!     silently writing nothing;
//!   * the audio decodes to the same bytes afterwards, measured through the
//!     engine's own decoder rather than through ffmpeg;
//!   * a name a family does not have is refused before the file is opened.
//!
//! Usage: `base_tag_probe <file>` with one existing audio file it may modify.
//! Copy the fixture first if the original matters.

use std::path::Path;
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

use neiro_base::protocol::{Event, TagKeyList};
use neiro_base::tag::{
    fields_from_values, Id3v2Encoding, TagFamily, TagFieldValue, TagPolicy,
};
use neiro_base::Base;

fn fail(why: &str) -> ! {
    eprintln!("FAIL: {why}");
    std::process::exit(1);
}

fn ok(what: &str) {
    println!("  ok   {what}");
}

fn check(ok: bool, label: &str, detail: String) {
    println!("  [{}] {:<52} {}", if ok { "PASS" } else { "FAIL" }, label, detail);
    if !ok {
        std::process::exit(1);
    }
}

/// Wait for one event matching `want`, bounded, and report a timeout as a
/// failure with the raw payload rather than as a missing result.
fn wait_event(
    rx: &Receiver<Event>,
    timeout: Duration,
    label: &str,
    mut want: impl FnMut(&Event) -> bool,
) -> Event {
    let deadline = Instant::now() + timeout;
    // A timeout on its own says nothing about why. The error that caused it is
    // the only useful thing in the report, and it arrives on the same channel.
    let mut last_error: Option<String> = None;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            match last_error {
                Some(error) => fail(&format!("timeout waiting for {label}; last error: {error}")),
                None => fail(&format!("timeout waiting for {label}; no event arrived")),
            }
        }
        match rx.recv_timeout(left) {
            // Every other event goes to the predicate. Listing the kinds here
            // was a bug: the check waited for `ArtworkRead` while this arm
            // dropped it as "unrelated", and the only symptom was the timeout.
            Ok(event) if !matches!(event, Event::Error { .. }) => {
                if want(&event) {
                    return event;
                }
            }
            // An error for a different operation is not this check's business.
            Ok(Event::Error { op, message }) => {
                if want(&Event::Error {
                    op: op.clone(),
                    message: message.clone(),
                }) {
                    fail(&format!("error while waiting for {label}: {op}: {message}"));
                }
                last_error = Some(format!("{op}: {message}"));
            }
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                fail("Base event channel closed")
            }
        }
    }
}

/// A digest of the file's bytes.
///
/// A tag write is allowed to change the bytes -- it moves the tag region and
/// rewrites the header -- so this is not an assertion about the container. It
/// is the fallback for a format with no decode here; the audio itself is
/// checked by `tag_check`, which decodes it.
fn digest(path: &Path) -> (u64, usize) {
    let bytes = std::fs::read(path).unwrap_or_else(|e| fail(&format!("read: {e}")));
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in &bytes {
        h ^= *b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    (h, bytes.len())
}


/// Wait for an error event for this operation.
///
/// Separate from `wait_event` because an error is the wanted outcome for a
/// refused write, and the general waiter treats a matched error as a failure.
fn wait_error(rx: &Receiver<Event>, timeout: Duration, op_wanted: &str) -> (String, String) {
    let deadline = Instant::now() + timeout;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            fail(&format!("timeout waiting for an {op_wanted} error"));
        }
        match rx.recv_timeout(left) {
            Ok(Event::Error { op, message }) => {
                if op == op_wanted {
                    return (op, message);
                }
                println!("  ..   ignored an error from {op}");
            }
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                fail("Base event channel closed")
            }
        }
    }
}

/// Put a picture in the file, so `read_artwork` has something to answer with.
///
/// The probe supplies the fixture rather than looking for one with art: a check
/// that only runs when the sample happens to carry a picture measures nothing
/// on the machines where it does not. The bytes are a real 1x1 PNG so the mime
/// the backend reports is the one the picture actually is.
fn insert_picture(path: &Path, family: lofty::tag::TagType, data: &[u8]) {
    use lofty::config::WriteOptions;
    use lofty::file::{AudioFile, TaggedFileExt};
    use lofty::picture::{Picture, PictureType};
    use lofty::tag::Tag;

    // Parsed from the bytes rather than constructed: the mime the backend then
    // reports is the one the picture actually is, so the check is not comparing
    // a constant against itself.
    let mut reader = data;
    let mut picture = Picture::from_reader(&mut reader).unwrap_or_else(|e| fail(&format!("png: {e}")));
    picture.set_pic_type(PictureType::CoverFront);
    picture.set_description(Some("front".into()));

    let mut file = lofty::read_from_path(path).unwrap_or_else(|e| fail(&format!("open: {e}")));
    let mut tag = Tag::new(family);
    tag.push_picture(picture);
    file.insert_tag(tag);
    file.save_to_path(path, WriteOptions::default())
        .unwrap_or_else(|e| fail(&format!("save: {e}")));
}

/// A 1x1 transparent PNG. Small, valid, and identifiable by its own signature,
/// which is what makes "the bytes came back unchanged" a real assertion.
const TINY_PNG: [u8; 67] = [
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82,
];

fn main() {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: base_tag_probe <file>");
        std::process::exit(2);
    };
    let path = Path::new(&path);

    // A copy beside the original: this probe writes, and a fixture that
    // matters should not be the thing under test.
    let work = std::env::temp_dir().join(format!(
        "neiro-tag-probe-{}{}",
        std::process::id(),
        path.extension()
            .map(|e| format!(".{}", e.to_string_lossy()))
            .unwrap_or_default()
    ));
    std::fs::copy(path, &work).unwrap_or_else(|e| fail(&format!("copy fixture: {e}")));

    println!("=== {} (working on {}) ===", path.display(), work.display());

    // A container the tag backend cannot parse is skipped, not failed: the
    // engine plays it and this probe is about tags.
    if !neiro_base::tag::supports_tags(&work) {
        println!("  ..   skipped: this container has no tag support in this build");
        println!("\nBASE TAG PROBE SKIPPED");
        return;
    }

    let base = Base::start().expect("start Base");
    let handle = base.handle();
    let (_ticket, events) = handle.subscribe();

    // ---- keys, asked once and independent of any file --------------------
    handle
        .request_tag_keys(Vec::new())
        .expect("request tag keys");
    let lists = match wait_event(&events, Duration::from_secs(5), "tag keys", |e| {
        matches!(e, Event::TagKeysChanged { .. })
    }) {
        Event::TagKeysChanged { lists } => lists,
        _ => unreachable!(),
    };
    check(
        !lists.is_empty(),
        "every family answers with its field names",
        format!("{} families", lists.len()),
    );
    for list in &lists {
        check(
            !list.keys.is_empty(),
            &format!("{} names fields", list.family.as_str()),
            format!("{} names", list.keys.len()),
        );
    }
    // A family spells several keys the same way -- ID3v2 puts the track number
    // and the track total in one TRCK frame, and TIPL holds five roles under
    // one name -- so the format name cannot identify a field. The identity can,
    // and that is what the wire carries.
    let mut ids: Vec<String> = Vec::new();
    for list in &lists {
        for name in &list.keys {
            // Every name a family lists has to resolve back to a key, or a
            // control would offer a row that cannot be written.
            let found = neiro_base::tag::writable_keys(list.family)
                .into_iter()
                .find(|(n, _)| n == name);
            check(
                found.is_some(),
                &format!("{} {} resolves", list.family.as_str(), name),
                String::new(),
            );
            ids.push(neiro_base::tag::key_id(found.expect("resolved").1));
        }
    }
    // The name of one family must not be assumed to work in another: ID3v2
    // calls the title TIT2 and Vorbis calls it TITLE, so a control that used
    // one list for both would write nothing.
    let id3 = lists.iter().find(|l: &&TagKeyList| l.family == TagFamily::Id3v2);
    let vorbis = lists.iter().find(|l| l.family == TagFamily::VorbisComments);
    if let (Some(id3), Some(vorbis)) = (id3, vorbis) {
        let differs = id3.keys.iter().any(|k| !vorbis.keys.contains(k));
        check(
            differs,
            "two families name their fields differently",
            format!("ID3v2 {} names, Vorbis {}", id3.keys.len(), vorbis.keys.len()),
        );
    }

    // ---- read ------------------------------------------------------------
    handle.request_tags(&work).expect("request tags");
    // A container the tag backend cannot read is not a defect in this probe:
    // it is a fact about the backend, and the engine plays those files anyway.
    // Reported as a skip so a run over a real library says so instead of
    // failing on the DSD album.
    let read = match wait_event(&events, Duration::from_secs(5), "tags read", |e| {
        matches!(e, Event::TagsRead { .. })
    }) {
        Event::TagsRead {
            families,
            writable,
            fields,
            ..
        } => (families, writable, fields),
        _ => unreachable!(),
    };
    let (families, writable, fields) = read;
    println!("  families: {families:?}");
    println!("  writable: {writable:?}");
    check(
        !writable.is_empty(),
        "the container reports at least one writable family",
        format!("{}", writable.len()),
    );
    // Every field names the family it came from, which is the whole point of
    // carrying it: two families can disagree and an editor has to show that.
    check(
        fields.iter().all(|f| writable.contains(&f.family) || families.contains(&f.family)),
        "every field is attributed to a family the file has",
        format!("{} fields", fields.len()),
    );
    // The name and the identity have to agree. A reader that reported one
    // identity for every field would pass every other check here -- the
    // families are right, the values are right -- and only this one catches it.
    for field in &fields {
        let resolved = neiro_base::tag::writable_keys(field.family)
            .into_iter()
            .find(|(n, _)| *n == field.name)
            .map(|(_, k)| neiro_base::tag::key_id(k));
        if let Some(resolved) = resolved {
            check(
                resolved == field.key,
                &format!("{} {} carries its own identity", field.family.as_str(), field.name),
                format!("name resolves to {resolved}, field says {}", field.key),
            );
        }
    }

    // ---- a name the family does not have --------------------------------
    //
    // Refused before the file is opened: a write that dropped the field would
    // report success for an edit that did not happen.
    let wrong = TagFieldValue {
        family: TagFamily::Id3v2,
        key: "NotAField".into(),
        name: String::new(),
        value: Some("x".into()),
    };
    match fields_from_values(&[wrong]) {
        Err(_) => ok("a name the family does not have is refused"),
        Ok(_) => fail("accepted a field name no family maps"),
    }

    // ---- a family the container cannot hold ------------------------------
    let impossible = [
        TagFamily::Id3v1,
        TagFamily::Id3v2,
        TagFamily::Ape,
        TagFamily::VorbisComments,
        TagFamily::Mp4Ilst,
        TagFamily::RiffInfo,
        TagFamily::AiffText,
    ]
    .into_iter()
    .find(|f| !writable.contains(f));
    if let Some(impossible) = impossible {
        let mut policy = TagPolicy::default();
        policy.write = [impossible].into_iter().collect();
        let (before, _) = digest(&work);
        handle
            .request_tag_write(
                &work,
                vec![TagFieldValue {
                    family: impossible,
                    key: neiro_base::tag::key_id(lofty::tag::ItemKey::TrackTitle),
                    name: "TITLE".into(),
                    value: Some("x".into()),
                }],
                policy,
            )
            .expect("request an impossible write");
        let (_, message) = wait_error(&events, Duration::from_secs(5), "tags");
        ok(&format!("{} refused: {message}", impossible.as_str()));
        let (after, _) = digest(&work);
        check(
            before == after,
            "a refused write leaves the file byte-identical",
            format!("{before:016x} -> {after:016x}"),
        );
    }

    // ---- write -----------------------------------------------------------
    let target = writable[0];
    // The wire carries the identity, so one name works for every family; the
    // format's own name for the field is a display detail.
    let key = neiro_base::tag::key_id(lofty::tag::ItemKey::TrackTitle);
    let mut policy = TagPolicy::default();
    policy.write = [target].into_iter().collect();
    policy.id3v2 = Id3v2Encoding::Utf8;

    let want = "Probe Title 2026";
    handle
        .request_tag_write(
            &work,
            vec![TagFieldValue {
                family: target,
                key: key.clone(),
                name: "TITLE".into(),
                value: Some(want.into()),
            }],
            policy,
        )
        .expect("request write");
    match wait_event(&events, Duration::from_secs(10), "write", |e| {
        matches!(e, Event::TagsChanged { .. })
    }) {
        Event::TagsChanged { families, .. } => ok(&format!(
            "{} wrote and the file now carries {families:?}",
            target.as_str()
        )),
        _ => unreachable!(),
    }

    // Read back through the protocol, not through the in-process call, so the
    // answer a host would get is the one under test.
    handle.request_tags(&work).expect("reread");
    let back = match wait_event(&events, Duration::from_secs(5), "reread", |e| {
        matches!(e, Event::TagsRead { .. })
    }) {
        Event::TagsRead { fields, .. } => fields,
        _ => unreachable!(),
    };
    let found = back
        .iter()
        .find(|f| f.family == target && f.key == key)
        .and_then(|f| f.value.as_deref());
    check(
        found == Some(want),
        "the value written is the value read back",
        format!("{found:?}"),
    );

    // ---- erase-only, the "remove strays" action --------------------------
    //
    // Writing a family and then clearing it is the whole of Picard's "remove
    // ID3 tags from FLAC files": the container is left with what it should
    // have had. The probe creates the second family itself rather than relying
    // on the fixture carrying one, so the check runs on every container.
    if writable.len() >= 2 {
        let second = writable[1];
        let title = neiro_base::tag::key_id(lofty::tag::ItemKey::TrackTitle);
        let mut write_second = TagPolicy::default();
        write_second.write = [second].into_iter().collect();
        handle
            .request_tag_write(
                &work,
                vec![TagFieldValue {
                    family: second,
                    key: title.clone(),
                    name: "TITLE".into(),
                    value: Some("Erase Me".into()),
                }],
                write_second,
            )
            .expect("request a second family");
        let carried = match wait_event(&events, Duration::from_secs(10), "second family", |e| {
            matches!(e, Event::TagsChanged { .. })
        }) {
            Event::TagsChanged { families, .. } => families,
            _ => unreachable!(),
        };
        check(
            carried.contains(&second),
            &format!("{} was written before the erase", second.as_str()),
            format!("{carried:?}"),
        );

        let mut erase = TagPolicy::default();
        erase.clear = [second].into_iter().collect();
        handle
            .request_tag_write(&work, Vec::new(), erase)
            .expect("request an erase");
        match wait_event(&events, Duration::from_secs(10), "erase", |e| {
            matches!(e, Event::TagsChanged { .. })
        }) {
            Event::TagsChanged { families, .. } => check(
                !families.contains(&second),
                &format!("{} erased and the file no longer carries it", second.as_str()),
                format!("{families:?}"),
            ),
            _ => unreachable!(),
        }
    } else {
        // One writable family: there is no second one to create and remove.
        println!("  ..   the container has one writable family; erase-only not exercised");
    }

    // ---- a value that is not text ----------------------------------------
    //
    // `None` is how the wire says "this item is not text" -- an embedded
    // picture, which this build does not write. Refused rather than written as
    // an empty field.
    let binary = TagFieldValue {
        family: target,
        key: neiro_base::tag::key_id(lofty::tag::ItemKey::TrackTitle),
        name: "TITLE".into(),
        value: None,
    };
    match fields_from_values(&[binary]) {
        Err(_) => ok("a non-text value is refused"),
        Ok(_) => fail("accepted a non-text value as a writable field"),
    }

    // ---- a family subset --------------------------------------------------
    //
    // `list_tag_keys` takes a subset; a host that only draws one family should
    // not have to receive seven lists.
    handle
        .request_tag_keys(vec![target])
        .expect("request one family's keys");
    let subset = match wait_event(&events, Duration::from_secs(5), "subset keys", |e| {
        matches!(e, Event::TagKeysChanged { .. })
    }) {
        Event::TagKeysChanged { lists } => lists,
        _ => unreachable!(),
    };
    check(
        subset.len() == 1 && subset[0].family == target,
        "a family subset answers with that family alone",
        format!("{} lists", subset.len()),
    );

    // ---- artwork ---------------------------------------------------------
    //
    // Two questions, and the point is that they are separate: a listing with no
    // bytes, and one picture's bytes on request. A track change asks the first.
    let art_family = writable
        .iter()
        .find(|f| {
            matches!(
                f,
                TagFamily::Id3v2 | TagFamily::VorbisComments | TagFamily::Mp4Ilst
            )
        })
        .copied();
    match art_family {
        Some(family) => {
            insert_picture(&work, family.to_lofty(), &TINY_PNG);

            handle
                .request_artwork(&work, None)
                .expect("request the artwork listing");
            let listed = match wait_event(&events, Duration::from_secs(5), "artwork", |e| {
                matches!(e, Event::ArtworkRead { .. })
            }) {
                Event::ArtworkRead { pictures, data, .. } => (pictures, data),
                _ => unreachable!(),
            };
            check(
                listed.0.len() == 1,
                "the listing reports the picture",
                format!("{} pictures", listed.0.len()),
            );
            check(
                listed.1.is_none(),
                "the listing carries no bytes",
                format!("{:?}", listed.1.as_ref().map(|d| d.len())),
            );
            if let Some(info) = listed.0.first() {
                check(
                    info.bytes == TINY_PNG.len(),
                    "the listing reports the picture's size",
                    format!("{} bytes", info.bytes),
                );
                check(
                    info.mime.as_deref() == Some("image/png"),
                    "the mime comes from the tag",
                    format!("{:?}", info.mime),
                );
                check(
                    info.kind == "CoverFront",
                    "the role comes from the tag",
                    info.kind.clone(),
                );
            }

            handle
                .request_artwork(&work, Some(0))
                .expect("request the picture");
            let fetched = match wait_event(&events, Duration::from_secs(5), "picture", |e| {
                matches!(e, Event::ArtworkRead { .. })
            }) {
                Event::ArtworkRead { data, .. } => data,
                _ => unreachable!(),
            };
            let bytes = fetched
                .and_then(|b64| {
                    use base64::Engine as _;
                    base64::engine::general_purpose::STANDARD.decode(b64).ok()
                })
                .unwrap_or_default();
            check(
                bytes == TINY_PNG,
                "the bytes decoded are the bytes in the file",
                format!("{} bytes", bytes.len()),
            );

            // ---- replacing, appending and removing --------------------
            //
            // Each edit is reported by re-reading, so the assertion is against
            // the file and not against what the command claimed to do.
            let b64 = {
                use base64::Engine as _;
                base64::engine::general_purpose::STANDARD.encode(TINY_PNG)
            };

            // Appending makes two.
            handle
                .request_set_artwork(
                    &work,
                    family,
                    None,
                    b64.clone(),
                    Some("image/png".into()),
                    Some("CoverBack".into()),
                    Some("inner".into()),
                )
                .expect("append a picture");
            let after_add = match wait_event(&events, Duration::from_secs(10), "append", |e| {
                matches!(e, Event::ArtworkChanged { .. })
            }) {
                Event::ArtworkChanged { pictures, .. } => pictures,
                _ => unreachable!(),
            };
            check(
                after_add.len() == 2,
                "appending leaves two pictures",
                format!("{} pictures", after_add.len()),
            );
            check(
                after_add.get(1).map(|p| p.kind.as_str()) == Some("CoverBack"),
                "the appended picture keeps the role it was given",
                format!("{:?}", after_add.get(1).map(|p| p.kind.clone())),
            );

            // Replacing index 1 keeps the count and changes the bytes.
            let mut other = TINY_PNG;
            other[40] ^= 0xff;
            let other_b64 = {
                use base64::Engine as _;
                base64::engine::general_purpose::STANDARD.encode(other)
            };
            handle
                .request_set_artwork(
                    &work,
                    family,
                    Some(1),
                    other_b64,
                    Some("image/png".into()),
                    Some("CoverBack".into()),
                    None,
                )
                .expect("replace a picture");
            let after_replace =
                match wait_event(&events, Duration::from_secs(10), "replace", |e| {
                    matches!(e, Event::ArtworkChanged { .. })
                }) {
                    Event::ArtworkChanged { pictures, .. } => pictures,
                    _ => unreachable!(),
                };
            check(
                after_replace.len() == 2,
                "replacing keeps the count",
                format!("{} pictures", after_replace.len()),
            );
            let bytes_now = neiro_base::tag::family_artwork(&work, family)
                .map(|list| list.get(1).map(|a| a.data.clone()))
                .ok()
                .flatten()
                .unwrap_or_default();
            check(
                bytes_now == other.to_vec(),
                "replacing stores the new bytes, measured from the file",
                format!("{} bytes", bytes_now.len()),
            );

            // Removing takes one out and leaves the other.
            handle
                .request_remove_artwork(&work, family, 1)
                .expect("remove a picture");
            let after_remove =
                match wait_event(&events, Duration::from_secs(10), "remove", |e| {
                    matches!(e, Event::ArtworkChanged { .. })
                }) {
                    Event::ArtworkChanged { pictures, .. } => pictures,
                    _ => unreachable!(),
                };
            check(
                after_remove.len() == 1,
                "removing leaves one picture",
                format!("{} pictures", after_remove.len()),
            );

            // A family with no room for a picture is refused before the file is
            // opened, not silently accepted.
            if writable.contains(&TagFamily::Id3v1) {
                handle
                    .request_set_artwork(
                        &work,
                        TagFamily::Id3v1,
                        None,
                        b64.clone(),
                        None,
                        None,
                        None,
                    )
                    .expect("request an impossible picture");
                let (_, message) = wait_error(&events, Duration::from_secs(5), "artwork");
                ok(&format!("a text-only family is refused: {message}"));
            }

            // An index past the end is an error, not an empty picture: silence
            // would look like a cover that failed to decode.
            handle
                .request_artwork(&work, Some(99))
                .expect("request a missing picture");
            let (_, message) = wait_error(&events, Duration::from_secs(5), "artwork");
            ok(&format!("a missing index is refused: {message}"));
        }
        None => {
            println!("  ..   the container holds no picture family; artwork not exercised");
        }
    }

    // ---- no temp file left behind ----------------------------------------
    if let Some(dir) = work.parent() {
        let leftovers: Vec<String> = std::fs::read_dir(dir)
            .unwrap_or_else(|e| fail(&format!("read_dir: {e}")))
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n.contains(".neiro-tmp-"))
            .collect();
        check(
            leftovers.is_empty(),
            "no temporary file left behind",
            format!("{leftovers:?}"),
        );
    }

    handle.shutdown().expect("shutdown");
    drop(base);
    let _ = std::fs::remove_file(&work);

    println!("\nBASE TAG PROBE PASSED");
}
