//! Jellyfin: a remote library, fetched into local files.
//!
//! The engine plays paths. It decodes from a file, its own DSD path opens a
//! file, its analyser decodes a file, and the library keys everything on a
//! path. Handing it `http://...` would work for plain decoding and quietly fail
//! everywhere else -- DSD would fall back to the generic decoder, the analysis
//! cache would lose its key (a URL has no size and no mtime), and the render
//! path would still claim bit-perfect about a stream nobody measured.
//!
//! So a remote track is downloaded first and then played as a file. Every
//! guarantee the engine makes stays true, because nothing above this module
//! knows the file arrived over the network.
//!
//! # Why the cache path is derived from the item id
//!
//! Favourites and playlists are stored by path. A path that changed between
//! runs would orphan every one of them, so the name is `<id>.<container>`: the
//! id is Jellyfin's own stable identity, and the container is what the server
//! says the file is. Two downloads of one track land on the same name.

use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};

/// Where the server is and who we are.
///
/// Two protocols, because the two servers speak different ones. Jellyfin has
/// its own REST API and a token header; Navidrome speaks Subsonic, where every
/// call is a query string carrying a per-request salt and an MD5 of the
/// password plus that salt.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Server {
    Jellyfin {
        /// Scheme and authority, no trailing slash: `http://192.168.1.10:8096`.
        base_url: String,
        auth: JellyfinAuth,
    },
    Navidrome {
        base_url: String,
        user: String,
        password: String,
    },
}


/// How a Jellyfin client proves who it is.
///
/// Two forms because the server has two: a key minted in the dashboard, or a
/// username and password exchanged for an access token. The exchange is done
/// once when the server is configured and the result replaced in place, so
/// nothing below this point ever sees a password.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum JellyfinAuth {
    /// An API key from the dashboard, or a token from an earlier sign-in.
    Token { token: String },
    /// A username and password. Answered with a token before anything else is
    /// tried: a wrong password must fail at connect time, not on the first
    /// listing.
    Password { user: String, password: String },
}

impl Server {
    /// The same config without the secret, for logging and events.
    ///
    /// A token in an event stream is a token in a log file: a host that shows
    /// the connection status must not be able to leak it by accident.
    pub fn redacted(&self) -> Self {
        match self {
            Self::Jellyfin { base_url, .. } => Self::Jellyfin {
                base_url: base_url.clone(),
                auth: JellyfinAuth::Token {
                    token: String::from("***"),
                },
            },
            Self::Navidrome { base_url, user, .. } => Self::Navidrome {
                base_url: base_url.clone(),
                user: user.clone(),
                password: String::from("***"),
            },
        }
    }

    pub fn base_url(&self) -> &str {
        match self {
            Self::Jellyfin { base_url, .. } | Self::Navidrome { base_url, .. } => base_url,
        }
    }
}


impl Server {
    /// The same server with its credentials resolved into a token.
    ///
    /// Jellyfin's password sign-in happens here, once, so every later call has
    /// a token and no call carries a password. A wrong password is reported at
    /// connect time -- the alternative is a connection that looks fine until
    /// the first listing fails for a reason the user cannot see.
    pub fn resolve(self) -> Result<Self, RemoteError> {
        match self {
            Self::Jellyfin {
                base_url,
                auth: JellyfinAuth::Password { user, password },
            } => {
                let token = jellyfin_sign_in(&base_url, &user, &password)?;
                Ok(Self::Jellyfin {
                    base_url,
                    auth: JellyfinAuth::Token { token },
                })
            }
            other => Ok(other),
        }
    }
}

/// Exchange a username and password for an access token.
///
/// The header names the client to the server's device list; Jellyfin rejects a
/// sign-in that does not say who is asking. `X-Emby-Authorization` is the
/// legacy spelling and is sent alongside, for the same reason the token header
/// is sent twice elsewhere: the server has accepted either across versions.
fn jellyfin_sign_in(base_url: &str, user: &str, password: &str) -> Result<String, RemoteError> {
    let url = format!("{}/Users/AuthenticateByName", base_url.trim_end_matches('/'));
    let claim = "MediaBrowser Client=\"neiro\", Device=\"desktop\", DeviceId=\"neiro-base\", Version=\"0.1\"";
    let body = serde_json::json!({ "Username": user, "Pw": password }).to_string();
    let mut response = ureq::post(&url)
        .header("Authorization", claim)
        .header("X-Emby-Authorization", claim)
        .header("Content-Type", "application/json")
        .send(body)
        .map_err(transport)?;
    let status = response.status().as_u16();
    let text = response
        .body_mut()
        .read_to_string()
        .map_err(|e| RemoteError::Transport(e.to_string()))?;
    if !(200..300).contains(&status) {
        return Err(RemoteError::Status { code: status, body: text });
    }
    let parsed: serde_json::Value =
        serde_json::from_str(&text).map_err(|e| RemoteError::Shape(e.to_string()))?;
    parsed
        .get("AccessToken")
        .and_then(|v| v.as_str())
        .map(|t| t.to_owned())
        .ok_or_else(|| RemoteError::Shape("the sign-in answered without an AccessToken".into()))
}

/// One audio item as the server describes it.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct RemoteTrack {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    /// The file's extension according to the server: `flac`, `mp3`, `dsf`.
    pub container: String,
    /// Ticks of 100 ns, Jellyfin's own unit, converted to milliseconds here so
    /// no consumer has to know the unit.
    pub duration_ms: u64,
    pub size: Option<u64>,
    /// The id the server uses for this track's cover, when it is not the track
    /// id itself. Jellyfin serves the art from the item, Subsonic from a
    /// separate `coverArt` id.
    #[serde(default)]
    pub cover_art: Option<String>,
}

/// A playlist on the server.
///
/// `id` is the server's own; it means nothing locally. A host that wants the
/// entries asks for them by it, and `track_count` is what the server says it
/// holds -- which is not how many a page returns.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct RemotePlaylist {
    pub id: String,
    pub name: String,
    pub track_count: u64,
    pub duration_ms: u64,
}

/// What this module can fail at.
#[derive(Debug)]
pub enum RemoteError {
    /// The server answered, and said no.
    Status { code: u16, body: String },
    /// No answer, or the connection failed.
    Transport(String),
    /// The answer was not what the API documents.
    Shape(String),
    Io(std::io::Error),
}

impl std::fmt::Display for RemoteError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Status { code, body } => write!(f, "server said {code}: {body}"),
            Self::Transport(s) => write!(f, "transport: {s}"),
            Self::Shape(s) => write!(f, "unexpected answer: {s}"),
            Self::Io(e) => write!(f, "io: {e}"),
        }
    }
}

impl std::error::Error for RemoteError {}

impl From<std::io::Error> for RemoteError {
    fn from(e: std::io::Error) -> Self {
        Self::Io(e)
    }
}

