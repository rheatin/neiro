//! The library: favourites, playlists and play counts.
//!
//! Base owns these because a host that kept its own copy would eventually
//! disagree with the queue: a UI showing "20 in this playlist" while the engine
//! plays from a three-entry queue is two answers to one question. AGENTS.md
//! rule 1 is about exactly that.
//!
//! What this deliberately does not do: it does not scan directories, does not
//! watch them for changes and does not own a file. A path is a fact the host
//! supplies; this layer remembers what the listener said about it.
//!
//! # Why SQLite
//!
//! Playlists are a many-to-many relation with an order, and "which playlists
//! contain this track" is the query a UI asks on every selection. That is a
//! join, not a scan. A JSON file would have to rewrite the whole library on
//! every favourite toggle, which is fine at a hundred tracks and not at ten
//! thousand.
//!
//! `bundled` compiles SQLite from source into the crate. The iOS build is a
//! static library, and depending on a system SQLite the host has to link is one
//! more thing to get wrong per platform.

use std::path::{Path, PathBuf};

use rusqlite::{params, Connection, OptionalExtension};

/// One playlist as the UI sees it.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Playlist {
    pub id: i64,
    pub name: String,
    /// Position in the sidebar. Assigned on creation and never reused.
    pub position: i64,
    pub track_count: i64,
}

/// A track's place in a playlist.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct PlaylistEntry {
    pub path: PathBuf,
    /// Position within the playlist, 0-based and dense after every edit.
    pub position: i64,
}

#[derive(Debug)]
pub enum LibraryError {
    Sql(rusqlite::Error),
    Io(std::io::Error),
    /// A playlist name that is empty or only whitespace.
    EmptyName,
    /// Two playlists cannot share a name: the sidebar would show two rows that
    /// look identical and behave differently.
    DuplicateName(String),
    NoSuchPlaylist(i64),
}

impl std::fmt::Display for LibraryError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LibraryError::Sql(e) => write!(f, "library: {e}"),
            LibraryError::Io(e) => write!(f, "library: {e}"),
            LibraryError::EmptyName => write!(f, "a playlist needs a name"),
            LibraryError::DuplicateName(name) => {
                write!(f, "a playlist called {name:?} already exists")
            }
            LibraryError::NoSuchPlaylist(id) => write!(f, "no playlist with id {id}"),
        }
    }
}

impl std::error::Error for LibraryError {}

impl From<rusqlite::Error> for LibraryError {
    fn from(value: rusqlite::Error) -> Self {
        // A unique-index violation is the duplicate-name case, and reporting it
        // as a generic SQL error would make the caller string-match a message.
        if let rusqlite::Error::SqliteFailure(err, _) = &value {
            if err.code == rusqlite::ErrorCode::ConstraintViolation {
                return LibraryError::DuplicateName(String::new());
            }
        }
        LibraryError::Sql(value)
    }
}

impl From<std::io::Error> for LibraryError {
    fn from(value: std::io::Error) -> Self {
        LibraryError::Io(value)
    }
}

pub type Result<T, E = LibraryError> = std::result::Result<T, E>;

/// The open library file.
///
/// One connection, used from the control thread. SQLite handles its own locking
/// across processes; within this process the owner is single-threaded by
/// construction, which is the same rule the session handle follows.
pub struct Library {
    db: Connection,
}

const SCHEMA_VERSION: i64 = 1;

