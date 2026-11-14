//! The library: favourites, playlists and play counts.
//!
//! Base owns these because a host that kept its own copy would eventually
//! disagree with the queue: a UI showing "20 in this playlist" while the engine
//! plays from a three-entry queue is two answers to one question. AGENTS.md
//! rule 1 is about exactly that.
//!
//! What this deliberately does not do: it does not own a file and it does not
//! walk directories. [`crate::scan`] walks and reports; this layer remembers.
//!
//! It does hold the **index**: `files(path, root, size, mtime_ms)`, what a walk
//! last saw. That belongs here for two reasons. It is the only way to answer
//! "what left?" when a directory is removed, and it is what turns a scan into a
//! difference instead of a re-import. It is derived data -- every row can be
//! rebuilt by walking -- which is why it lives beside the listener's own data
//! but is never the authority on it: a file that disappears loses its index row
//! and keeps its star.
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
    /// A path is already known, so a move onto it would merge two files.
    PathOccupied(PathBuf),
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
            LibraryError::PathOccupied(path) => {
                write!(f, "{} is already known to the library", path.display())
            }
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

const SCHEMA_VERSION: i64 = 5;

/// What the browsing index remembers about one file.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct StoredTags {
    pub path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub disc_no: i64,
    pub track_no: i64,
    pub year: i64,
    pub genre: String,
    pub duration_ms: i64,
    pub sample_rate: i64,
    pub bits: i64,
    pub channels: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct AlbumRow {
    pub album: String,
    pub album_artist: String,
    pub year: i64,
    pub track_count: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ArtistRow {
    pub artist: String,
    pub track_count: i64,
    pub album_count: i64,
}

/// One row, written through whatever connection the caller holds -- a plain
/// handle or a transaction. Shared so a batch and a single write cannot drift.
fn write_tags_row(db: &rusqlite::Connection, tags: &StoredTags) -> Result<()> {
    // Folded here, in Rust, because SQLite's LIKE folds ASCII only. Every field
    // a listener can type is in the haystack.
    let search_text = format!(
        "{} {} {} {}",
        tags.title, tags.artist, tags.album, tags.album_artist
    )
    .to_lowercase();
    db.execute(
        "INSERT INTO tags (path, title, artist, album, album_artist, disc_no,
                           track_no, year, genre, duration_ms, sample_rate,
                           bits, channels, search_text)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)
         ON CONFLICT(path) DO UPDATE SET
             title = excluded.title,
             artist = excluded.artist,
             album = excluded.album,
             album_artist = excluded.album_artist,
             disc_no = excluded.disc_no,
             track_no = excluded.track_no,
             year = excluded.year,
             genre = excluded.genre,
             duration_ms = excluded.duration_ms,
             sample_rate = excluded.sample_rate,
             bits = excluded.bits,
             channels = excluded.channels,
             search_text = excluded.search_text",
        params![
            path_text(&tags.path)?,
            tags.title,
            tags.artist,
            tags.album,
            tags.album_artist,
            tags.disc_no,
            tags.track_no,
            tags.year,
            tags.genre,
            tags.duration_ms,
            tags.sample_rate,
            tags.bits,
            tags.channels,
            search_text,
        ],
    )?;
    Ok(())
}

fn row_to_tags(row: &rusqlite::Row<'_>) -> rusqlite::Result<StoredTags> {
    Ok(StoredTags {
        path: PathBuf::from(row.get::<_, String>(0)?),
        title: row.get(1)?,
        artist: row.get(2)?,
        album: row.get(3)?,
        album_artist: row.get(4)?,
        disc_no: row.get(5)?,
        track_no: row.get(6)?,
        year: row.get(7)?,
        genre: row.get(8)?,
        duration_ms: row.get(9)?,
        sample_rate: row.get(10)?,
        bits: row.get(11)?,
        channels: row.get(12)?,
    })
}