/// The headers every call carries.
///
/// Both spellings of the token header, because Jellyfin has accepted two across
/// its versions and a client that picks one fails on the other half. This is
/// protocol compatibility, not a second opinion about a value: the server
/// decides whether either is good.
fn jellyfin_request(
    token: &str,
    url: &str,
) -> ureq::RequestBuilder<ureq::typestate::WithoutBody> {
    ureq::get(url)
        .header("Authorization", &format!("MediaBrowser Token=\"{token}\""))
        .header("X-Emby-Token", token)
        .header("Accept", "application/json")
}


/// The signed-in user's id, which `/Items` requires.
///
/// A Jellyfin newer than 10.9 answers `400` to a listing without it, and a
/// token carries no user: one call to `/Users/Me` answers, and every later
/// listing reuses it.
fn jellyfin_user_id(base_url: &str, token: &str) -> Result<String, RemoteError> {
    static CACHE: std::sync::OnceLock<std::sync::Mutex<std::collections::HashMap<String, String>>> =
        std::sync::OnceLock::new();
    let cache = CACHE.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()));
    let key = format!("{}|{token}", base_url.trim_end_matches('/'));
    if let Ok(guard) = cache.lock() {
        if let Some(id) = guard.get(&key) {
            return Ok(id.clone());
        }
    }
    let url = format!("{}/Users/Me", base_url.trim_end_matches('/'));
    let mut response = jellyfin_request(token, &url).call().map_err(transport)?;
    let status = response.status().as_u16();
    let body = response
        .body_mut()
        .read_to_string()
        .map_err(|e| RemoteError::Transport(e.to_string()))?;
    if !(200..300).contains(&status) {
        return Err(RemoteError::Status { code: status, body });
    }
    let parsed: serde_json::Value =
        serde_json::from_str(&body).map_err(|e| RemoteError::Shape(e.to_string()))?;
    let id = parsed
        .get("Id")
        .and_then(|v| v.as_str())
        .ok_or_else(|| RemoteError::Shape("no user Id".into()))?
        .to_string();
    if let Ok(mut guard) = cache.lock() {
        guard.insert(key, id.clone());
    }
    Ok(id)
}

/// Turn a ureq failure into this module's error, keeping the server's body.
fn transport(e: ureq::Error) -> RemoteError {
    match e {
        ureq::Error::StatusCode(code) => RemoteError::Status {
            code,
            body: String::new(),
        },
        other => RemoteError::Transport(other.to_string()),
    }
}

/// List audio items.
///
/// `start` and `limit` are passed through rather than paged here: the caller
/// knows how much it wants to show, and a client that silently capped the list
/// would look like a library with fewer tracks.
pub fn list_audio(
    server: &Server,
    start: usize,
    limit: usize,
) -> Result<Vec<RemoteTrack>, RemoteError> {
    match server {
        Server::Jellyfin { base_url, auth } => {
            let token = match auth {
                JellyfinAuth::Token { token } => token.as_str(),
                JellyfinAuth::Password { .. } => {
                    return Err(RemoteError::Shape(
                        "the server was configured with a password but never resolved".into(),
                    ))
                }
            };
            let user_id = jellyfin_user_id(base_url, token)?;
            let url = format!(
                "{}/Items?IncludeItemTypes=Audio&Recursive=true&Fields=MediaSources,Path\
                 &SortBy=SortName&EnableImages=false&StartIndex={start}&Limit={limit}&userId={user_id}",
                base_url.trim_end_matches('/')
            );
            let mut response = jellyfin_request(token, &url).call().map_err(transport)?;
            let status = response.status().as_u16();
            let body = response
                .body_mut()
                .read_to_string()
                .map_err(|e| RemoteError::Transport(e.to_string()))?;
            if !(200..300).contains(&status) {
                return Err(RemoteError::Status { code: status, body });
            }
            let parsed: serde_json::Value =
                serde_json::from_str(&body).map_err(|e| RemoteError::Shape(e.to_string()))?;
            let items = parsed
                .get("Items")
                .and_then(|v| v.as_array())
                .ok_or_else(|| RemoteError::Shape("no Items array".into()))?;
            Ok(items.iter().filter_map(track_from_json).collect())
        }
        Server::Navidrome { .. } => {
            // `search3` with an empty query returns everything the user can see
            // on Navidrome; it is the one call that yields songs rather than
            // albums, and a song is what this layer plays.
            let envelope = subsonic_get(
                server,
                "search3",
                &[
                    ("query", String::new()),
                    ("songCount", limit.to_string()),
                    ("songOffset", start.to_string()),
                    ("artistCount", "0".into()),
                    ("albumCount", "0".into()),
                ],
            )?;
            let songs = envelope
                .pointer("/searchResult3/song")
                .and_then(|v| v.as_array())
                .ok_or_else(|| RemoteError::Shape("no searchResult3.song array".into()))?;
            Ok(songs.iter().filter_map(navidrome_track).collect())
        }
    }
}

/// One item, or an error naming what the answer was missing.
fn track_from_json(item: &serde_json::Value) -> Option<RemoteTrack> {
    let id = item.get("Id")?.as_str()?.to_owned();
    let media = item
        .get("MediaSources")
        .and_then(|v| v.as_array())
        .and_then(|a| a.first());

    // Jellyfin reports the container per media source; the item's own
    // `Container` is the fallback for servers that omit the source list.
    let container = media
        .and_then(|m| m.get("Container"))
        .and_then(|v| v.as_str())
        .or_else(|| item.get("Container").and_then(|v| v.as_str()))
        .unwrap_or("bin")
        .to_ascii_lowercase();

    let size = media
        .and_then(|m| m.get("Size"))
        .and_then(|v| v.as_u64());

    // Ticks are 100 ns each; the API calls this field `RunTimeTicks`.
    let duration_ms = item
        .get("RunTimeTicks")
        .and_then(|v| v.as_u64())
        .map(|ticks| ticks / 10_000)
        .unwrap_or(0);

    Some(RemoteTrack {
        // Jellyfin serves a track's art from the item's own id.
        cover_art: Some(id.clone()),
        title: item
            .get("Name")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
        artist: item
            .get("AlbumArtist")
            .and_then(|v| v.as_str())
            .or_else(|| {
                item.get("Artists")
                    .and_then(|v| v.as_array())
                    .and_then(|a| a.first())
                    .and_then(|v| v.as_str())
            })
            .unwrap_or("")
            .to_owned(),
        album: item
            .get("Album")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
        id,
        container,
        duration_ms,
        size,
    })
}

/// The file a downloaded item lives at.
///
/// Derived from the id and the container and nothing else, so it is the same
/// name on every run. A path that moved would orphan the favourite and the
/// playlist entries that name it.
pub fn cache_path(dest: &Path, track: &RemoteTrack) -> PathBuf {
    dest.join(format!("{}.{}", safe(&track.id), safe(&track.container)))
}

/// Keep a name usable as one path component.
///
/// Jellyfin ids are hex, but a server is a remote party and this is the one
/// place its strings become a path. Anything that could climb out of the cache
/// directory is replaced rather than trusted.
fn safe(text: &str) -> String {
    let cleaned: String = text
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .collect();
    if cleaned.is_empty() {
        String::from("item")
    } else {
        cleaned
    }
}

