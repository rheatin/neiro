//! Tag write-back: what the file says about itself, and how to change it.
//!
//! Base owns this because it owns the library: favourites and playlists already
//! live here, and a tag the listener edits is the same kind of fact. The engine
//! stays audio-only -- it decodes samples and reports what the container said
//! when it opened them, and it never writes to a listener's file.
//!
//! # Why a policy rather than a list of fields
//!
//! A container can carry several tag families at once. An MP3 may hold ID3v1,
//! ID3v2 and APEv2 simultaneously, and the listener has to say which of them to
//! read, which to write and which to erase -- that is the three-column panel in
//! every tag editor. Collapsing that into "write these fields" loses the only
//! decision that matters, so [`TagPolicy`] carries it explicitly.
//!
//! # Why the write is a replace
//!
//! [`lofty`] edits in place: it seeks to the tag region and rewrites it, and
//! when the new tag does not fit in the existing padding it shifts the audio
//! that follows. A process that dies between the shift and the write leaves a
//! file whose audio no longer matches its frames. So every write goes through a
//! copy in the same directory, which is renamed over the original only once it
//! is complete and flushed. The copy is not a backup -- it is deleted by the
//! rename, and nothing is left behind when the write succeeds.
//!
//! # The one thing this cannot do
//!
//! `lofty`'s `ItemKey` is a closed set of 89 named fields; the formats that
//! allow arbitrary keys (Vorbis comments, APE) cannot be given one it does not
//! name. Everything a listener is likely to edit is in that set -- title,
//! artist, album artist, composer, conductor, ISRC, BPM, mood, lyrics, sort
//! orders, the lot -- but a private `X-MY-FIELD` cannot be written. That is a
//! limit of the container backend, not of this module, and it is recorded here
//! so a later reader does not rediscover it as a bug.

use std::collections::BTreeSet;
use std::fs;
use std::io::{self, Write as _};
use std::path::{Path, PathBuf};

use lofty::config::WriteOptions;
use lofty::file::{AudioFile, TaggedFile, TaggedFileExt};
use lofty::picture::PictureType;
use lofty::tag::{ItemKey, ItemValue, Tag, TagItem, TagType};

/// One tag family a container can carry, in the vocabulary of the file itself.
///
/// A container supports some subset of these; [`TagPolicy::write`] naming a
/// family the file cannot hold is caught before anything is written, rather
/// than being silently dropped on the floor.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TagFamily {
    Id3v1,
    Id3v2,
    Ape,
    VorbisComments,
    Mp4Ilst,
    RiffInfo,
    AiffText,
}

impl TagFamily {
    /// The `lofty` name for this family.
    ///
    /// Public because the write path is not the only caller: a probe that has
    /// to put a picture in a file needs the same mapping, and restating it
    /// there would be a second table to keep in step.
    pub fn to_lofty(self) -> TagType {
        match self {
            Self::Id3v1 => TagType::Id3v1,
            Self::Id3v2 => TagType::Id3v2,
            Self::Ape => TagType::Ape,
            Self::VorbisComments => TagType::VorbisComments,
            Self::Mp4Ilst => TagType::Mp4Ilst,
            Self::RiffInfo => TagType::RiffInfo,
            Self::AiffText => TagType::AiffText,
        }
    }

    /// The family, or `None` for one this build does not know.
    ///
    /// `TagType` is non-exhaustive, so a future backend release can hand back a
    /// family we have never heard of. Reporting it as some existing family
    /// would be a lie about what is in the file; dropping it from the listing
    /// is the honest answer, and the caller decides what to say.
    fn from_lofty(t: TagType) -> Option<Self> {
        Some(match t {
            TagType::Id3v1 => Self::Id3v1,
            TagType::Id3v2 => Self::Id3v2,
            TagType::Ape => Self::Ape,
            TagType::VorbisComments => Self::VorbisComments,
            TagType::Mp4Ilst => Self::Mp4Ilst,
            TagType::RiffInfo => Self::RiffInfo,
            TagType::AiffText => Self::AiffText,
            _ => return None,
        })
    }

    /// The family's name as a tag editor labels it.
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Id3v1 => "ID3v1",
            Self::Id3v2 => "ID3v2",
            Self::Ape => "APE",
            Self::VorbisComments => "Vorbis comment",
            Self::Mp4Ilst => "MP4 ilst",
            Self::RiffInfo => "RIFF INFO",
            Self::AiffText => "AIFF text",
        }
    }

    /// Whether every file of this family carries the field under the same key.
    ///
    /// Only ID3v1 is fixed-width and fixed-name; the others can hold any of the
    /// keys they declare.
    pub fn is_fixed_vocabulary(self) -> bool {
        matches!(self, Self::Id3v1)
    }
}

/// How text is encoded inside an ID3v2 tag.
///
/// Three encodings, because the answer changes which version of the frame is
/// written: UTF-8 only exists in ID3v2.4, and ISO-8859-1 only survives without
/// loss in a listener's own locale. A tag editor has to let the listener pick,
/// so this is a policy field rather than a constant.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Id3v2Encoding {
    /// ID3v2.4, UTF-8. The right default: it holds any script.
    #[default]
    Utf8,
    /// ID3v2.3, UTF-16.
    Utf16,
    /// ID3v2.3, ISO-8859-1. Lossy for anything outside Latin-1.
    Latin1,
}

impl Id3v2Encoding {
    /// The `lofty` switches this encoding implies.
    ///
    /// `use_id3v23` selects the frame version and `lossy_text_encoding` selects
    /// ISO-8859-1 over UTF-16; the two together are exactly the three choices.
    fn write_options(self) -> (bool, bool) {
        match self {
            Self::Utf8 => (false, false),
            Self::Utf16 => (true, false),
            Self::Latin1 => (true, true),
        }
    }
}