impl Library {
    /// Open (or create) the library at `path`.
    ///
    /// Parent directories are created, because the default location is inside
    /// the host's app-data directory and that directory is not guaranteed to
    /// exist on a first run.
    ///
    /// An empty path is SQLite's private temporary database: it opens, it
    /// behaves like any other library, and it is gone when the process exits.
    /// That is a real behaviour and not a bug, but it is not what a host means
    /// by "the path my settings hold", so the C ABI refuses the empty string
    /// and points at `NAE_BaseStart` for "no library". Use `in_memory` to ask
    /// for the throwaway case deliberately.
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
        if version < 2 {
            // What a folder scan last saw. Size and modification time are what
            // a walk can read without decoding, and they are enough to tell an
            // unchanged file from a changed one and a moved file from a deleted
            // one. `root` is stored so a deletion can be answered with the
            // files that were actually under it instead of "look again".
            db.execute_batch(
                "CREATE TABLE IF NOT EXISTS files (
                     path     TEXT PRIMARY KEY,
                     root     TEXT NOT NULL,
                     size     INTEGER NOT NULL,
                     mtime_ms INTEGER NOT NULL
                 );
                 CREATE INDEX IF NOT EXISTS files_root ON files(root);
                 PRAGMA user_version = 2;",
            )?;
        }
        if version < 3 {
            // Windows and SMB compare paths without regard to case, so two
            // spellings of one folder are one folder. Without this the index
            // stores them as two: `\\server\Music` and `\\SERVER\Music` are
            // different TEXT values, and a host that changed the spelling --
            // or a mapped drive that resolved differently -- re-imported the
            // whole library and then held two rows for every file.
            //
            // Measured on a real share: the second spelling reported all 3,585
            // files as added. The table is derived, so rebuilding it is free.
            db.execute_batch(
                "DROP TABLE IF EXISTS files;
                 CREATE TABLE files (
                     path     TEXT PRIMARY KEY COLLATE NOCASE,
                     root     TEXT NOT NULL COLLATE NOCASE,
                     size     INTEGER NOT NULL,
                     mtime_ms INTEGER NOT NULL
                 );
                 CREATE INDEX IF NOT EXISTS files_root ON files(root);
                 PRAGMA user_version = 3;",
            )?;
        }
        if version < 4 {
            // What a UI browses by.
            //
            // Until now the library remembered only what the listener did to a
            // path: favourite, play count, playlists. Browsing had no answer --
            // an album view would have had to open every file on every launch,
            // which is a decode per row. These are the fields (and only the
            // fields) that a list, an album grid and a search need; they are a
            // cache of the file's own tags, rebuilt from the file and never
            // written back to it.
            //
            // `album` is indexed rather than sorted on read: grouping thousands
            // of rows in Rust would read all of them, which is the same mistake
            // as reading tags per path one level up.
            db.execute_batch(
                "CREATE TABLE IF NOT EXISTS tags (
                     path         TEXT PRIMARY KEY,
                     title        TEXT NOT NULL DEFAULT '',
                     artist       TEXT NOT NULL DEFAULT '',
                     album        TEXT NOT NULL DEFAULT '',
                     album_artist TEXT NOT NULL DEFAULT '',
                     disc_no      INTEGER NOT NULL DEFAULT 0,
                     track_no     INTEGER NOT NULL DEFAULT 0,
                     year         INTEGER NOT NULL DEFAULT 0,
                     genre        TEXT NOT NULL DEFAULT '',
                     duration_ms  INTEGER NOT NULL DEFAULT 0,
                     sample_rate  INTEGER NOT NULL DEFAULT 0,
                     bits         INTEGER NOT NULL DEFAULT 0,
                     channels     INTEGER NOT NULL DEFAULT 0
                 );
                 CREATE INDEX IF NOT EXISTS tags_album  ON tags(album COLLATE NOCASE);
                 CREATE INDEX IF NOT EXISTS tags_artist ON tags(artist COLLATE NOCASE);
                 CREATE INDEX IF NOT EXISTS tags_album_artist
                     ON tags(album_artist COLLATE NOCASE);
                 PRAGMA user_version = 4;",
            )?;
        }
        if version < 5 {
            // `LIKE` folds case for ASCII only, so a listener searching "bela"
            // never finds "Béla". The fold is done in Rust, which knows every
            // script, and the result is what a query is matched against.
            db.execute_batch(
                "ALTER TABLE tags ADD COLUMN search_text TEXT NOT NULL DEFAULT '';
                 PRAGMA user_version = 5;",
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


/// One file as a scan last saw it.
///
/// Size and modification time are what a walk can read without opening the
/// file, and they are enough to tell unchanged from changed and moved from
/// deleted. They are not an identity: two files with the same size and the same
/// timestamp are possible, which is why a pairing that finds more than one
/// candidate gives up instead of guessing.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IndexedFile {
    pub path: PathBuf,
    pub size: i64,
    pub mtime_ms: i64,
}

impl Library {
    /// Every file the index holds under `root`.
    pub fn files_under(&self, root: &Path) -> Result<Vec<IndexedFile>> {
        let root = path_text(root)?;
        let mut statement = self.db.prepare(
            "SELECT path, size, mtime_ms FROM files WHERE root = ?1 ORDER BY path",
        )?;
        let rows = statement.query_map(params![root], |row| {
            Ok(IndexedFile {
                path: PathBuf::from(row.get::<_, String>(0)?),
                size: row.get(1)?,
                mtime_ms: row.get(2)?,
            })
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    /// Record what a scan found, in one transaction.
    ///
    /// The order matters: a moved file has to be moved while both its old and
    /// its new path are still known, so the rewrite happens before the
    /// insert and after the delete. A failure anywhere leaves the index as it
    /// was, which is the same state a crash before the commit would leave.
    // ---- the browsing index ----------------------------------------------
    //
    // These are a cache of what the file says about itself. They are never
    // written back to the file, and a read that fails leaves the row alone
    // rather than storing a guess.

    /// Store or replace what the file's tags say.
    pub fn put_tags(&self, tags: &StoredTags) -> Result<()> {
        write_tags_row(&self.db, tags)
    }

    /// Store many rows in one transaction.
    ///
    /// One `INSERT` per row means one implicit transaction per row, and on a
    /// first scan of ten thousand files that is ten thousand fsyncs on the
    /// control thread while commands queue behind it. The index worker returns
    /// in bursts, so the burst is the natural unit.
    pub fn put_tags_many(&self, rows: &[StoredTags]) -> Result<()> {
        if rows.is_empty() {
            return Ok(());
        }
        let tx = self.db.unchecked_transaction()?;
        for tags in rows {
            write_tags_row(&tx, tags)?;
        }
        tx.commit()?;
        Ok(())
    }

    /// Whether this path has been indexed yet.
    pub fn has_tags(&self, path: &Path) -> Result<bool> {
        let count: i64 = self.db.query_row(
            "SELECT COUNT(*) FROM tags WHERE path = ?1",
            params![path_text(path)?],
            |row| row.get(0),
        )?;
        Ok(count > 0)
    }

    /// Drop the row for a file that is gone.
    pub fn forget_tags(&self, path: &Path) -> Result<()> {
        self.db
            .execute("DELETE FROM tags WHERE path = ?1", params![path_text(path)?])?;
        Ok(())
    }

    pub fn track_count(&self) -> Result<i64> {
        Ok(self
            .db
            .query_row("SELECT COUNT(*) FROM tags", [], |row| row.get(0))?)
    }

    pub fn list_tracks(&self, offset: i64, limit: i64) -> Result<Vec<StoredTags>> {
        let mut statement = self.db.prepare(
            "SELECT path, title, artist, album, album_artist, disc_no, track_no,
                    year, genre, duration_ms, sample_rate, bits, channels
             FROM tags
             ORDER BY album_artist COLLATE NOCASE, album COLLATE NOCASE,
                      disc_no, track_no, path
             LIMIT ?1 OFFSET ?2",
        )?;
        let rows = statement.query_map(params![limit, offset], row_to_tags)?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    /// One row per album, with the artist credited for the album.
    pub fn list_albums(&self) -> Result<Vec<AlbumRow>> {
        let mut statement = self.db.prepare(
            "SELECT MAX(album),
                    COALESCE(NULLIF(MAX(album_artist), ''), MAX(artist)) AS credited,
                    MAX(year),
                    COUNT(*)
             FROM tags
             WHERE album <> ''
             GROUP BY album COLLATE NOCASE
             ORDER BY credited COLLATE NOCASE, MAX(album) COLLATE NOCASE",
        )?;
        let rows = statement.query_map([], |row| {
            Ok(AlbumRow {
                album: row.get(0)?,
                album_artist: row.get(1)?,
                year: row.get(2)?,
                track_count: row.get(3)?,
            })
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    pub fn list_artists(&self) -> Result<Vec<ArtistRow>> {
        let mut statement = self.db.prepare(
            "SELECT MAX(artist), COUNT(*), COUNT(DISTINCT album COLLATE NOCASE)
             FROM tags
             WHERE artist <> ''
             GROUP BY artist COLLATE NOCASE
             ORDER BY artist COLLATE NOCASE",
        )?;
        let rows = statement.query_map([], |row| {
            Ok(ArtistRow {
                artist: row.get(0)?,
                track_count: row.get(1)?,
                album_count: row.get(2)?,
            })
        })?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    /// Substring search over the fields a listener types into a box.
    ///
    /// `LIKE` rather than FTS5: the bundled SQLite may or may not carry the FTS5
    /// module, and a search that silently returns nothing when it does not is
    /// worse than a scan. The table is the size of the library, and this is one
    /// scan of it per keystroke at worst -- a host that types ahead should debounce.
    pub fn search(&self, query: &str, limit: i64) -> Result<Vec<StoredTags>> {
        // `%` and `_` are wildcards to LIKE, so they are escaped rather than
        // dropped: a listener searching "50% off" means those characters.
        let escaped = query.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
        let needle = format!("%{}%", escaped.to_lowercase());
        let mut statement = self.db.prepare(
            "SELECT path, title, artist, album, album_artist, disc_no, track_no,
                    year, genre, duration_ms, sample_rate, bits, channels
             FROM tags
             WHERE search_text LIKE ?1 ESCAPE '\\' 
             ORDER BY album_artist COLLATE NOCASE, album COLLATE NOCASE,
                      disc_no, track_no, path
             LIMIT ?2",
        )?;
        let rows = statement.query_map(params![needle, limit], row_to_tags)?;
        let mut out = Vec::new();
        for row in rows {
            out.push(row?);
        }
        Ok(out)
    }

    /// Delete every file indexed under `root` (and the tags read from them).
    /// A root the listener removed is not a library any more, so leaving its
    /// rows behind would keep its tracks in every list and in every count.
    pub fn forget_root(&mut self, root: &Path) -> Result<usize> {
        let root_text = path_text(root)?;
        let tx = self.db.transaction()?;
        tx.execute("DELETE FROM tags WHERE path IN (SELECT path FROM files WHERE root = ?1)", params![root_text.clone()])?;
        let removed = tx.execute("DELETE FROM files WHERE root = ?1", params![root_text])?;
        tx.commit()?;
        Ok(removed)
    }

    pub fn apply_scan(
        &mut self,
        root: &Path,
        present: &[IndexedFile],
        removed: &[PathBuf],
        moved: &[(PathBuf, PathBuf)],
    ) -> Result<Vec<PathBuf>> {
        let root_text = path_text(root)?;
        let tx = self.db.transaction()?;
        for path in removed {
            tx.execute("DELETE FROM files WHERE path = ?1", params![path_text(path)?])?;
            // The browse row is derived from the file, and the file is gone. A
            // row left behind keeps the track in every list and in its album's
            // count for as long as the library exists.
            tx.execute("DELETE FROM tags WHERE path = ?1", params![path_text(path)?])?;
        }
        let mut skipped: Vec<PathBuf> = Vec::new();
        for (from, to) in moved {
            match rewrite_path(&tx, from, to) {
                Ok(()) => {}
                // The destination is already known, so this is not a move the
                // library can honour: merging would give one file the other's
                // history. Forget the old index row and leave the star where
                // the listener put it, rather than aborting the whole scan.
                Err(LibraryError::PathOccupied(_)) => {
                    tx.execute(
                        "DELETE FROM files WHERE path = ?1",
                        params![path_text(from)?],
                    )?;
                    skipped.push(to.clone());
                }
                Err(e) => return Err(e),
            }
        }
        for file in present {
            tx.execute(
                "INSERT INTO files (path, root, size, mtime_ms) VALUES (?1, ?2, ?3, ?4)
                 ON CONFLICT(path) DO UPDATE SET
                     root = excluded.root,
                     size = excluded.size,
                     mtime_ms = excluded.mtime_ms",
                params![
                    path_text(&file.path)?,
                    root_text,
                    file.size,
                    file.mtime_ms
                ],
            )?;
        }
        tx.commit()?;
        Ok(skipped)
    }
}

/// Move one path through every table that stores it.
///
/// The one place that knows what "moving a file" means in this library: every
/// table that stores a path is listed here, and `apply_scan` calls it inside
/// its transaction.
fn rewrite_path(db: &Connection, from: &Path, to: &Path) -> Result<()> {
    let from_text = path_text(from)?;
    let to_text = path_text(to)?;
    if from_text == to_text {
        return Ok(());
    }
    let occupied: Option<i64> = db
        .query_row("SELECT 1 FROM tracks WHERE path = ?1", params![to_text], |row| {
            row.get(0)
        })
        .optional()?;
    if occupied.is_some() {
        return Err(LibraryError::PathOccupied(to.to_path_buf()));
    }
    db.execute(
        "UPDATE tracks SET path = ?2 WHERE path = ?1",
        params![from_text, to_text],
    )?;
    db.execute(
        "UPDATE playlist_entries SET path = ?2 WHERE path = ?1",
        params![from_text, to_text],
    )?;
    // The browsing row moves too. Without this a rename left the old path in
    // every list -- a row pointing at a file that no longer exists -- and the
    // new path absent, because a move is neither an addition nor a modification.
    db.execute(
        "UPDATE tags SET path = ?2 WHERE path = ?1",
        params![from_text, to_text],
    )?;
    db.execute(
        "UPDATE files SET path = ?2 WHERE path = ?1",
        params![from_text, to_text],
    )?;
    Ok(())
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

#[cfg(test)]
mod tests {
    use super::*;

    fn lib() -> Library {
        Library::in_memory().expect("an in-memory library")
    }

    fn file(path: &str, size: i64, mtime_ms: i64) -> IndexedFile {
        IndexedFile {
            path: PathBuf::from(path),
            size,
            mtime_ms,
        }
    }

    /// A move keeps everything the listener said about the file. This is the
    /// whole reason a move is detected instead of being reported as a delete
    /// plus an add.
    #[test]
    fn a_move_carries_the_star_and_the_play_count() {
        let mut lib = lib();
        let from = Path::new("/music/a/track.flac");
        let to = Path::new("/music/b/track.flac");
        lib.set_favourite(from, true).expect("favourite");
        lib.record_play(from, 1).expect("play");
        lib.create_playlist("Evening").expect("playlist");
        lib.add_to_playlist(1, from).expect("add to playlist");
        lib.apply_scan(
            Path::new("/music"),
            &[file("/music/a/track.flac", 10, 100)],
            &[],
            &[],
        )
        .expect("index");

        // The production path: a scan that saw the file move.
        lib.apply_scan(
            Path::new("/music"),
            &[file("/music/b/track.flac", 10, 100)],
            &[],
            &[(from.to_path_buf(), to.to_path_buf())],
        )
        .expect("a scan that saw the move");

        assert!(lib.is_favourite(to).expect("favourite at the new path"));
        assert_eq!(lib.play_count(to).expect("count"), 1);
        assert_eq!(lib.playlist_entries(1).expect("entries")[0].path, to);
        assert!(!lib.is_favourite(from).expect("old path"));
        let indexed = lib.files_under(Path::new("/music")).expect("index");
        assert_eq!(indexed.len(), 1);
        assert_eq!(indexed[0].path, to, "the index moved with it");
    }

    /// Moving onto a path the library already knows would merge two files and
    /// give one of them the other's history, so the scan refuses that pairing
    /// and says which one it dropped.
    #[test]
    fn a_move_onto_a_known_path_is_skipped_and_reported() {
        let mut lib = lib();
        let from = Path::new("/music/a.flac");
        let to = Path::new("/music/b.flac");
        lib.set_favourite(to, true).expect("star the destination");
        lib.set_favourite(from, true).expect("star the source");
        let skipped = lib
            .apply_scan(
                Path::new("/music"),
                &[file("/music/b.flac", 10, 100)],
                &[],
                &[(from.to_path_buf(), to.to_path_buf())],
            )
            .expect("scan");
        assert_eq!(skipped, vec![to.to_path_buf()], "the pairing is reported");
        // The destination keeps its own star; the source keeps its own. Neither
        // file inherited the other's history.
        assert!(lib.is_favourite(to).expect("destination"));
        assert!(lib.is_favourite(from).expect("source"));
    }

    /// A scan that removes a file drops its index entry and leaves the star
    /// alone: the file is gone, not the listener's opinion of it.
    #[test]
    fn a_removed_file_leaves_the_index_and_keeps_the_star() {
        let mut lib = lib();
        let path = Path::new("/music/a.flac");
        lib.set_favourite(path, true).expect("star");
        lib.apply_scan(Path::new("/music"), &[file("/music/a.flac", 10, 100)], &[], &[])
            .expect("index");
        lib.apply_scan(
            Path::new("/music"),
            &[],
            &[PathBuf::from("/music/a.flac")],
            &[],
        )
        .expect("rescan");
        assert!(lib.files_under(Path::new("/music")).expect("index").is_empty());
        assert!(lib.is_favourite(path).expect("star"), "the star survived");
    }

    /// Two roots keep their own entries: a scan of one must not disturb the
    /// index of the other.
    #[test]
    fn roots_are_indexed_apart() {
        let mut lib = lib();
        lib.apply_scan(Path::new("/a"), &[file("/a/x.flac", 1, 1)], &[], &[])
            .expect("index a");
        lib.apply_scan(Path::new("/b"), &[file("/b/y.flac", 2, 2)], &[], &[])
            .expect("index b");
        assert_eq!(lib.files_under(Path::new("/a")).expect("a").len(), 1);
        assert_eq!(lib.files_under(Path::new("/b")).expect("b").len(), 1);
    }
}