/// Download one item into `dest`, returning its path.
///
/// The bytes go to a sibling `.part` first and are renamed into place only when
/// the transfer finished and, when the server said how long the body would be,
/// when the length agrees. A partial file that carried the final name would be
/// played as if it were whole.
pub fn download(
    server: &Server,
    track: &RemoteTrack,
    dest: &Path,
) -> Result<PathBuf, RemoteError> {
    fs::create_dir_all(dest)?;
    let final_path = cache_path(dest, track);
    let part = final_path.with_extension(format!(
        "{}.part",
        final_path
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("bin")
    ));

    let (status, announced, bytes) = match server {
        Server::Jellyfin { base_url, auth } => {
            let token = match auth {
                JellyfinAuth::Token { token } => token.as_str(),
                JellyfinAuth::Password { .. } => {
                    return Err(RemoteError::Shape(
                        "the server was configured with a password but never resolved".into(),
                    ))
                }
            };
            let url = format!(
                "{}/Items/{}/Download",
                base_url.trim_end_matches('/'),
                track.id
            );
            let mut response = jellyfin_request(token, &url)
                .header("Accept", "*/*")
                .call()
                .map_err(transport)?;
            let status = response.status().as_u16();
            let announced = response.body().content_length();
            if !(200..300).contains(&status) {
                let body = response.body_mut().read_to_string().unwrap_or_default();
                return Err(RemoteError::Status { code: status, body });
            }
            let bytes = read_whole_body(response.body_mut())?;
            (status, announced, bytes)
        }
        Server::Navidrome { .. } => {
            // `format=raw` is the only way to ask Subsonic for the file as it
            // is stored. Without it the server may transcode to the client's
            // advertised codec, which would make every bit-perfect claim about
            // this track false while looking like a normal download.
            let envelope_url = subsonic_url(
                server,
                "stream",
                &[
                    ("id", track.id.clone()),
                    ("format", "raw".into()),
                    ("maxBitRate", "0".into()),
                ],
            )?;
            let mut response = ureq::get(&envelope_url).call().map_err(transport)?;
            let status = response.status().as_u16();
            if !(200..300).contains(&status) {
                let body = response.body_mut().read_to_string().unwrap_or_default();
                return Err(RemoteError::Status { code: status, body });
            }
            // A Subsonic server reports a refused stream inside a 200 whose
            // body is a JSON error document, not audio. Reading that as bytes
            // writes the error into the cache under a `.flac` name and calls it
            // a successful download, which is exactly the failure this whole
            // module exists to avoid.
            let content_type = response
                .body()
                .mime_type()
                .unwrap_or("")
                .to_ascii_lowercase();
            if content_type.starts_with("application/json")
                || content_type.starts_with("text/")
            {
                let body = response.body_mut().read_to_string().unwrap_or_default();
                let parsed: serde_json::Value = serde_json::from_str(&body).unwrap_or_default();
                let code = parsed
                    .pointer("/subsonic-response/error/code")
                    .and_then(|v| v.as_i64())
                    .unwrap_or(0) as u16;
                let message = parsed
                    .pointer("/subsonic-response/error/message")
                    .and_then(|v| v.as_str())
                    .unwrap_or(&body)
                    .to_owned();
                return Err(RemoteError::Status { code, body: message });
            }
            let announced = response.body().content_length();
            let bytes = read_whole_body(response.body_mut())?;
            (status, announced, bytes)
        }
    };
    let _ = status;

    if let Some(announced) = announced {
        if announced != bytes.len() as u64 {
            return Err(RemoteError::Shape(format!(
                "the server announced {announced} bytes and sent {}",
                bytes.len()
            )));
        }
    }
    if bytes.is_empty() {
        return Err(RemoteError::Shape("the download was empty".into()));
    }
    if let Some(size) = track.size {
        if size != bytes.len() as u64 {
            return Err(RemoteError::Shape(format!(
                "the catalogue says {size} bytes and the server sent {}",
                bytes.len()
            )));
        }
    }

    {
        let mut file = fs::File::create(&part)?;
        file.write_all(&bytes)?;
        file.flush()?;
        file.sync_all()?;
    }
    fs::rename(&part, &final_path)?;
    Ok(final_path)
}


// ---- Subsonic / Navidrome ------------------------------------------------
//
// Every call is a query string carrying the client id, the protocol version,
// a fresh salt and `md5(password + salt)`. The salt is per request on purpose:
// it means a captured URL cannot be replayed with a different one, and it is
// why nothing here caches a signed URL.

/// The client id and protocol version this build claims.
///
/// `c` identifies the client to the server's logs; `v` is the schema version
/// this code was written against. Both are required and neither is a secret.
const SUBSONIC_CLIENT: &str = "neiro";
const SUBSONIC_VERSION: &str = "1.16.1";

/// `md5(password + salt)`, lower-case hex, as the protocol defines the token.
///
/// The concatenation is UTF-8 on both sides; the checkpoint in the tests is the
/// vector from the OpenSubsonic reference, not this function's own output.
pub fn subsonic_token(password: &str, salt: &str) -> String {
    use md5::{Digest, Md5};
    let mut hasher = Md5::new();
    hasher.update(password.as_bytes());
    hasher.update(salt.as_bytes());
    hasher
        .finalize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// A salt for one request.
///
/// Six characters is the protocol minimum. Derived from the clock and the id of
/// the request rather than from a random source: it only has to differ between
/// requests, and a dependency on an OS random generator would be a dependency
/// for nothing.
fn subsonic_salt() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let mixed = nanos ^ (std::process::id() as u128) << 32;
    format!("{:012x}", mixed & 0xffff_ffff_ffff)
}

/// The signed URL for one endpoint.
fn subsonic_url(
    server: &Server,
    endpoint: &str,
    params: &[(&str, String)],
) -> Result<String, RemoteError> {
    let Server::Navidrome {
        base_url,
        user,
        password,
    } = server
    else {
        return Err(RemoteError::Shape("not a Subsonic server".into()));
    };
    let salt = subsonic_salt();
    let token = subsonic_token(password, &salt);
    let mut url = format!(
        "{}/rest/{endpoint}.view?u={}&t={}&s={}&v={}&c={}&f=json",
        base_url.trim_end_matches('/'),
        urlencode(user),
        token,
        salt,
        SUBSONIC_VERSION,
        SUBSONIC_CLIENT
    );
    for (key, value) in params {
        url.push('&');
        url.push_str(key);
        url.push('=');
        url.push_str(&urlencode(value));
    }
    Ok(url)
}

/// Minimal percent-encoding for a query component.
///
/// Enough for the characters a username, id or query can contain. The reserved
/// set is what `application/x-www-form-urlencoded` calls unreserved plus the
/// characters that would end a component.
fn urlencode(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for byte in text.as_bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => {
                out.push(*byte as char)
            }
            _ => out.push_str(&format!("%{byte:02X}")),
        }
    }
    out
}

