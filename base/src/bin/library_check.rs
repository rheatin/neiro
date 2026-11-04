//! Checks the library layer: favourites, play counts and playlists.
//!
//! These are the behaviours a UI depends on and that a naive implementation gets
//! wrong. Each one is measured as a consequence rather than as "the call
//! returned Ok":
//!
//!   * a favourite survives a reopen -- otherwise "remembered" means "for this
//!     run", which is not what a listener expects from a star;
//!   * playlist positions stay dense after a removal -- the UI addresses entries
//!     by index, so a hole makes `entry[3]` and `position 3` disagree;
//!   * a track cannot appear twice in one playlist, because a UI cannot show
//!     that and a "remove" would take away the wrong one;
//!   * playlist names are unique case-insensitively, because two sidebar rows
//!     that look identical are indistinguishable to a listener;
//!   * deleting a playlist takes its entries with it.

use neiro_base::library::{Library, LibraryError};

fn check(ok: bool, label: &str, detail: String) {
    println!("  [{}] {:<52} {}", if ok { "PASS" } else { "FAIL" }, label, detail);
    if !ok {
        std::process::exit(1);
    }
}

fn main() {
    let dir = std::env::temp_dir().join("neiro-library-check");
    let _ = std::fs::remove_dir_all(&dir);
    let db_path = dir.join("library.db");

    // ---- persistence -----------------------------------------------------
    //
    // The first handle writes, is dropped, and a second one opens the same file.
    // A favourite that only lived in memory would be gone here, and that is the
    // difference between "remembered" and "remembered until you close the app".
    let a = std::path::Path::new("/music/a.flac");
    let b = std::path::Path::new("/music/b.flac");
    let c = std::path::Path::new("/music/c.flac");
    {
        let lib = Library::open(&db_path).expect("open library");
        check(
            lib.set_favourite(a, true).expect("favourite a"),
            "setting a favourite reports a change",
            String::new(),
        );
        check(
            !lib.set_favourite(a, true).expect("favourite a again"),
            "setting the same favourite twice is not a change",
            String::new(),
        );
        lib.create_playlist("Evening").expect("create playlist");
        lib.add_to_playlist(1, a).expect("add a");
        lib.add_to_playlist(1, b).expect("add b");
        lib.record_play(a, 1_700_000_000_000).expect("record play");
        lib.record_play(a, 1_700_000_000_001).expect("record play");
    }
    {
        let lib = Library::open(&db_path).expect("reopen library");
        check(
            lib.is_favourite(a).expect("read favourite"),
            "a favourite survives a reopen",
            String::new(),
        );
        check(
            !lib.is_favourite(b).expect("read non-favourite"),
            "a track never favourited reads false",
            String::new(),
        );
        let entries = lib.playlist_entries(1).expect("entries");
        check(
            entries.len() == 2,
            "playlist entries survive a reopen",
            format!("{} entries", entries.len()),
        );
        check(
            lib.play_count(a).expect("count") == 2,
            "a play count survives a reopen",
            format!("{}", lib.play_count(a).expect("count")),
        );
    }

    let lib = Library::open(&db_path).expect("open library");

    // ---- favourites ------------------------------------------------------
    let favs = lib.favourites().expect("favourites");
    check(
        favs == vec![a.to_path_buf()],
        "the favourite list holds exactly what was set",
        format!("{favs:?}"),
    );

    // ---- playlists -------------------------------------------------------
    let list = lib.create_playlist("Morning").expect("create second");
    check(list.position > 0, "a second playlist sorts after the first", format!("{}", list.position));

    // Duplicate names are refused, case-insensitively: two rows that look the
    // same in a sidebar are indistinguishable to the listener.
    match lib.create_playlist("evening") {
        Err(LibraryError::DuplicateName(_)) => check(true, "a duplicate name is refused", String::new()),
        other => check(false, "a duplicate name is refused", format!("{other:?}")),
    }
    match lib.create_playlist("   ") {
        Err(LibraryError::EmptyName) => check(true, "a blank name is refused", String::new()),
        other => check(false, "a blank name is refused", format!("{other:?}")),
    }

    // ---- entries stay dense ---------------------------------------------
    //
    // The UI addresses entries by index. A hole left by a removal makes
    // `entry[1]` and `position 1` disagree, and a later "move to position 1"
    // lands nowhere.
    lib.add_to_playlist(list.id, b).expect("add b");
    lib.add_to_playlist(list.id, c).expect("add c");
    let before = lib.playlist_entries(list.id).expect("entries");
    check(
        before.len() == 2,
        "the second playlist has two entries",
        format!("{}", before.len()),
    );
    check(
        before.iter().map(|e| e.position).collect::<Vec<_>>() == vec![0, 1],
        "positions are dense and start at zero",
        format!("{:?}", before.iter().map(|e| e.position).collect::<Vec<_>>()),
    );

    lib.remove_from_playlist(list.id, b).expect("remove b");
    let after = lib.playlist_entries(list.id).expect("entries after removal");
    check(
        after.len() == 1 && after[0].path == c && after[0].position == 0,
        "a removal closes the gap it leaves",
        format!("{:?}", after.iter().map(|e| (&e.path, e.position)).collect::<Vec<_>>()),
    );

    // The same track twice in one playlist is not allowed: a UI cannot show it
    // and "remove" would take away the wrong one.
    check(
        !lib.add_to_playlist(list.id, c).expect("add c again"),
        "adding a track twice is a no-op",
        String::new(),
    );
    check(
        lib.playlist_entries(list.id).expect("entries").len() == 1,
        "the duplicate was not stored",
        String::new(),
    );

    // ---- the reverse query ----------------------------------------------
    //
    // "Which playlists contain this track" is what a UI asks on every selection,
    // and it is the query that justifies the relational schema.
    let containing = lib.playlists_containing(a).expect("containing");
    check(
        containing.len() == 1 && containing[0].name == "Evening",
        "a track reports the playlists it is in",
        format!("{:?}", containing.iter().map(|p| &p.name).collect::<Vec<_>>()),
    );
    check(
        containing[0].track_count == 2,
        "the containing playlist reports its own size",
        format!("{}", containing[0].track_count),
    );

    // ---- deletion cascades ----------------------------------------------
    lib.delete_playlist(list.id).expect("delete playlist");
    check(
        lib.playlists().expect("playlists").len() == 1,
        "the deleted playlist is gone from the list",
        String::new(),
    );
    match lib.playlist_entries(list.id) {
        Err(LibraryError::NoSuchPlaylist(_)) => {
            check(true, "entries of a deleted playlist are not reachable", String::new())
        }
        Ok(entries) => check(
            entries.is_empty(),
            "entries of a deleted playlist are not reachable",
            format!("{} entries", entries.len()),
        ),
        Err(other) => check(false, "entries of a deleted playlist are not reachable", format!("{other:?}")),
    }

    // ---- most played -----------------------------------------------------
    let top = lib.most_played(10).expect("most played");
    check(
        top.len() == 1 && top[0].0 == a.to_path_buf() && top[0].1 == 2,
        "the most-played list holds the track that was played",
        format!("{top:?}"),
    );

    let _ = std::fs::remove_dir_all(&dir);
    println!("\nAll library checks passed.");
}