/// Which families to read, write and erase, and how ID3v2 text is encoded.
///
/// `clear` with an empty `write` is erase-only: it strips the families a
/// container should not carry and leaves the rest alone. That is the operation
/// behind "remove ID3 tags from FLAC files" and "remove APEv2 tags from MP3
/// files", and it is a real edit rather than a degenerate write.
#[derive(Debug, Clone, Default, serde::Serialize, serde::Deserialize)]
#[serde(default)]
pub struct TagPolicy {
    /// Families to write the given fields into.
    pub write: BTreeSet<TagFamily>,
    /// Families to erase entirely before writing.
    pub clear: BTreeSet<TagFamily>,
    pub id3v2: Id3v2Encoding,
    /// Write ID3v2 only when ID3v1 cannot hold the text.
    ///
    /// ID3v1 is 30 bytes of ISO-8859-1 per field, so a title in Japanese does
    /// not fit and the file needs ID3v2. When this is set, a file whose text
    /// does fit gets ID3v1 alone and no ID3v2 block is created -- which is what
    /// a listener with an old player in the chain wants.
    pub id3v2_only_when_id3v1_overflows: bool,
}

/// Every family this module knows, in the order a tag editor lists them.
const ALL_FAMILIES: [TagFamily; 7] = [
    TagFamily::Id3v1,
    TagFamily::Id3v2,
    TagFamily::Ape,
    TagFamily::VorbisComments,
    TagFamily::Mp4Ilst,
    TagFamily::RiffInfo,
    TagFamily::AiffText,
];

/// The bytes ID3v1 can carry in one field.
const ID3V1_FIELD_BYTES: usize = 30;

impl TagPolicy {
    /// Whether the given fields force an ID3v2 block.
    ///
    /// True when any value is outside ISO-8859-1 or longer than a v1 field.
    fn id3v1_cannot_hold(fields: &[TagField]) -> bool {
        fields.iter().any(|f| match &f.value {
            ItemValue::Text(s) | ItemValue::Locator(s) => {
                s.len() > ID3V1_FIELD_BYTES || !s.chars().all(|c| (c as u32) <= 0xFF)
            }
            ItemValue::Binary(_) => true,
        })
    }
}

/// One field to write.
#[derive(Debug, Clone, PartialEq)]
pub struct TagField {
    pub key: ItemKey,
    pub value: ItemValue,
}

impl TagField {
    pub fn text(key: ItemKey, value: impl Into<String>) -> Self {
        Self { key, value: ItemValue::Text(value.into()) }
    }
}

/// One field as the file stores it: which family it came from, and its value.
///
/// The family is part of the record because two families in the same file can
/// disagree about the same key — an MP3 whose ID3v2 says 2019 and whose APEv2
/// says 2018 is a real file, not a corrupt one. A snapshot that flattened them
/// into one list would have to pick a winner, and whichever it picked would be
/// a value nobody owns (AGENTS.md rule 1).
#[derive(Debug, Clone, PartialEq)]
pub struct TagEntry {
    pub family: TagFamily,
    pub key: ItemKey,
    pub value: ItemValue,
}

/// What the file says now.
///
/// `families` is what the file actually carries, which is not the same as what
/// the policy asked for: a WAV holds RIFF INFO and an ID3v2 chunk, an MP4 holds
/// ilst and nothing else. A UI draws its checkboxes from this.
#[derive(Debug, Clone)]
pub struct TagSnapshot {
    pub path: PathBuf,
    pub families: Vec<TagFamily>,
    pub fields: Vec<TagEntry>,
}

impl TagSnapshot {
    /// The first text value for a key, across every family in the file.
    ///
    /// For a control that shows one value. A reader that has to show the
    /// families separately walks `fields`, which keeps them apart.
    pub fn get_text(&self, key: ItemKey) -> Option<&str> {
        self.fields.iter().find_map(|f| match (&f.value, f.key) {
            (ItemValue::Text(s) | ItemValue::Locator(s), k) if k == key => Some(s.as_str()),
            _ => None,
        })
    }

    /// The text one named family gives this key, if that family carries it.
    pub fn get_text_in(&self, family: TagFamily, key: ItemKey) -> Option<&str> {
        self.fields
            .iter()
            .find(|f| f.family == family && f.key == key)
            .and_then(|f| match &f.value {
                ItemValue::Text(s) | ItemValue::Locator(s) => Some(s.as_str()),
                _ => None,
            })
    }

    /// Every field with its family, in the form the protocol carries.
    pub fn values(&self) -> Vec<TagFieldValue> {
        self.fields.iter().map(TagFieldValue::from_entry).collect()
    }
}

/// One field as the protocol carries it.
///
/// `key` is the identity: the `ItemKey` variant name, which is stable across
/// formats and unambiguous. `name` is how `family` spells that field, and it is
/// for display only.
///
/// The two are not the same thing, and the difference is not cosmetic. A family
/// can spell several keys the same way: ID3v2 puts both the track number and
/// the track total in one `TRCK` frame, and both `TIPL` and `TXXX` hold several
/// roles under one name. Resolving a write by name would have to pick one of
/// them, so the wire carries the identity and lets the editor show the label.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct TagFieldValue {
    pub family: TagFamily,
    /// The `ItemKey` variant name: `TrackNumber`, `TrackTotal`, `TrackTitle`.
    pub key: String,
    /// The name `family` gives this field, when it has one.
    #[serde(default)]
    pub name: String,
    /// The text, or `None` for an item that is not text — an embedded picture.
    /// This build writes text only, so a `None` value is refused by
    /// [`fields_from_values`] rather than being written as an empty field.
    pub value: Option<String>,
}