/// Call one endpoint and unwrap `subsonic-response`, or report what it said.
fn subsonic_get(
    server: &Server,
    endpoint: &str,
    params: &[(&str, String)],
) -> Result<serde_json::Value, RemoteError> {
    let url = subsonic_url(server, endpoint, params)?;
    let mut response = ureq::get(&url).call().map_err(transport)?;
    let status = response.status().as_u16();
    let body = response
        .body_mut()
        .read_to_string()
        .map_err(|e| RemoteError::Transport(e.to_string()))?;
    let parsed: serde_json::Value = serde_json::from_str(&body)
        .map_err(|e| RemoteError::Shape(format!("{e} (HTTP {status})")))?;
    let envelope = parsed
        .get("subsonic-response")
        .ok_or_else(|| RemoteError::Shape("no subsonic-response".into()))?;
    // The protocol reports a refused call inside a 200 body, so the status
    // field is the answer, not the HTTP code.
    if envelope.get("status").and_then(|v| v.as_str()) != Some("ok") {
        let code = envelope
            .pointer("/error/code")
            .and_then(|v| v.as_i64())
            .unwrap_or(0);
        let message = envelope
            .pointer("/error/message")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned();
        return Err(RemoteError::Status {
            code: code as u16,
            body: message,
        });
    }
    Ok(envelope.clone())
}

/// One song from `search3`.
fn navidrome_track(song: &serde_json::Value) -> Option<RemoteTrack> {
    let id = song.get("id")?.as_str()?.to_owned();
    // `suffix` is the file's own extension: "flac", "mp3". Without it the cache
    // name would have no extension and the decoder would have to guess.
    let container = song
        .get("suffix")
        .and_then(|v| v.as_str())
        .unwrap_or("bin")
        .to_ascii_lowercase();
    Some(RemoteTrack {
        id,
        title: song.get("title").and_then(|v| v.as_str()).unwrap_or("").to_owned(),
        artist: song
            .get("artist")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
        album: song.get("album").and_then(|v| v.as_str()).unwrap_or("").to_owned(),
        container,
        cover_art: song
            .get("coverArt")
            .and_then(|v| v.as_str())
            .map(|s| s.to_owned()),
        // Subsonic reports whole seconds.
        duration_ms: song
            .get("duration")
            .and_then(|v| v.as_u64())
            .unwrap_or(0)
            * 1000,
        size: song.get("size").and_then(|v| v.as_u64()),
    })
}


/// The largest body a download may be.
///
/// A track is tens of megabytes; the client's default cap is 10 MiB, so a
/// perfectly ordinary FLAC fails with "the response body is larger than request
/// limit" -- measured against a real server, where a 40 MB file was refused.
/// A gibibyte is far above any single track and still a bound.
const MAX_DOWNLOAD_BYTES: u64 = 1 << 30;

/// Read a body whole, with the download cap rather than the default one.
fn read_whole_body(body: &mut ureq::Body) -> Result<Vec<u8>, RemoteError> {
    body.with_config()
        .limit(MAX_DOWNLOAD_BYTES)
        .read_to_vec()
        .map_err(|e| RemoteError::Transport(e.to_string()))
}



/// Covers already downloaded, in memory in front of a directory on disk.
///
/// A browser walks a library and asks for one cover per row, and Jellyfin
/// serves an album's art from every item in it, so without this the same
/// picture is fetched once per track. The key is the server's cover id, which
/// is per album on both backends.
///
/// Two layers because they answer different questions. Memory makes a second
/// look free within one run; the directory makes the *first* look after a
/// restart free, which is the one that costs the listener bandwidth.
///
/// # Why the directory is keyed by server
///
/// A cover id is only meaningful on the server that issued it. Two servers can
/// both have `cover-1` and mean different pictures, so the files live under
/// `<cache_dir>/artwork/<server>/`. A host that re-sends `set_remote` on every
/// launch therefore keeps its cache -- wiping it on configure would make the
/// persistence useless for the common case.
pub struct ArtworkCache {
    map: std::collections::HashMap<String, RemoteArtwork>,
    order: std::collections::VecDeque<String>,
    max_entries: usize,
    /// Where the pictures live between runs, when the host gave a cache
    /// directory. Absent means memory only.
    dir: Option<PathBuf>,
    /// Files the directory holds, so the bound can be enforced without listing
    /// it on every write. Only this code writes there.
    files: usize,
    max_files: usize,
}

impl std::fmt::Debug for ArtworkCache {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ArtworkCache")
            .field("memory", &self.map.len())
            .field("files", &self.files)
            .field("dir", &self.dir)
            .finish()
    }
}

impl Default for ArtworkCache {
    /// Sixty-four covers in memory. Derived `Default` would have given a
    /// capacity of zero -- a cache that silently stores nothing, which it did
    /// until the probe counted requests instead of checking a return value.
    fn default() -> Self {
        Self::with_capacity(64)
    }
}

/// A name a cover is stored under, and the way back to its type.
///
/// The mime is part of the file name rather than in a sidecar or a second
/// index: one file, nothing to keep in step, and the round trip is exact for
/// anything a server sends. `image/png` becomes the extension `image_png`.
fn artwork_file_name(id: &str, mime: &str) -> String {
    let safe_mime: String = mime
        .chars()
        .map(|c| if c == '/' { '_' } else if c.is_ascii_alphanumeric() || c == '-' || c == '.' { c } else { '_' })
        .collect();
    format!("{}.{}", safe(id), safe_mime)
}

/// The type a stored file name encodes, if it encodes one.
fn artwork_mime(name: &str) -> Option<String> {
    let (_, extension) = name.rsplit_once('.')?;
    let mime = extension.replacen('_', "/", 1);
    // A name with no separator is not a mime this wrote.
    mime.contains('/').then_some(mime)
}

impl ArtworkCache {
    pub fn with_capacity(max_entries: usize) -> Self {
        Self {
            map: Default::default(),
            order: Default::default(),
            max_entries: max_entries.max(1),
            dir: None,
            files: 0,
            max_files: 512,
        }
    }

    /// Keep the pictures in `dir` between runs.
    ///
    /// The directory is created on first write. What it already holds counts
    /// towards the bound, so a cache left behind by a previous run is not
    /// allowed to grow past it.
    pub fn with_dir(mut self, dir: PathBuf) -> Self {
        let count = fs::read_dir(&dir)
            .map(|entries| entries.flatten().count())
            .unwrap_or(0);
        self.files = count;
        self.dir = Some(dir);
        self
    }