impl Library {
    /// Open (or create) the library at `path`.
    ///
    /// Parent directories are created, because the default location is inside
    /// the host's app-data directory and that directory is not guaranteed to
    /// exist on a first run.
    pub fn open(path: &Path) -> Result<Self> {
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                std::fs::create_dir_all(parent)?;
            }
        }
        let db = Connection::open(path)?;
        Self::migrate(db)
    }

    /// An in-memory library. Used by the checks and by a host that has not
    /// chosen a location yet; it behaves identically except that nothing
    /// survives the process.
    pub fn in_memory() -> Result<Self> {
        Self::migrate(Connection::open_in_memory()?)
    }

    fn migrate(db: Connection) -> Result<Self> {
        // Foreign keys are off by default in SQLite, and the playlist-entry
        // table relies on ON DELETE CASCADE to keep a deleted playlist from
        // leaving rows behind. Setting it per connection is the documented way.
        db.execute_batch("PRAGMA foreign_keys = ON;")?;

        let version: i64 = db
            .query_row("PRAGMA user_version", [], |row| row.get(0))
            .unwrap_or(0);
        if version < SCHEMA_VERSION {
            db.execute_batch(
                "CREATE TABLE IF NOT EXISTS tracks (
                     path       TEXT PRIMARY KEY,
                     favourite  INTEGER NOT NULL DEFAULT 0,
                     play_count INTEGER NOT NULL DEFAULT 0,
                     last_played_ms INTEGER
                 );
                 CREATE TABLE IF NOT EXISTS playlists (
                     id       INTEGER PRIMARY KEY AUTOINCREMENT,
                     name     TEXT NOT NULL UNIQUE COLLATE NOCASE,
                     position INTEGER NOT NULL
                 );
                 CREATE TABLE IF NOT EXISTS playlist_entries (
                     playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
                     position    INTEGER NOT NULL,
                     path        TEXT NOT NULL,
                     PRIMARY KEY (playlist_id, position)
                 );
                 CREATE INDEX IF NOT EXISTS playlist_entries_path
                     ON playlist_entries(path);
                 PRAGMA user_version = 1;",
            )?;
        }
        Ok(Self { db })
    }

    // ---- favourites -------------------------------------------------------

    pub fn is_favourite(&self, path: &Path) -> Result<bool> {
        let text = path_text(path)?;
        let value: Option<i64> = self
            .db
            .query_row(
                "SELECT favourite FROM tracks WHERE path = ?1",
                params![text],
                |row| row.get(0),
            )
            .optional()?;
        Ok(value.unwrap_or(0) != 0)
    }

    /// Set or clear the favourite flag. Returns whether it changed, so a UI can
    /// skip an event nobody needs.
    pub fn set_favourite(&self, path: &Path, on: bool) -> Result<bool> {
        let text = path_text(path)?;
        let before = self.is_favourite(path)?;
        if before == on {
            return Ok(false);
        }
        self.db.execute(
            "INSERT INTO tracks (path, favourite) VALUES (?1, ?2)
             ON CONFLICT(path) DO UPDATE SET favourite = excluded.favourite",
            params![text, if on { 1 } else { 0 }],
        )?;
        Ok(true)
    }

    /// Every favourite, sorted by path.
    ///
    /// Sorted because a UI that paints an unordered set gets a different
    /// arrangement on every launch, and a favourite list that shuffles itself is
    /// indistinguishable from a bug.
    pub fn favourites(&self) -> Result<Vec<PathBuf>> {
        let mut stmt = self
            .db
            .prepare("SELECT path FROM tracks WHERE favourite = 1 ORDER BY path")?;
        let rows = stmt.query_map([], |row| row.get::<_, String>(0))?;
        let mut out = Vec::new();
        for row in rows {
            out.push(PathBuf::from(row?));
        }
        Ok(out)
    }

    // ---- play counts ------------------------------------------------------

    /// Record that a track was played. `at_ms` is the host's clock; Base does
    /// not own a clock and does not invent one.
    pub fn record_play(&self, path: &Path, at_ms: i64) -> Result<i64> {
        let text = path_text(path)?;
        self.db.execute(
            "INSERT INTO tracks (path, play_count, last_played_ms) VALUES (?1, 1, ?2)
             ON CONFLICT(path) DO UPDATE SET
                 play_count = play_count + 1,
                 last_played_ms = excluded.last_played_ms",
            params![text, at_ms],
        )?;
        let count: i64 = self.db.query_row(
            "SELECT play_count FROM tracks WHERE path = ?1",
            params![text],
            |row| row.get(0),
        )?;
        Ok(count)
    }

    pub fn play_count(&self, path: &Path) -> Result<i64> {
        let text = path_text(path)?;
        let count: Option<i64> = self
            .db
            .query_row(
                "SELECT play_count FROM tracks WHERE path = ?1",
                params![text],
                |row| row.get(0),
            )
            .optional()?;
        Ok(count.unwrap_or(0))
    }

    /// Most-played first. Tracks with equal counts sort by path so the order is
    /// stable.
    pub fn most_played(&self, limit: usize) -> Result<Vec<(PathBuf, i64)>> {
        let mut stmt = self.db.prepare(
            "SELECT path, play_count FROM tracks
             WHERE play_count > 0
             ORDER BY play_count DESC, path
             LIMIT ?1",
        )?;
        let rows = stmt.query_map(params![limit as i64], |row| {
            Ok((PathBuf::from(row.get::<_, String>(0)?), row.get::<_, i64>(1)?))
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    // ---- playlists --------------------------------------------------------

    pub fn create_playlist(&self, name: &str) -> Result<Playlist> {
        let trimmed = name.trim();
        if trimmed.is_empty() {
            return Err(LibraryError::EmptyName);
        }
        let position: i64 = self
            .db
            .query_row(
                "SELECT COALESCE(MAX(position), -1) + 1 FROM playlists",
                [],
                |row| row.get(0),
            )
            .unwrap_or(0);
        // The UNIQUE index is the authority on duplicate names, and it compares
        // with COLLATE NOCASE. Checking here as well would be a second opinion
        // that can disagree with the index -- and the index is what actually
        // rejects the insert.
        self.db
            .execute(
                "INSERT INTO playlists (name, position) VALUES (?1, ?2)",
                params![trimmed, position],
            )
            .map_err(|e| match e {
                rusqlite::Error::SqliteFailure(err, _)
                    if err.code == rusqlite::ErrorCode::ConstraintViolation =>
                {
                    LibraryError::DuplicateName(trimmed.to_string())
                }
                other => LibraryError::Sql(other),
            })?;
        let id = self.db.last_insert_rowid();
        Ok(Playlist {
            id,
            name: trimmed.to_string(),
            position,
            track_count: 0,
        })
    }

    pub fn rename_playlist(&self, id: i64, name: &str) -> Result<()> {
        let trimmed = name.trim();
        if trimmed.is_empty() {
            return Err(LibraryError::EmptyName);
        }
        let changed = self
            .db
            .execute(
                "UPDATE playlists SET name = ?1 WHERE id = ?2",
                params![trimmed, id],
            )
            .map_err(|e| match e {
                rusqlite::Error::SqliteFailure(err, _)
                    if err.code == rusqlite::ErrorCode::ConstraintViolation =>
                {
                    LibraryError::DuplicateName(trimmed.to_string())
                }
                other => LibraryError::Sql(other),
            })?;
        if changed == 0 {
            return Err(LibraryError::NoSuchPlaylist(id));
        }
        Ok(())
    }

    /// Delete a playlist and its entries. The cascade does the second half.
    pub fn delete_playlist(&self, id: i64) -> Result<()> {
        let changed = self
            .db
            .execute("DELETE FROM playlists WHERE id = ?1", params![id])?;
        if changed == 0 {
            return Err(LibraryError::NoSuchPlaylist(id));
        }
        Ok(())
    }

    pub fn playlists(&self) -> Result<Vec<Playlist>> {
        let mut stmt = self.db.prepare(
            "SELECT p.id, p.name, p.position, COUNT(e.path)
             FROM playlists p
             LEFT JOIN playlist_entries e ON e.playlist_id = p.id
             GROUP BY p.id
             ORDER BY p.position",
        )?;
        let rows = stmt.query_map([], |row| {
            Ok(Playlist {
                id: row.get(0)?,
                name: row.get(1)?,
                position: row.get(2)?,
                track_count: row.get(3)?,
            })
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    /// Append a path to a playlist. Adding one that is already there is a no-op
    /// rather than a duplicate: the same track twice in one playlist is a
    /// mistake a UI cannot show, so it is not allowed to exist.
    pub fn add_to_playlist(&self, playlist: i64, path: &Path) -> Result<bool> {
        self.require_playlist(playlist)?;
        let text = path_text(path)?;
        let already: Option<i64> = self
            .db
            .query_row(
                "SELECT 1 FROM playlist_entries WHERE playlist_id = ?1 AND path = ?2",
                params![playlist, text],
                |row| row.get(0),
            )
            .optional()?;
        if already.is_some() {
            return Ok(false);
        }
        let position: i64 = self
            .db
            .query_row(
                "SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_entries
                 WHERE playlist_id = ?1",
                params![playlist],
                |row| row.get(0),
            )
            .unwrap_or(0);
        self.db.execute(
            "INSERT INTO playlist_entries (playlist_id, position, path) VALUES (?1, ?2, ?3)",
            params![playlist, position, text],
        )?;
        Ok(true)
    }

    /// Remove a path from a playlist and close the gap it leaves.
    ///
    /// The positions have to stay dense: the UI addresses entries by index, and
    /// a hole would make `entry[3]` and `position 3` disagree.
    pub fn remove_from_playlist(&self, playlist: i64, path: &Path) -> Result<bool> {
        self.require_playlist(playlist)?;
        let text = path_text(path)?;
        let position: Option<i64> = self
            .db
            .query_row(
                "SELECT position FROM playlist_entries
                 WHERE playlist_id = ?1 AND path = ?2",
                params![playlist, text],
                |row| row.get(0),
            )
            .optional()?;
        let Some(position) = position else {
            return Ok(false);
        };
        self.db.execute(
            "DELETE FROM playlist_entries WHERE playlist_id = ?1 AND position = ?2",
            params![playlist, position],
        )?;
        self.db.execute(
            "UPDATE playlist_entries SET position = position - 1
             WHERE playlist_id = ?1 AND position > ?2",
            params![playlist, position],
        )?;
        Ok(true)
    }

    pub fn playlist_entries(&self, playlist: i64) -> Result<Vec<PlaylistEntry>> {
        self.require_playlist(playlist)?;
        let mut stmt = self.db.prepare(
            "SELECT path, position FROM playlist_entries
             WHERE playlist_id = ?1 ORDER BY position",
        )?;
        let rows = stmt.query_map(params![playlist], |row| {
            Ok(PlaylistEntry {
                path: PathBuf::from(row.get::<_, String>(0)?),
                position: row.get(1)?,
            })
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    /// The playlists a track belongs to. This is the query a UI asks on every
    /// selection, and the reason the schema is relational.
    pub fn playlists_containing(&self, path: &Path) -> Result<Vec<Playlist>> {
        let text = path_text(path)?;
        let mut stmt = self.db.prepare(
            "SELECT p.id, p.name, p.position, COUNT(e2.path)
             FROM playlists p
             JOIN playlist_entries e ON e.playlist_id = p.id AND e.path = ?1
             LEFT JOIN playlist_entries e2 ON e2.playlist_id = p.id
             GROUP BY p.id
             ORDER BY p.position",
        )?;
        let rows = stmt.query_map(params![text], |row| {
            Ok(Playlist {
                id: row.get(0)?,
                name: row.get(1)?,
                position: row.get(2)?,
                track_count: row.get(3)?,
            })
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    fn require_playlist(&self, id: i64) -> Result<()> {
        let found: Option<i64> = self
            .db
            .query_row("SELECT id FROM playlists WHERE id = ?1", params![id], |row| {
                row.get(0)
            })
            .optional()?;
        match found {
            Some(_) => Ok(()),
            None => Err(LibraryError::NoSuchPlaylist(id)),
        }
    }
}

/// A path as the library stores it.
///
/// UTF-8, because that is what the engine takes and what the C ABI carries. A
/// path that is not representable is refused rather than mangled: the library
/// would otherwise store a name that does not open.
fn path_text(path: &Path) -> Result<String> {
    path.to_str().map(str::to_owned).ok_or_else(|| {
        LibraryError::Io(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "path is not valid UTF-8",
        ))
    })
}