/// The stable identity of a key: its variant name.
///
/// This is the name a caller sends back. It is the same string in every format,
/// which is what makes it usable as an identifier; the format's own name for
/// the field is not.
pub fn key_id(key: ItemKey) -> String {
    format!("{key:?}")
}

/// The key a variant name denotes, or `None` when no key has that name.
pub fn key_from_id(id: &str) -> Option<ItemKey> {
    ItemKey::VARIANTS
        .iter()
        .find(|k| key_id(**k) == id)
        .copied()
}

impl TagFieldValue {
    fn from_entry(entry: &TagEntry) -> Self {
        Self {
            family: entry.family,
            key: key_id(entry.key),
            name: Self::name_of(entry.family, entry.key),
            value: match &entry.value {
                ItemValue::Text(s) | ItemValue::Locator(s) => Some(s.clone()),
                _ => None,
            },
        }
    }

    /// The name `key` has in `family`'s vocabulary, or its identity when that
    /// family has no name for it.
    ///
    /// A family that names several keys the same way keeps that name: it is
    /// what the file's own documentation calls the field, and a UI that
    /// disambiguated it by inventing a suffix would be showing a name the
    /// format does not have.
    pub fn name_of(family: TagFamily, key: ItemKey) -> String {
        writable_keys(family)
            .into_iter()
            .find(|(_, k)| *k == key)
            .map(|(name, _)| name.to_owned())
            .unwrap_or_else(|| key_id(key))
    }
}

/// Resolve wire fields into the units a write takes.
///
/// An identity no key has is refused here, before any file is opened, for the
/// same reason the policy's families are checked before the copy is made: a
/// write that silently dropped half its fields would report success for an edit
/// that did not happen.
pub fn fields_from_values(values: &[TagFieldValue]) -> Result<Vec<TagField>, TagError> {
    values
        .iter()
        .map(|value| {
            let key = key_from_id(&value.key).ok_or_else(|| {
                TagError::Unsupported(format!("no tag field is named {}", value.key))
            })?;
            let Some(text) = &value.value else {
                return Err(TagError::Unsupported(format!(
                    "{} {} is not text; this build writes text only",
                    value.family.as_str(),
                    value.key
                )));
            };
            Ok(TagField::text(key, text.clone()))
        })
        .collect()
}

/// One embedded picture: what it is, and its bytes.
///
/// Kept apart from [`TagEntry`] because a picture is not a text field. The
/// wire carries pictures through their own command, for two reasons: a
/// `tags_read` answers on every selection and a 640x640 cover is a hundred
/// kilobytes, and a `TagFieldValue` with a `null` value already says "there is
/// a picture here" without carrying it.
#[derive(Debug, Clone)]
pub struct Artwork {
    /// The family that holds this picture. A file can carry the same cover in
    /// more than one, and each family indexes its own list.
    pub family: TagFamily,
    /// The picture's role: `CoverFront`, `CoverBack`, `Artist`.
    pub kind: PictureType,
    pub mime: Option<String>,
    pub description: Option<String>,
    pub data: Vec<u8>,
}

impl Artwork {
    /// What this picture is, without its bytes, for a listing.
    pub fn info(&self) -> ArtworkInfo {
        ArtworkInfo {
            family: self.family,
            kind: picture_kind_id(self.kind),
            mime: self.mime.clone(),
            description: self.description.clone(),
            bytes: self.data.len(),
        }
    }

    /// The bytes as base64, the only form the JSON bridge can carry.
    ///
    /// Here rather than at the call site: one encoder means one answer for
    /// "what does this picture look like on the wire", and a host that decodes
    /// it gets the same bytes the file holds.
    pub fn to_base64(&self) -> String {
        use base64::Engine as _;
        base64::engine::general_purpose::STANDARD.encode(&self.data)
    }
}

/// A picture's role, type and size. Enough to list them; the bytes are asked
/// for separately.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct ArtworkInfo {
    /// The family that holds it. Carried because a file can hold the same cover
    /// in two families, and an index means nothing without the list it counts.
    pub family: TagFamily,
    /// The `PictureType` variant name: `CoverFront`, `CoverBack`, `Artist`.
    pub kind: String,
    /// `image/jpeg`, `image/png`, or absent when the file did not say.
    pub mime: Option<String>,
    pub description: Option<String>,
    pub bytes: usize,
}

/// Every picture the file carries, in family order and then in each family's
/// own order.
///
/// An empty vector is a measurement -- the file has no pictures -- and is not
/// the same as a failure to read it.
pub fn read_artwork(path: &Path) -> Result<Vec<Artwork>, TagError> {
    let file = open(path)?;
    let mut out = Vec::new();
    for tag in file.tags() {
        let Some(family) = TagFamily::from_lofty(tag.tag_type()) else {
            continue;
        };
        for picture in tag.pictures() {
            out.push(Artwork {
                family,
                kind: picture.pic_type(),
                mime: picture.mime_type().map(|m| m.as_str().to_owned()),
                description: picture.description().map(|d| d.to_owned()),
                data: picture.data().to_vec(),
            });
        }
    }
    Ok(out)
}

/// The pictures one family holds, in its own order.
///
/// An index a caller sends is an index into this, which is why it has to be
/// asked per family rather than over the flattened list.
pub fn family_artwork(path: &Path, family: TagFamily) -> Result<Vec<Artwork>, TagError> {
    Ok(read_artwork(path)?
        .into_iter()
        .filter(|a| a.family == family)
        .collect())
}