    pub fn get(&mut self, id: &str) -> Option<RemoteArtwork> {
        if let Some(value) = self.map.get(id) {
            let value = value.clone();
            self.touch(id);
            return Some(value);
        }
        // Disk is the second layer: a restart still has the picture, and the
        // bytes are the only thing that has to survive.
        let dir = self.dir.clone()?;
        let entries = fs::read_dir(&dir).ok()?;
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            if artwork_stem(&name) != safe(id) {
                continue;
            }
            let mime = artwork_mime(&name).unwrap_or_else(|| "application/octet-stream".into());
            let data = fs::read(entry.path()).ok()?;
            let art = RemoteArtwork { mime, data };
            self.put_memory(id, art.clone());
            return Some(art);
        }
        None
    }

    pub fn put(&mut self, id: &str, art: RemoteArtwork) {
        self.put_memory(id, art.clone());
        let Some(dir) = self.dir.clone() else {
            return;
        };
        if fs::create_dir_all(&dir).is_err() {
            return;
        }
        let path = dir.join(artwork_file_name(id, &art.mime));
        let existed = path.exists();
        // A failed write is not fatal: the memory layer is still correct for
        // this run, and the next one re-fetches.
        if fs::write(&path, &art.data).is_err() {
            return;
        }
        if !existed {
            self.files += 1;
            self.prune();
        }
    }

    /// Forget what is in memory. The disk layer stays: it is keyed by server
    /// and is the point of the whole thing.
    pub fn clear(&mut self) {
        self.map.clear();
        self.order.clear();
    }

    fn put_memory(&mut self, id: &str, art: RemoteArtwork) {
        if !self.map.contains_key(id) {
            while self.map.len() >= self.max_entries {
                match self.order.pop_front() {
                    Some(old) => {
                        self.map.remove(&old);
                    }
                    None => break,
                }
            }
        }
        self.map.insert(id.to_owned(), art);
        self.touch(id);
    }

    fn touch(&mut self, id: &str) {
        if let Some(pos) = self.order.iter().position(|k| k == id) {
            self.order.remove(pos);
        }
        self.order.push_back(id.to_owned());
    }

    /// Drop the oldest files until the directory is back under its bound.
    ///
    /// Oldest by modification time, which this code sets on write: an entry
    /// only moves to the end when it is fetched again, so the least recently
    /// *stored* goes first.
    fn prune(&mut self) {
        let Some(dir) = self.dir.clone() else {
            return;
        };
        if self.files <= self.max_files {
            return;
        }
        let mut entries: Vec<(std::time::SystemTime, PathBuf)> = fs::read_dir(&dir)
            .map(|entries| {
                entries
                    .flatten()
                    .filter_map(|e| {
                        let modified = e.metadata().ok()?.modified().ok()?;
                        Some((modified, e.path()))
                    })
                    .collect()
            })
            .unwrap_or_default();
        entries.sort();
        let target = self.max_files * 9 / 10;
        for (_, path) in entries.iter().take(self.files.saturating_sub(target)) {
            if fs::remove_file(path).is_ok() {
                self.files = self.files.saturating_sub(1);
            }
        }
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.map.len()
    }
}


/// A short, stable directory name for one server.
///
/// FNV-1a, hex. Not a security boundary -- the credentials are not in it, only
/// the address and the protocol -- so a short non-cryptographic hash is enough
/// and keeps the path readable.
fn server_dir(key: &str) -> String {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for byte in key.as_bytes() {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    format!("{hash:016x}")
}

/// The part of a stored file name before its type.
fn artwork_stem(name: &str) -> &str {
    name.rsplit_once('.').map(|(stem, _)| stem).unwrap_or(name)
}

/// One picture, as a server hands it over.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RemoteArtwork {
    pub mime: String,
    pub data: Vec<u8>,
}

/// The cover for one track, or `None` when the server says there is none.
///
/// `None` and an error are different answers: a track without art is a normal
/// track, and a failed request is a failed request.
pub fn fetch_artwork(server: &Server, track: &RemoteTrack) -> Result<Option<RemoteArtwork>, RemoteError> {
    let Some(id) = track.cover_art.clone() else {
        return Ok(None);
    };
    match server {
        Server::Jellyfin { base_url, auth } => {
            let token = token_of(auth)?;
            // A ceiling, because a host scales it anyway and a 4000px cover is
            // a megabyte over the wire for a 200px thumbnail.
            let url = format!(
                "{}/Items/{}/Images/Primary?maxHeight=600&quality=90",
                base_url.trim_end_matches('/'),
                id
            );
            let mut response = match jellyfin_request(token, &url).call() {
                // 404 is the honest "this track has no picture", not a failure.
                Ok(response) => response,
                Err(ureq::Error::StatusCode(404)) => return Ok(None),
                Err(e) => return Err(transport(e)),
            };
            let status = response.status().as_u16();
            if !(200..300).contains(&status) {
                return Err(RemoteError::Status {
                    code: status,
                    body: String::new(),
                });
            }
            let mime = response
                .body()
                .mime_type()
                .unwrap_or("image/jpeg")
                .to_owned();
            let data = read_whole_body(response.body_mut())?;
            Ok(Some(RemoteArtwork { mime, data }))
        }
        Server::Navidrome { .. } => {
            let url = subsonic_url(server, "getCoverArt", &[("id", id), ("size", "600".into())])?;
            let mut response = match ureq::get(&url).call() {
                Ok(response) => response,
                Err(ureq::Error::StatusCode(404)) => return Ok(None),
                Err(e) => return Err(transport(e)),
            };
            let status = response.status().as_u16();
            if !(200..300).contains(&status) {
                return Err(RemoteError::Status { code: status, body: String::new() });
            }
            // The same trap as `stream`: a refusal is a JSON document inside a
            // 200, and saving it as a picture succeeds.
            let mime = response.body().mime_type().unwrap_or("image/jpeg").to_string();
            let data = read_whole_body(response.body_mut())?;
            if mime.starts_with("application/json") || mime.starts_with("text/") {
                let text = String::from_utf8_lossy(&data).into_owned();
                let parsed: serde_json::Value = serde_json::from_str(&text).unwrap_or_default();
                let code = parsed
                    .pointer("/subsonic-response/error/code")
                    .and_then(|v| v.as_i64())
                    .unwrap_or(0) as u16;
                return Err(RemoteError::Status { code, body: text });
            }
            Ok(Some(RemoteArtwork { mime, data }))
        }
    }
}

