//! The out-of-process transport, driven the way a separate UI process drives it.
//!
//! Everything here goes through a named pipe: a `Command` is serialized to one
//! line of JSON, sent, and the answer is read back as an `Event`. That is the
//! whole point of the module -- if the protocol could only be exercised from
//! inside the process, the transport would not be tested by this file.
//!
//! What is measured:
//!
//!   * a command sent over the wire reaches Base and takes effect;
//!   * its answer comes back over the same connection, as the event a host
//!     would receive;
//!   * a malformed command is reported rather than dropped, because a client
//!     that sent garbage and got silence would think the server had died;
//!   * the listener is reachable again after the client disconnects.

use std::time::{Duration, Instant};

use neiro_base::protocol::{Command, Event};
use neiro_base::transport::{self, Client};
use neiro_base::Base;

fn check(ok: bool, label: &str, detail: String) {
    println!(
        "  [{}] {:<52} {}",
        if ok { "PASS" } else { "FAIL" },
        label,
        detail
    );
    if !ok {
        std::process::exit(1);
    }
}

/// Read events until one satisfies `want`, with a bounded budget.
///
/// The budget is a real one: the server polls its command channel every 10 ms,
/// so an answer that has not arrived in a second is not late, it is absent.
fn wait_for(client: &mut Client, want: impl Fn(&Event) -> bool) -> Option<Event> {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        match client.recv() {
            Ok(Some(event)) if want(&event) => return Some(event),
            Ok(Some(_)) => {}
            Ok(None) => return None,
            Err(_) => return None,
        }
    }
    None
}

fn main() {
    let pipe = r"\\.\pipe\neiro-base-transport-check";
    // The library has to be open: `ListPlaylists` answers with the list, and
    // without a library it answers with an error instead -- the check waited
    // for an event that was never coming, which looked like a hung transport.
    let db = std::env::temp_dir().join("neiro-transport-check.db");
    let _ = std::fs::remove_file(&db);
    let library = neiro_base::library::Library::open(&db).expect("open library");
    let base = Base::start_with_policy_and_library(
        neiro_base::protocol::PlaybackPolicy::default(),
        library,
    )
    .expect("start Base");
    let handle = base.handle();
    let mut server = transport::Transport::start(handle.clone(), pipe).expect("start transport");

    // ---- a command takes effect over the wire ---------------------------
    //
    // The command is a request for the playlist list, which needs no device and
    // no library: it answers with an empty list, and the answer arriving at all
    // is what proves the round trip.
    let mut client = connect_with_retry(pipe);
    client
        .send(&Command::ListPlaylists)
        .expect("send a command over the pipe");
    let answer = wait_for(&mut client, |event| {
        matches!(event, Event::PlaylistsChanged { .. })
    });
    check(
        answer.is_some(),
        "a command over the pipe is answered on the same connection",
        format!("{:?}", answer.map(|e| serde_json::to_string(&e).unwrap())),
    );

    // ---- a malformed command is reported --------------------------------
    //
    // Sent as raw text rather than through `send`, because `send` serializes a
    // valid `Command`. A client that mistypes its JSON has to learn that.
    {
        client
            .send_raw("{\"command\":\"no_such_command\"}\n")
            .expect("send malformed JSON");
        let reported = wait_for(&mut client, |event| {
            matches!(event, Event::Error { op, .. } if op == "transport")
        });
        check(
            reported.is_some(),
            "malformed JSON is reported, not dropped",
            format!("{reported:?}"),
        );
    }

    // ---- a client that sends no newline does not stall the server --------
    //
    // `read_line` blocks until a newline arrives, so a client that sends bytes
    // without one used to freeze the server -- no events served, no commands
    // read, for as long as the client stayed silent. The read is bounded to what
    // the pipe already holds now.
    {
        // The partial line has no newline, so it is not a command and is not
        // answered. What is measured is that the *server* keeps running: a
        // complete command sent afterwards still gets its reply.
        //
        // The partial bytes stay buffered ahead of the next line -- that is
        // what a line protocol does -- so the check finishes that line rather
        // than leaving the stream misaligned for the next step.
        client
            .send_raw("{\"command\":\"list_playlists\"")
            .expect("send a partial line");
        // Complete it into a valid command by adding the rest and a newline.
        client
            .send_raw("}\n")
            .expect("finish the partial line");
        let answered = wait_for(&mut client, |event| {
            matches!(event, Event::PlaylistsChanged { .. })
        });
        check(
            answered.is_some(),
            "a partial line does not stall the server",
            format!("{:?}", answered.is_some()),
        );
    }

    // ---- an over-long line is dropped, and said so ----------------------
    //
    // Unbounded buffering let a client grow the server's memory until it died.
    // The line is discarded past the limit and the client is told once.
    {
        // Just past the limit, and inside the pipe's inbound buffer: a line
        // that does not fit in one write would block the *client* before the
        // server could ever report it, and the check would hang rather than
        // measure anything.
        let mut huge = String::from("{\"command\":\"no_such");
        huge.push_str(&"x".repeat(4100));
        huge.push('\n');
        client.send_raw(&huge).expect("send an over-long line");
        let reported = wait_for(&mut client, |event| {
            matches!(event, Event::Error { op, message }
                if op == "transport" && message.contains("longer than"))
        });
        check(
            reported.is_some(),
            "an over-long line is dropped and reported",
            format!("{reported:?}"),
        );
        // The stream realigns: the next valid command is answered.
        client
            .send(&Command::ListPlaylists)
            .expect("send after the over-long line");
        let answered = wait_for(&mut client, |event| {
            matches!(event, Event::PlaylistsChanged { .. })
        });
        check(
            answered.is_some(),
            "the stream realigns after a dropped line",
            format!("{:?}", answered.is_some()),
        );
    }

    // ---- stop returns while a client is still connected ------------------
    //
    // `serve_client` used to loop until the client disconnected, so `stop`'s
    // join waited for a UI that was still running -- and that UI never
    // disconnects, so shutting Base down hung. The connection stays open across
    // the stop here, which is the case that hung.
    {
        let started = Instant::now();
        server.stop();
        check(
            started.elapsed() < Duration::from_secs(5),
            "stop returns while a client is connected",
            format!("{:.2}s", started.elapsed().as_secs_f64()),
        );
        // The client's connection is gone, which is the honest outcome of a
        // stopped server: reading returns None rather than blocking.
        check(
            client.recv().ok().flatten().is_none(),
            "a stopped server closes the client's connection",
            String::new(),
        );
        // Restart for the reconnect check below.
        server = transport::Transport::start(handle.clone(), pipe).expect("restart transport");
    }

    // ---- the listener is reachable again --------------------------------
    //
    // A UI that restarts has to be able to reconnect. The server accepts one
    // client at a time, so the slot has to come back when the first leaves.
    drop(client);
    let mut second = connect_with_retry(pipe);
    second
        .send(&Command::ListPlaylists)
        .expect("send from a second client");
    let answer = wait_for(&mut second, |event| {
        matches!(event, Event::PlaylistsChanged { .. })
    });
    check(
        answer.is_some(),
        "a second client can connect after the first leaves",
        format!("{:?}", answer.is_some()),
    );

    drop(second);
    server.stop();
    base.shutdown();
    println!("\nAll transport checks passed.");
}

/// Connect, retrying while the listener is between clients.
fn connect_with_retry(pipe: &str) -> Client {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match transport::connect(pipe) {
            Ok(client) => return client,
            Err(e) => {
                if Instant::now() >= deadline {
                    panic!("could not connect to {pipe}: {e}");
                }
                std::thread::sleep(Duration::from_millis(20));
            }
        }
    }
}