/// A picture to put into a file.
///
/// The bytes are what the caller has; the role, mime and description are what
/// the tag will say about them. Everything but `data` has a defined default,
/// because a caller that only has bytes should not have to invent the rest.
#[derive(Debug, Clone)]
pub struct ArtworkInput {
    pub data: Vec<u8>,
    pub mime: Option<String>,
    pub kind: Option<String>,
    pub description: Option<String>,
}

impl ArtworkInput {
    /// The role this picture should have, and the error when the name is one
    /// this build does not know.
    ///
    /// An unknown name is refused rather than defaulted: writing `CoverFront`
    /// for a caller that asked for something else stores a different picture
    /// than the one it wanted.
    fn picture_type(&self) -> Result<PictureType, TagError> {
        match self.kind.as_deref() {
            None => Ok(PictureType::CoverFront),
            Some(id) => picture_kind_from_id(id)
                .ok_or_else(|| TagError::Unsupported(format!("no picture kind is named {id}"))),
        }
    }
}

/// The identity of a picture's role, and the way back.
///
/// The same shape as a field's: the wire carries a stable name, and a name this
/// build does not know is refused rather than guessed at.
pub fn picture_kind_id(kind: PictureType) -> String {
    format!("{kind:?}")
}

pub fn picture_kind_from_id(id: &str) -> Option<PictureType> {
    Some(match id {
        "Other" => PictureType::Other,
        "Icon" => PictureType::Icon,
        "OtherIcon" => PictureType::OtherIcon,
        "CoverFront" => PictureType::CoverFront,
        "CoverBack" => PictureType::CoverBack,
        "Leaflet" => PictureType::Leaflet,
        "Media" => PictureType::Media,
        "LeadArtist" => PictureType::LeadArtist,
        "Artist" => PictureType::Artist,
        "Conductor" => PictureType::Conductor,
        "Band" => PictureType::Band,
        "Composer" => PictureType::Composer,
        "Lyricist" => PictureType::Lyricist,
        "RecordingLocation" => PictureType::RecordingLocation,
        "DuringRecording" => PictureType::DuringRecording,
        "DuringPerformance" => PictureType::DuringPerformance,
        "ScreenCapture" => PictureType::ScreenCapture,
        "BrightFish" => PictureType::BrightFish,
        "Illustration" => PictureType::Illustration,
        "BandLogo" => PictureType::BandLogo,
        "PublisherLogo" => PictureType::PublisherLogo,
        _ => return None,
    })
}

/// Whether a family has anywhere to put a picture.
///
/// ID3v1 is eight fixed text fields, RIFF INFO is text chunks and AIFF text is
/// text: none of them can carry an image. The container's writability says
/// nothing about this, so it is a separate question.
pub fn family_holds_pictures(family: TagFamily) -> bool {
    matches!(
        family,
        TagFamily::Id3v2 | TagFamily::Ape | TagFamily::VorbisComments | TagFamily::Mp4Ilst
    )
}

#[derive(Debug)]
pub enum TagError {
    Io(io::Error),
    /// The file is not a container this build understands.
    Unsupported(String),
    /// The policy names a family this file cannot hold.
    FamilyNotSupported { file: PathBuf, family: TagFamily },
    /// `lofty` failed to parse or encode.
    Backend(String),
}

impl std::fmt::Display for TagError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "io: {e}"),
            Self::Unsupported(s) => write!(f, "unsupported container: {s}"),
            Self::FamilyNotSupported { file, family } => {
                write!(f, "{} cannot carry {}", file.display(), family.as_str())
            }
            Self::Backend(s) => write!(f, "tag backend: {s}"),
        }
    }
}

impl std::error::Error for TagError {}

impl From<io::Error> for TagError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

/// Containers the decoder plays and the tag backend cannot read.
///
/// Measured, not guessed. `lofty` recognises both by their magic bytes and then
/// has no parser, so the failure surfaces as "failed to parse file" -- which
/// reads like a corrupt download and is not one. The engine decodes both: 33
/// `.dsf` and 13 `.tak` in one real library, every one playing at its own rate.
///
/// This is a statement about the dependency, not about music, and it has to be
/// revisited when the backend grows a parser. There is nothing to interrogate:
/// `FileType` has no variant for either, and the probe accepts them anyway.
const NO_TAG_SUPPORT: &[&str] = &["dsf", "dff", "tak"];

/// Whether this file's tags can be read and written at all.
///
/// Asks before an edit dialog is drawn: a file the backend cannot parse is a
/// file whose tag form would be empty and whose Save would fail. The engine
/// still plays it, so "cannot edit" is the whole of the limitation.
///
/// The extension decides here, because the backend's own probe accepts these
/// containers and then fails, which is not a question a caller can usefully ask.
pub fn supports_tags(path: &Path) -> bool {
    let extension = path
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    !NO_TAG_SUPPORT.contains(&extension.as_str())
}