/// The playlists the server holds.
pub fn list_playlists(server: &Server) -> Result<Vec<RemotePlaylist>, RemoteError> {
    match server {
        Server::Jellyfin { base_url, auth } => {
            let token = token_of(auth)?;
            let user_id = jellyfin_user_id(base_url, token)?;
            let url = format!(
                "{}/Items?IncludeItemTypes=Playlist&Recursive=true&Fields=ChildCount&EnableImages=false&userId={user_id}",
                base_url.trim_end_matches('/')
            );
            let mut response = jellyfin_request(token, &url).call().map_err(transport)?;
            let status = response.status().as_u16();
            let body = response.body_mut().read_to_string().map_err(|e| RemoteError::Transport(e.to_string()))?;
            if !(200..300).contains(&status) {
                return Err(RemoteError::Status { code: status, body });
            }
            let parsed: serde_json::Value =
                serde_json::from_str(&body).map_err(|e| RemoteError::Shape(e.to_string()))?;
            let items = parsed
                .get("Items")
                .and_then(|v| v.as_array())
                .ok_or_else(|| RemoteError::Shape("no Items array".into()))?;
            Ok(items
                .iter()
                .filter_map(|item| {
                    Some(RemotePlaylist {
                        id: item.get("Id")?.as_str()?.to_owned(),
                        name: item.get("Name").and_then(|v| v.as_str()).unwrap_or("").to_owned(),
                        track_count: item.get("ChildCount").and_then(|v| v.as_u64()).unwrap_or(0),
                        duration_ms: item
                            .get("RunTimeTicks")
                            .and_then(|v| v.as_u64())
                            .map(|t| t / 10_000)
                            .unwrap_or(0),
                    })
                })
                .collect())
        }
        Server::Navidrome { .. } => {
            let envelope = subsonic_get(server, "getPlaylists", &[])?;
            let mut items = envelope
                .pointer("/playlists/playlist")
                .and_then(|v| v.as_array())
                .cloned()
                .unwrap_or_default();
            Ok(items
                .drain(..)
                .filter_map(|p| {
                    Some(RemotePlaylist {
                        id: p.get("id")?.as_str()?.to_owned(),
                        name: p.get("name").and_then(|v| v.as_str()).unwrap_or("").to_owned(),
                        track_count: p.get("songCount").and_then(|v| v.as_u64()).unwrap_or(0),
                        duration_ms: p.get("duration").and_then(|v| v.as_u64()).unwrap_or(0) * 1000,
                    })
                })
                .collect())
        }
    }
}

/// The entries of one playlist.
pub fn playlist_tracks(
    server: &Server,
    id: &str,
    start: usize,
    limit: usize,
) -> Result<Vec<RemoteTrack>, RemoteError> {
    match server {
        Server::Jellyfin { base_url, auth } => {
            let token = token_of(auth)?;
            // `ParentId` rather than `/Playlists/{id}/Items`: the Items
            // endpoint answers the same shape as a listing.
            let user_id = jellyfin_user_id(base_url, token)?;
            let url = format!(
                "{}/Items?ParentId={}&IncludeItemTypes=Audio&Recursive=true&Fields=MediaSources\
                 &StartIndex={start}&Limit={limit}&EnableImages=false&userId={user_id}",
                base_url.trim_end_matches('/'),
                urlencode(id)
            );
            let mut response = jellyfin_request(token, &url).call().map_err(transport)?;
            let status = response.status().as_u16();
            let body = response.body_mut().read_to_string().map_err(|e| RemoteError::Transport(e.to_string()))?;
            if !(200..300).contains(&status) {
                return Err(RemoteError::Status { code: status, body });
            }
            let parsed: serde_json::Value =
                serde_json::from_str(&body).map_err(|e| RemoteError::Shape(e.to_string()))?;
            let items = parsed
                .get("Items")
                .and_then(|v| v.as_array())
                .ok_or_else(|| RemoteError::Shape("no Items array".into()))?;
            Ok(items.iter().filter_map(track_from_json).collect())
        }
        Server::Navidrome { .. } => {
            let envelope = subsonic_get(server, "getPlaylist", &[("id", id.to_owned())])?;
            let mut entries = envelope
                .pointer("/playlist/entry")
                .and_then(|v| v.as_array())
                .cloned()
                .unwrap_or_default();
            Ok(entries
                .drain(..)
                .skip(start)
                .take(limit)
                .filter_map(|song| navidrome_track(&song))
                .collect())
        }
    }
}

/// The token a Jellyfin call needs, or the reason it has none.
fn token_of(auth: &JellyfinAuth) -> Result<&str, RemoteError> {
    match auth {
        JellyfinAuth::Token { token } => Ok(token),
        JellyfinAuth::Password { .. } => Err(RemoteError::Shape(
            "the server was configured with a password but never resolved".into(),
        )),
    }
}

/// The name the server gave this item, for a listing that has no local file.
pub fn describe(track: &RemoteTrack) -> String {
    if track.album.is_empty() {
        format!("{} — {}", track.title, track.artist)
    } else {
        format!("{} — {} — {}", track.title, track.artist, track.album)
    }
}

/// What a host asks the remote worker to do.
///
/// The worker owns the remote session -- the config, the cache directory and
/// the last listing -- because the listing is what a fetch resolves an id
/// against. Two owners of that list would disagree the moment a second listing
/// arrived.
#[derive(Debug)]
pub enum Job {
    /// Point the worker at a server, or at none when `config` is absent.
    Configure {
        config: Option<Server>,
        cache_dir: Option<PathBuf>,
    },
    List {
        start: usize,
        limit: usize,
    },
    /// Download one item of the last listing and report its path.
    Fetch {
        id: String,
    },
    /// The cover of one listed track.
    FetchArtwork {
        id: String,
    },
    /// The server's playlists.
    ListPlaylists,
    /// The entries of one playlist.
    ListPlaylistItems {
        id: String,
        start: usize,
        limit: usize,
    },
}

/// The worker's state.
#[derive(Default)]
pub struct Session {
    config: Option<Server>,
    cache_dir: Option<PathBuf>,
    listing: Vec<RemoteTrack>,
    artwork: ArtworkCache,
    /// The directory the current cache was built for, so configuring twice with
    /// the same server does not throw the pictures away.
    artwork_dir: Option<PathBuf>,
}

/// Change the listing field: a `List` answer merges into the session's
/// accumulated listing by id (a later page's row for the same id wins), so a
/// host that paged through 3,000 items can click any of them. The old rule
/// ("the last listing replaces") made every click outside the last page fail
/// with `no listed track has the id`. The protection worth keeping — an id
/// nobody ever listed resolves to nothing — survives: only ids a real
/// listing carried enter here. `Configure` clears the accumulator, so a
/// server change cannot serve another server's ids.
fn listing_upsert(listing: &mut Vec<RemoteTrack>, tracks: Vec<RemoteTrack>) {
    for track in tracks {
        match listing.iter_mut().find(|known| known.id == track.id) {
            Some(known) => *known = track,
            None => listing.push(track),
        }
    }
}

impl Session {
    /// Run one job, returning the event to publish.
    ///
    /// Each job answers with exactly one event, including the failures: a host
    /// that asked and got nothing back has no way to tell "still working" from
    /// "this host cannot reach the server".
    pub fn run(&mut self, job: Job) -> crate::protocol::Event {
        match job {
            Job::Configure { config, cache_dir } => {
                // A password becomes a token here, once. Every later call has a
                // token and none carries a password.
                let config = match config.map(|c| c.resolve()) {
                    Some(Ok(resolved)) => Some(resolved),
                    Some(Err(e)) => return self.error(&e.to_string()),
                    None => None,
                };
                self.config = config;
                self.cache_dir = cache_dir;
                self.listing.clear();
                // Memory is dropped; the directory is not. It is keyed by
                // server, and a host that re-sends `set_remote` on every launch
                // would otherwise re-download every cover it had.
                self.artwork.clear();
                self.artwork_dir = None;
                crate::protocol::Event::RemoteChanged {
                    configured: self.config.is_some(),
                    kind: self.config.as_ref().map(|c| match c {
                        Server::Jellyfin { .. } => "jellyfin".to_string(),
                        Server::Navidrome { .. } => "navidrome".to_string(),
                    }),
                    base_url: self.config.as_ref().map(|c| c.base_url().to_string()),
                    cache_dir: self.cache_dir.clone(),
                }
            }
            Job::List { start, limit } => match self.config.as_ref() {
                None => self.error("no server is configured"),
                Some(config) => match list_audio(config, start, limit) {
                    Ok(tracks) => {
                        listing_upsert(&mut self.listing, tracks.clone());
                        crate::protocol::Event::RemoteListed { tracks }
                    }
                    Err(e) => self.error(&e.to_string()),
                },
            },
            Job::FetchArtwork { id } => {
                let Some(config) = self.config.clone() else {
                    return self.error("no server is configured");
                };
                let Some(track) = self.listing.iter().find(|t| t.id == id).cloned() else {
                    return self.error(&format!("no listed track has the id {id}"));
                };
                // One round trip per cover, not one per track: Jellyfin serves
                // an album's art from every item in it, so a browser would
                // otherwise fetch the same picture once per row.
                // The disk layer lives under the host's cache directory, in a
                // subdirectory per server: a cover id means nothing on another
                // one, and switching servers must not serve the wrong picture.
                let wanted = self.cache_dir.as_ref().map(|dir| {
                    let server = match &config {
                        Server::Jellyfin { base_url, .. } | Server::Navidrome { base_url, .. } => {
                            format!("{}{}", base_url, match &config {
                                Server::Jellyfin { .. } => "|jellyfin",
                                Server::Navidrome { .. } => "|navidrome",
                            })
                        }
                    };
                    dir.join("artwork").join(server_dir(&server))
                });
                if let Some(dir) = wanted {
                    if self.artwork_dir.as_ref() != Some(&dir) {
                        self.artwork = ArtworkCache::default().with_dir(dir.clone());
                        self.artwork_dir = Some(dir);
                    }
                }
                let cover_id = track.cover_art.clone();
                let cached = cover_id
                    .as_deref()
                    .and_then(|id| self.artwork.get(id));
                let art = match cached {
                    Some(art) => Ok(Some(art)),
                    None => {
                        let fetched = fetch_artwork(&config, &track);
                        // Stored here rather than in a combinator: the write is
                        // the point of the cache, and a closure that runs only
                        // on one path is a thing that silently does not.
                        if let (Ok(Some(art)), Some(id)) = (&fetched, cover_id.as_deref()) {
                            self.artwork.put(id, art.clone());
                        }
                        fetched
                    }
                };
                match art {
                    Ok(Some(art)) => {
                        use base64::Engine as _;
                        crate::protocol::Event::RemoteArtwork {
                            id,
                            mime: Some(art.mime),
                            data: Some(
                                base64::engine::general_purpose::STANDARD.encode(&art.data),
                            ),
                        }
                    }
                    Ok(None) => crate::protocol::Event::RemoteArtwork {
                        id,
                        mime: None,
                        data: None,
                    },
                    Err(e) => self.error(&e.to_string()),
                }
            }
            Job::ListPlaylists => match self.config.as_ref() {
                None => self.error("no server is configured"),
                Some(config) => match list_playlists(config) {
                    Ok(playlists) => crate::protocol::Event::RemotePlaylists { playlists },
                    Err(e) => self.error(&e.to_string()),
                },
            },
            Job::ListPlaylistItems { id, start, limit } => match self.config.as_ref() {
                None => self.error("no server is configured"),
                Some(config) => match playlist_tracks(config, &id, start, limit) {
                    Ok(tracks) => crate::protocol::Event::RemotePlaylistItems { id, tracks },
                    Err(e) => self.error(&e.to_string()),
                },
            },
            Job::Fetch { id } => {
                let Some(config) = self.config.clone() else {
                    return self.error("no server is configured");
                };
                let Some(cache_dir) = self.cache_dir.clone() else {
                    return self.error("no cache directory is configured");
                };
                let Some(track) = self.listing.iter().find(|t| t.id == id).cloned() else {
                    // The id names nothing this session listed. Downloading a
                    // guess would put a file in the cache under a name the
                    // caller never saw.
                    return self.error(&format!("no listed track has the id {id}"));
                };
                match download(&config, &track, &cache_dir) {
                    Ok(path) => crate::protocol::Event::RemoteFetched { track, path },
                    Err(e) => self.error(&e.to_string()),
                }
            }
        }
    }

    fn error(&self, message: &str) -> crate::protocol::Event {
        crate::protocol::Event::Error {
            op: "remote".into(),
            message: message.into(),
        }
    }
}