/// One row of the browsing index: the tags a list needs, plus the audio
/// properties that let it be drawn without decoding.
///
/// Read with a single open. A caller that used `read_tags` and then asked the
/// engine for the format would open the file twice and decode it once, which is
/// what made a library view cost a decode per row.
pub fn read_index_row(path: &Path) -> Result<crate::library::StoredTags, TagError> {
    let file = open(path)?;
    let mut row = crate::library::StoredTags {
        path: path.to_path_buf(),
        ..Default::default()
    };
    for tag in file.tags() {
        for item in tag.items() {
            let text = || match item.value() {
                ItemValue::Text(s) | ItemValue::Locator(s) => Some(s.clone()),
                _ => None,
            };
            let number = || text().and_then(|t| {
                let digits: String = t.chars().take_while(|c| c.is_ascii_digit()).collect();
                digits.parse::<i64>().ok()
            });
            match item.key() {
                ItemKey::TrackTitle if row.title.is_empty() => {
                    if let Some(v) = text() { row.title = v }
                }
                ItemKey::TrackArtist if row.artist.is_empty() => {
                    if let Some(v) = text() { row.artist = v }
                }
                ItemKey::AlbumTitle if row.album.is_empty() => {
                    if let Some(v) = text() { row.album = v }
                }
                ItemKey::AlbumArtist if row.album_artist.is_empty() => {
                    if let Some(v) = text() { row.album_artist = v }
                }
                ItemKey::Genre if row.genre.is_empty() => {
                    if let Some(v) = text() { row.genre = v }
                }
                ItemKey::DiscNumber if row.disc_no == 0 => {
                    if let Some(v) = number() { row.disc_no = v }
                }
                ItemKey::TrackNumber if row.track_no == 0 => {
                    if let Some(v) = number() { row.track_no = v }
                }
                ItemKey::Year | ItemKey::RecordingDate if row.year == 0 => {
                    if let Some(v) = number() { row.year = v }
                }
                _ => {}
            }
        }
    }
    // The file's own numbers, not the tag's. A WAV carries no duration frame,
    // and a FLAC's STREAMINFO is more trustworthy than an ID3 frame that was
    // copied from somewhere else.
    let properties = file.properties();
    row.duration_ms = properties.duration().as_millis() as i64;
    row.sample_rate = i64::from(properties.sample_rate().unwrap_or(0));
    row.bits = i64::from(properties.bit_depth().unwrap_or(0));
    row.channels = i64::from(properties.channels().unwrap_or(0));
    Ok(row)
}

fn open(path: &Path) -> Result<TaggedFile, TagError> {
    match lofty::read_from_path(path) {
        Ok(file) => Ok(file),
        Err(e) => {
            let extension = path
                .extension()
                .and_then(|e| e.to_str())
                .unwrap_or("")
                .to_ascii_lowercase();
            if NO_TAG_SUPPORT.contains(&extension.as_str()) {
                return Err(TagError::Unsupported(format!(
                    "{} has no tag support in this build; the engine still plays it",
                    extension.to_ascii_uppercase()
                )));
            }
            Err(TagError::Backend(e.to_string()))
        }
    }
}

/// Read every tag family the file carries.
pub fn read_tags(path: &Path) -> Result<TagSnapshot, TagError> {
    let file = open(path)?;
    let mut families = Vec::new();
    let mut fields = Vec::new();
    for tag in file.tags() {
        // A family this build does not know cannot be named, and a field whose
        // family cannot be named cannot be attributed to one. Both are skipped
        // together, rather than reporting the field under a family it is not
        // from.
        let Some(family) = TagFamily::from_lofty(tag.tag_type()) else {
            continue;
        };
        families.push(family);
        for item in tag.items() {
            fields.push(TagEntry {
                family,
                key: item.key(),
                value: item.value().clone(),
            });
        }
    }
    Ok(TagSnapshot { path: path.to_path_buf(), families, fields })
}

/// The families a file of this kind can carry.
///
/// Asked of the file rather than of its extension: a `.wav` renamed from `.mp3`
/// is decoded as a WAV, and the answer has to follow the content.
pub fn writable_families(path: &Path) -> Result<Vec<TagFamily>, TagError> {
    let file = open(path)?;
    let file_type = file.file_type();
    Ok(ALL_FAMILIES
        .into_iter()
        .filter(|f| file_type.tag_support(f.to_lofty()).is_writable())
        .collect())
}

/// Copy the file, let `mutate` change the copy, then put the copy in place.
///
/// The original is untouched until the replacement is complete and flushed: the
/// work happens on a copy in the same directory, a rename swaps it in, and a
/// failure removes the copy and leaves the original exactly as it was. Same
/// directory on purpose -- a rename across volumes is a copy, and a copy is not
/// atomic.
///
/// `verify` runs against the copy after it is saved and before the rename. It
/// is not optional and not advisory: a backend that accepts the edit and writes
/// nothing must be an error, because reporting success for a write that did not
/// happen is the failure this project treats as worse than a refusal.
fn rewrite<T>(
    path: &Path,
    options: WriteOptions,
    mutate: impl FnOnce(&mut TaggedFile) -> Result<T, TagError>,
    verify: impl FnOnce(&TaggedFile, &T) -> Result<(), TagError>,
) -> Result<T, TagError> {
    let temp = temp_path_for(path)?;
    let result = rewrite_inner(path, &temp, options, mutate, verify);
    // The copy is never left behind, on the success path or the failure path.
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}

fn rewrite_inner<T>(
    path: &Path,
    temp: &Path,
    options: WriteOptions,
    mutate: impl FnOnce(&mut TaggedFile) -> Result<T, TagError>,
    verify: impl FnOnce(&TaggedFile, &T) -> Result<(), TagError>,
) -> Result<T, TagError> {
    fs::copy(path, temp)?;

    let mut file = open(temp)?;
    let outcome = mutate(&mut file)?;

    file.save_to_path(temp, options).map_err(|e| TagError::Backend(e.to_string()))?;
    drop(file);

    // Flush the copy before the rename, or a crash can leave a renamed file
    // whose last blocks never reached the disk.
    {
        let mut f = fs::OpenOptions::new().write(true).open(temp)?;
        f.flush()?;
        f.sync_all()?;
    }

    let check = open(temp)?;
    verify(&check, &outcome)?;
    drop(check);

    replace_file(temp, path)?;
    Ok(outcome)
}