/// Run jobs until the channel closes.
///
/// One worker, not a pool: a download saturates a link and a listing is one
/// request. Two downloads at once would halve each other's throughput and make
/// the first track start later than if it had the link to itself.
pub(crate) fn run(
    jobs: std::sync::mpsc::Receiver<Job>,
    events: crate::EventBus,
) {
    let mut session = Session::default();
    while let Ok(job) = jobs.recv() {
        events.emit(session.run(job));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn track(id: &str, container: &str) -> RemoteTrack {
        RemoteTrack {
            id: id.into(),
            title: "T".into(),
            artist: "A".into(),
            album: "B".into(),
            container: container.into(),
            duration_ms: 1,
            size: None,
            cover_art: None,
        }
    }

    /// The cache name is the same on every run, or favourites stop resolving.
    #[test]
    fn the_cache_path_is_derived_from_the_item() {
        let dest = Path::new("/cache");
        assert_eq!(
            cache_path(dest, &track("abc123", "flac")),
            Path::new("/cache/abc123.flac")
        );
        assert_eq!(
            cache_path(dest, &track("abc123", "flac")),
            cache_path(dest, &track("abc123", "flac"))
        );
    }

    /// A server is a remote party; its id must not be able to name a path that
    /// climbs out of the cache directory.
    #[test]
    fn a_hostile_id_stays_one_component() {
        let dest = Path::new("/cache");
        let path = cache_path(dest, &track("../../etc/passwd", "flac"));
        assert_eq!(path.parent(), Some(dest), "{}", path.display());
        assert!(!path.to_string_lossy().contains(".."), "{}", path.display());
    }

    /// The token is the one the protocol's own example produces.
    ///
    /// Pinned against the published vector, not against this function's output:
    /// a hash that disagreed with every other Subsonic client would still be a
    /// stable string here.
    #[test]
    fn the_subsonic_token_matches_the_reference_vector() {
        assert_eq!(
            subsonic_token("sesame", "c19b2d"),
            "26719a1196d2a940705a59634eb18eab"
        );
        assert_eq!(subsonic_token("sesame", ""), "c8dae1c50e092f3d877192fc555b1dcf");
    }

    /// The salt has to differ between requests or a captured URL replays.
    #[test]
    fn each_request_gets_its_own_salt() {
        let a = subsonic_salt();
        std::thread::sleep(std::time::Duration::from_millis(2));
        let b = subsonic_salt();
        assert_ne!(a, b);
        assert!(a.len() >= 6, "the protocol wants at least six characters");
    }

    /// A song is read from the shape `search3` documents.
    #[test]
    fn a_navidrome_song_becomes_a_track() {
        let song = serde_json::json!({
            "id": "tr-1",
            "title": "Salt",
            "artist": "Elin Vahl",
            "album": "Northbound",
            "suffix": "FLAC",
            "duration": 236,
            "size": 9000000u64
        });
        let track = navidrome_track(&song).expect("a track");
        assert_eq!(track.id, "tr-1");
        assert_eq!(track.container, "flac", "lower-cased for a file name");
        assert_eq!(track.duration_ms, 236_000, "seconds became milliseconds");
        assert_eq!(track.size, Some(9_000_000));
    }

    /// A query component has to survive a value with spaces and slashes.
    #[test]
    fn values_are_percent_encoded() {
        assert_eq!(urlencode("Hej då/ok"), "Hej%20d%C3%A5%2Fok");
        assert_eq!(urlencode("plain-1.2_3~"), "plain-1.2_3~");
    }

    /// A cover is fetched once and answered from memory afterwards.
    ///
    /// Measured as a count rather than as a call having returned: the second
    /// request must not reach the network at all, which is what the stub's
    /// request counter shows.
    #[test]
    fn the_artwork_cache_answers_the_second_request() {
        let mut cache = ArtworkCache::with_capacity(4);
        let art = RemoteArtwork {
            mime: "image/jpeg".into(),
            data: vec![1, 2, 3],
        };
        assert!(cache.get("cover-1").is_none(), "nothing cached yet");
        cache.put("cover-1", art.clone());
        assert_eq!(cache.get("cover-1"), Some(art), "the same bytes come back");
    }

    /// Bounded, and the bound evicts the least recently used.
    #[test]
    fn the_artwork_cache_is_bounded() {
        let mut cache = ArtworkCache::with_capacity(2);
        for id in ["a", "b", "c"] {
            cache.put(
                id,
                RemoteArtwork {
                    mime: "image/png".into(),
                    data: vec![0u8; 100],
                },
            );
        }
        assert_eq!(cache.len(), 2, "the bound held");
        assert!(cache.get("a").is_none(), "the oldest went");
        assert!(cache.get("c").is_some(), "the newest stayed");
    }

    /// A cover id from one server means nothing on another.
    #[test]
    fn a_new_server_drops_the_cached_covers() {
        let mut cache = ArtworkCache::with_capacity(4);
        cache.put(
            "cover-1",
            RemoteArtwork {
                mime: "image/jpeg".into(),
                data: vec![1],
            },
        );
        cache.clear();
        assert!(cache.get("cover-1").is_none());
    }

    /// A picture survives the process.
    ///
    /// This is the whole point of the disk layer: a restart used to mean every
    /// cover was downloaded again, which is bandwidth the listener pays for and
    /// a visibly slower first browse.
    #[test]
    fn a_cover_survives_a_restart() {
        let dir = std::env::temp_dir().join(format!("neiro-art-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let art = RemoteArtwork {
            mime: "image/png".into(),
            data: vec![0x89, b'P', b'N', b'G', 1, 2, 3],
        };
        {
            let mut cache = ArtworkCache::default().with_dir(dir.clone());
            cache.put("cover-1", art.clone());
        }
        // A fresh cache is what a new process gets.
        let mut restarted = ArtworkCache::default().with_dir(dir.clone());
        assert_eq!(restarted.get("cover-1"), Some(art), "the picture came back");
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The type has to survive with the bytes, or a host draws a data URI with
    /// the wrong mime and the picture does not render.
    #[test]
    fn the_mime_round_trips_through_the_file_name() {
        for mime in ["image/jpeg", "image/png", "image/webp", "image/gif"] {
            let name = artwork_file_name("cover-1", mime);
            assert_eq!(artwork_mime(&name).as_deref(), Some(mime), "{name}");
            assert_eq!(artwork_stem(&name), "cover-1", "{name}");
        }
    }

    /// An id that could climb out of the cache directory does not.
    #[test]
    fn a_hostile_cover_id_stays_one_component() {
        let name = artwork_file_name("../../etc/passwd", "image/png");
        assert!(!name.contains('/'), "{name}");
        assert!(!name.contains(".."), "{name}");
    }

    /// The directory is bounded, and the oldest files go first.
    #[test]
    fn the_disk_cache_is_bounded() {
        let dir = std::env::temp_dir().join(format!("neiro-art-evict-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let mut cache = ArtworkCache::default().with_dir(dir.clone());
        cache.max_files = 4;
        for i in 0..10 {
            cache.put(
                &format!("cover-{i}"),
                RemoteArtwork {
                    mime: "image/png".into(),
                    data: vec![i as u8; 8],
                },
            );
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        let files = std::fs::read_dir(&dir).unwrap().flatten().count();
        assert!(files <= 4, "{files} files left in {}", dir.display());
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The token never reaches an event stream.
    #[test]
    fn the_redacted_config_hides_the_secret() {
        let jellyfin = Server::Jellyfin {
            base_url: "http://host:8096".into(),
            auth: JellyfinAuth::Token {
                token: "secret-token".into(),
            },
        };
        assert_eq!(jellyfin.redacted().base_url(), jellyfin.base_url());
        assert!(!serde_json::to_string(&jellyfin.redacted())
            .unwrap()
            .contains("secret-token"));

        let navidrome = Server::Navidrome {
            base_url: "http://host:4533".into(),
            user: "joe".into(),
            password: "sesame".into(),
        };
        let redacted = serde_json::to_string(&navidrome.redacted()).unwrap();
        assert!(!redacted.contains("sesame"), "{redacted}");
        assert!(redacted.contains("joe"), "the user is not a secret: {redacted}");
    }

    /// The items array is what the API documents, and a missing field is a
    /// shape error rather than an empty library.
    #[test]
    fn a_track_is_read_from_the_documented_shape() {
        let item = serde_json::json!({
            "Id": "abc",
            "Name": "Salt",
            "AlbumArtist": "Elin Vahl",
            "Album": "Northbound",
            "RunTimeTicks": 23_600_000_000u64,
            "MediaSources": [{ "Container": "FLAC", "Size": 9_000_000u64 }]
        });
        let track = track_from_json(&item).expect("a track");
        assert_eq!(track.title, "Salt");
        assert_eq!(track.container, "flac", "lower-cased for a file name");
        assert_eq!(track.duration_ms, 2_360_000);
        assert_eq!(track.size, Some(9_000_000));
    }

    /// An item with no id cannot be addressed, so it is not a track.
    #[test]
    fn an_item_without_an_id_is_skipped() {
        let item = serde_json::json!({ "Name": "Nameless" });
        assert!(track_from_json(&item).is_none());
    }
}