/// The write options a policy implies.
fn options_for(policy: &TagPolicy) -> WriteOptions {
    let (use_v23, lossy) = policy.id3v2.write_options();
    WriteOptions::new()
        .use_id3v23(use_v23)
        .lossy_text_encoding(lossy)
        // Padding is what lets a later edit stay in place instead of shifting
        // the audio; 4 KiB is the figure every tag editor uses.
        .preferred_padding(4096)
}

/// Write fields into a file, then put the result in place atomically.
///
/// The original is untouched until the replacement is complete: the work happens
/// on a copy in the same directory, and a rename swaps it in. A failed write
/// leaves the original exactly as it was and removes the copy.
pub fn write_tags(path: &Path, fields: &[TagField], policy: &TagPolicy) -> Result<(), TagError> {
    // Refuse before touching anything: a policy that names a family this
    // container cannot hold is a caller bug, and dropping it silently would
    // report success for a write that never happened.
    let supported = writable_families(path)?;
    for family in policy.write.iter().chain(policy.clear.iter()) {
        if !supported.contains(family) {
            return Err(TagError::FamilyNotSupported {
                file: path.to_path_buf(),
                family: *family,
            });
        }
    }

    rewrite(
        path,
        options_for(policy),
        |file| {
            for family in &policy.clear {
                clear_family(file, *family);
            }

            let targets = write_targets(fields, policy)?;
            for family in &targets {
                let mut tag = Tag::new(family.to_lofty());
                for field in fields {
                    tag.insert(TagItem::new(field.key, field.value.clone()));
                }
                file.insert_tag(tag);
            }
            Ok((targets, policy.clear.clone()))
        },
        |check, (targets, cleared)| {
            for family in targets {
                if !check.contains_tag_type(family.to_lofty()) {
                    return Err(TagError::Backend(format!(
                        "{} was not written to {}",
                        family.as_str(),
                        path.display()
                    )));
                }
            }
            // A family the policy asked to erase has to be gone, or a UI that
            // offered "remove" would show it still there.
            for family in cleared {
                if check.contains_tag_type(family.to_lofty()) {
                    return Err(TagError::Backend(format!(
                        "{} was not removed from {}",
                        family.as_str(),
                        path.display()
                    )));
                }
            }
            Ok(())
        },
    )?;
    Ok(())
}

/// Remove one family from the file being rewritten.
///
/// `lofty` does not write empty tags, and that is the removal mechanism:
/// `remove` only drops the tag from this list, leaving its bytes on disk, while
/// an empty tag is written as an absence.
fn clear_family(file: &mut TaggedFile, family: TagFamily) {
    file.insert_tag(Tag::new(family.to_lofty()));
}

/// Which families actually receive the fields, and the refusals that stop a
/// write which would change nothing.
fn write_targets(fields: &[TagField], policy: &TagPolicy) -> Result<Vec<TagFamily>, TagError> {
    let mut targets: Vec<TagFamily> = Vec::new();
    let only_when_overflow = policy.id3v2_only_when_id3v1_overflows;
    let overflows = TagPolicy::id3v1_cannot_hold(fields);

    for family in &policy.write {
        // The overflow rule, read both ways. When the text fits ID3v1, ID3v2 is
        // not created. When it does not fit, ID3v1 is dropped instead of being
        // written truncated or rejected: it is 30 bytes of ISO-8859-1, so a
        // title in Japanese has no representation there at all, and asking the
        // backend to try would fail the whole edit over one family.
        if *family == TagFamily::Id3v2 && only_when_overflow && !overflows {
            continue;
        }
        if *family == TagFamily::Id3v1 && overflows && policy.write.contains(&TagFamily::Id3v2) {
            continue;
        }
        targets.push(*family);
    }

    // ID3v1 alone cannot carry this text, and silently writing nothing would
    // report success for an edit that did not happen.
    if targets.as_slice() == [TagFamily::Id3v1] && overflows {
        return Err(TagError::Unsupported(
            "ID3v1 holds 30 bytes of ISO-8859-1 per field; add ID3v2 to the policy to write this text"
                .into(),
        ));
    }
    if targets.is_empty() && policy.clear.is_empty() {
        return Err(TagError::Unsupported(
            "the policy names no family this file can be written with".into(),
        ));
    }
    Ok(targets)
}


/// The text fields a tag holds, in a form that can be compared across a save.
///
/// A tag is replaced whole when a picture is written, so this is the record
/// that survives the replacement only if the backend kept it.
fn fields_of(tag: &Tag) -> Vec<(ItemKey, ItemValue)> {
    tag.items()
        .map(|item| (item.key(), item.value().clone()))
        .collect()
}

/// The tag a family holds, or an empty one when it holds none.
fn tag_for(file: &TaggedFile, family: TagFamily) -> Tag {
    file.tag(family.to_lofty())
        .cloned()
        .unwrap_or_else(|| Tag::new(family.to_lofty()))
}

/// Refuse an artwork edit the file cannot take, before touching it.
fn artwork_target(path: &Path, family: TagFamily) -> Result<(), TagError> {
    if !family_holds_pictures(family) {
        return Err(TagError::Unsupported(format!(
            "{} has no field that can hold a picture",
            family.as_str()
        )));
    }
    let supported = writable_families(path)?;
    if !supported.contains(&family) {
        return Err(TagError::FamilyNotSupported {
            file: path.to_path_buf(),
            family,
        });
    }
    Ok(())
}

/// Put a picture into one family, replacing `index` or appending.
///
/// The family is named because pictures live in one: a file can carry the same
/// cover in ID3v2 and in APE, and an index counts within a family's own list.
pub fn set_artwork(
    path: &Path,
    family: TagFamily,
    index: Option<usize>,
    input: &ArtworkInput,
) -> Result<(), TagError> {
    artwork_target(path, family)?;
    let kind = input.picture_type()?;

    let mut options = WriteOptions::new().preferred_padding(4096);
    if family == TagFamily::Id3v2 {
        options = options.use_id3v23(false).lossy_text_encoding(false);
    }

    rewrite(
        path,
        options,
        |file| {
            let mut tag = tag_for(file, family);
            // The whole tag is replaced, so the fields have to survive the
            // replacement. Captured here and compared after the save: a picture
            // write that dropped the titles would look like a success.
            let before = fields_of(&tag);
            let picture = build_picture(kind, input);
            match index {
                Some(i) => {
                    if i >= tag.pictures().len() {
                        return Err(TagError::Unsupported(format!(
                            "{} has {} picture(s); there is no index {}",
                            family.as_str(),
                            tag.pictures().len(),
                            i
                        )));
                    }
                    tag.set_picture(i, picture);
                }
                None => tag.push_picture(picture),
            }
            file.insert_tag(tag);
            Ok((index, before))
        },
        |check, (replaced, before)| {
            let after = check
                .tag(family.to_lofty())
                .map(fields_of)
                .unwrap_or_default();
            if after != *before {
                return Err(TagError::Backend(format!(
                    "{} lost or changed a text field while a picture was written",
                    family.as_str()
                )));
            }
            let said = check
                .tag(family.to_lofty())
                .map(|t| t.pictures().to_vec())
                .unwrap_or_default();
            let sought = replaced.unwrap_or(said.len().saturating_sub(1));
            match said.get(sought) {
                Some(picture) if picture.data() == input.data.as_slice() => Ok(()),
                Some(_) => Err(TagError::Backend(format!(
                    "{} picture {sought} is not the one that was written",
                    family.as_str()
                ))),
                None => Err(TagError::Backend(format!(
                    "{} has no picture {sought} after the write",
                    family.as_str()
                ))),
            }
        },
    )?;
    Ok(())
}

/// Remove one picture from one family.
pub fn remove_artwork(path: &Path, family: TagFamily, index: usize) -> Result<(), TagError> {
    artwork_target(path, family)?;

    let mut options = WriteOptions::new().preferred_padding(4096);
    if family == TagFamily::Id3v2 {
        options = options.use_id3v23(false).lossy_text_encoding(false);
    }

    rewrite(
        path,
        options,
        |file| {
            let mut tag = tag_for(file, family);
            let fields = fields_of(&tag);
            let before = tag.pictures().len();
            if index >= before {
                return Err(TagError::Unsupported(format!(
                    "{} has {before} picture(s); there is no index {index}",
                    family.as_str()
                )));
            }
            tag.remove_picture(index);
            file.insert_tag(tag);
            Ok((before, fields))
        },
        |check, (before, fields)| {
            if let Some(tag) = check.tag(family.to_lofty()) {
                if fields_of(tag) != *fields {
                    return Err(TagError::Backend(format!(
                        "{} lost or changed a text field while a picture was removed",
                        family.as_str()
                    )));
                }
            } else if !fields.is_empty() {
                return Err(TagError::Backend(format!(
                    "{} took its text fields with it when the last picture left",
                    family.as_str()
                )));
            }
            let after = check
                .tag(family.to_lofty())
                .map(|t| t.pictures().len())
                .unwrap_or(0);
            if after + 1 != *before {
                return Err(TagError::Backend(format!(
                    "{} had {before} picture(s) and has {after} after removing one",
                    family.as_str()
                )));
            }
            Ok(())
        },
    )?;
    Ok(())
}

/// The picture to store, with the role, type and description the caller gave.
fn build_picture(kind: PictureType, input: &ArtworkInput) -> lofty::picture::Picture {
    use lofty::picture::{MimeType, Picture};
    let mut builder = Picture::unchecked(input.data.clone()).pic_type(kind);
    if let Some(mime) = input.mime.as_deref() {
        builder = builder.mime_type(MimeType::from_str(mime));
    }
    if let Some(description) = input.description.as_deref() {
        builder = builder.description(description.to_owned());
    }
    builder.build()
}

/// Rename `from` over `to`, replacing it.
///
/// `fs::rename` maps to `MoveFileEx(.., MOVEFILE_REPLACE_EXISTING)` on Windows
/// and to `rename(2)` elsewhere, so both replace an existing destination.
fn replace_file(from: &Path, to: &Path) -> Result<(), TagError> {
    fs::rename(from, to)?;
    Ok(())
}

/// A scratch path beside the original: same volume, so the rename is atomic,
/// and named so a crashed run's leftovers are identifiable.
fn temp_path_for(path: &Path) -> Result<PathBuf, TagError> {
    let dir = path.parent().ok_or_else(|| {
        TagError::Unsupported(format!("{} has no parent directory", path.display()))
    })?;
    let stem = path.file_stem().and_then(|n| n.to_str()).ok_or_else(|| {
        TagError::Unsupported(format!("{} has no file stem", path.display()))
    })?;
    // The marker goes before the extension on purpose: the tag backend picks
    // the container from the extension, and `x.flac.neiro-tmp-1` would present
    // an extension of `neiro-tmp-1` and be rejected as an unknown format.
    let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("");
    let suffix = if ext.is_empty() { String::new() } else { format!(".{ext}") };
    Ok(dir.join(format!(".{stem}.neiro-tmp-{}{suffix}", std::process::id())))
}

/// Every family this build knows, in the order a tag editor lists them.
pub fn all_families() -> &'static [TagFamily] {
    &ALL_FAMILIES
}

/// Keys a file of this kind can hold, as text, for a UI to offer.
///
/// The list comes from the format's own mapping, so a control cannot offer a
/// field the file would silently refuse.
pub fn writable_keys(family: TagFamily) -> Vec<(&'static str, ItemKey)> {
    // ID3v1 has no key mapping: it is a fixed block of byte ranges, so the
    // backend reports nothing for it and a UI would show an empty form. The
    // eight fields it does hold are restated here because the backend keeps
    // that list private, and they are not a guess -- they are the whole
    // format.
    if family == TagFamily::Id3v1 {
        return ID3V1_KEYS.to_vec();
    }
    let t = family.to_lofty();
    ItemKey::VARIANTS
        .iter()
        .filter_map(|k| k.map_key(t).map(|name| (name, *k)))
        .collect()
}

/// The whole of ID3v1: eight names, 30 bytes each except the track number.
const ID3V1_KEYS: [(&str, ItemKey); 8] = [
    ("TITLE", ItemKey::TrackTitle),
    ("ARTIST", ItemKey::TrackArtist),
    ("ALBUM", ItemKey::AlbumTitle),
    ("COMMENT", ItemKey::Comment),
    ("TRACKNUMBER", ItemKey::TrackNumber),
    ("GENRE", ItemKey::Genre),
    ("YEAR", ItemKey::Year),
    ("DATE", ItemKey::RecordingDate),
];

#[cfg(test)]
mod tests {
    use super::*;

    /// Every key the backend knows has one identity, and it is unique.
    ///
    /// The wire carries this string, so two keys sharing one would make a write
    /// land on whichever the lookup found first.
    #[test]
    fn every_key_has_one_identity() {
        let mut ids: Vec<String> = Vec::new();
        for key in ItemKey::VARIANTS.iter() {
            let id = key_id(*key);
            assert!(
                !ids.contains(&id),
                "{id} is the identity of two keys: {key:?}"
            );
            assert_eq!(
                key_from_id(&id),
                Some(*key),
                "{id} does not resolve back to {key:?}"
            );
            ids.push(id);
        }
        assert!(!ids.is_empty(), "the backend reports no keys at all");
    }

    /// An identity resolves to the same key in every family, which is what
    /// makes it usable instead of the family's own name.
    #[test]
    fn identity_is_not_the_display_name() {
        // ID3v2 puts the track number and the track total in one TRCK frame.
        // Resolving by the family's name would have to choose between them.
        let number = writable_keys(TagFamily::Id3v2)
            .into_iter()
            .filter(|(name, _)| *name == "TRCK")
            .map(|(_, key)| key)
            .collect::<Vec<_>>();
        assert!(
            number.len() > 1,
            "the fixture assumption is gone: TRCK no longer names two keys"
        );
        assert_eq!(
            key_id(ItemKey::TrackNumber),
            "TrackNumber",
            "the identity is the variant name"
        );
        assert_ne!(
            key_id(ItemKey::TrackNumber),
            key_id(ItemKey::TrackTotal),
            "the two keys in one frame must not share an identity"
        );
    }

    /// A field read from a file reports the identity and the name together, and
    /// they agree for the family that produced them.
    #[test]
    fn a_field_carries_its_identity_and_its_name() {
        let entry = TagEntry {
            family: TagFamily::Id3v2,
            key: ItemKey::TrackArtist,
            value: ItemValue::Text("Elin Vahl".into()),
        };
        let value = TagFieldValue::from_entry(&entry);
        assert_eq!(value.key, "TrackArtist");
        assert_eq!(value.name, "TPE1");
        assert_eq!(value.value.as_deref(), Some("Elin Vahl"));

        // And it comes back as the same key, which is what a write needs.
        let back = fields_from_values(&[value]).expect("resolves");
        assert_eq!(back.len(), 1);
        assert_eq!(back[0].key, ItemKey::TrackArtist);
    }

    /// An identity nothing answers to is refused, not ignored.
    #[test]
    fn an_unknown_identity_is_refused() {
        let bad = TagFieldValue {
            family: TagFamily::Id3v2,
            key: "NotAField".into(),
            name: String::new(),
            value: Some("x".into()),
        };
        assert!(fields_from_values(&[bad]).is_err(), "accepted an unknown key");
    }

    /// The base64 a host receives decodes to the bytes the file holds.
    ///
    /// Pinned against the published vectors rather than against this crate's
    /// own output: a round trip through the same code would pass whatever it
    /// produced, including a wrong alphabet.
    #[test]
    fn artwork_encodes_as_standard_base64() {
        let cases: [(&[u8], &str); 4] = [
            (b"", ""),
            (b"f", "Zg=="),
            (b"fo", "Zm8="),
            (b"foob", "Zm9vYg=="),
        ];
        for (data, want) in cases {
            let art = Artwork {
                family: TagFamily::VorbisComments,
                kind: PictureType::CoverFront,
                mime: Some("image/jpeg".into()),
                description: None,
                data: data.to_vec(),
            };
            assert_eq!(art.to_base64(), want, "encoding {data:?}");
        }
    }

    /// The listing carries the role and the size, which is what a tag editor
    /// shows before the bytes are asked for.
    #[test]
    fn artwork_info_describes_without_the_bytes() {
        let art = Artwork {
            family: TagFamily::VorbisComments,
            kind: PictureType::CoverBack,
            mime: Some("image/png".into()),
            description: Some("inner sleeve".into()),
            data: vec![0u8; 1234],
        };
        let info = art.info();
        assert_eq!(info.kind, "CoverBack");
        assert_eq!(info.mime.as_deref(), Some("image/png"));
        assert_eq!(info.description.as_deref(), Some("inner sleeve"));
        assert_eq!(info.bytes, 1234);
    }
}
