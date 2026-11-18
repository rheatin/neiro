use std::sync::Mutex;
use std::time::Duration;

use crate::LyricsCandidate;

/// Builds an HTTP agent configured with a 6-second timeout.
pub fn create_agent() -> ureq::Agent {
    ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(6)))
        .build()
        .new_agent()
}

/// Decodes standard Base64 string into bytes.
pub fn decode_base64(input: &str) -> Option<Vec<u8>> {
    let mut clean = Vec::with_capacity(input.len());
    for b in input.bytes() {
        if !b.is_ascii_whitespace() {
            clean.push(b);
        }
    }
    if clean.is_empty() {
        return Some(Vec::new());
    }

    let decode_byte = |b: u8| -> Option<u8> {
        match b {
            b'A'..=b'Z' => Some(b - b'A'),
            b'a'..=b'z' => Some(b - b'a' + 26),
            b'0'..=b'9' => Some(b - b'0' + 52),
            b'+' | b'-' => Some(62),
            b'/' | b'_' => Some(63),
            _ => None,
        }
    };

    let mut out = Vec::with_capacity((clean.len() * 3) / 4);
    let chunks = clean.chunks_exact(4);
    let remainder = chunks.remainder();

    for chunk in chunks {
        let b0 = decode_byte(chunk[0])?;
        let b1 = decode_byte(chunk[1])?;
        let b2 = if chunk[2] == b'=' { None } else { Some(decode_byte(chunk[2])?) };
        let b3 = if chunk[3] == b'=' { None } else { Some(decode_byte(chunk[3])?) };

        out.push((b0 << 2) | (b1 >> 4));
        if let Some(c2) = b2 {
            out.push(((b1 & 0x0F) << 4) | (c2 >> 2));
            if let Some(c3) = b3 {
                out.push(((c2 & 0x03) << 6) | c3);
            }
        }
    }

    if !remainder.is_empty() {
        let b0 = decode_byte(remainder[0])?;
        if remainder.len() > 1 {
            let b1 = decode_byte(remainder[1])?;
            out.push((b0 << 2) | (b1 >> 4));
            if remainder.len() > 2 {
                let b2 = if remainder[2] == b'=' { None } else { Some(decode_byte(remainder[2])?) };
                if let Some(c2) = b2 {
                    out.push(((b1 & 0x0F) << 4) | (c2 >> 2));
                }
            }
        }
    }

    Some(out)
}

/// Decodes Base64 string into UTF-8 text.
pub fn decode_base64_to_string(input: &str) -> Option<String> {
    let bytes = decode_base64(input)?;
    String::from_utf8(bytes).ok()
}

/// Decodes numeric and named HTML entities like `&#58;`, `&#46;`, `&#10;`, `&amp;`.
pub fn decode_html_entities(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let mut chars = input.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch == '&' {
            let mut entity = String::new();
            while let Some(&next_ch) = chars.peek() {
                if next_ch == ';' {
                    chars.next();
                    break;
                } else if next_ch.is_alphanumeric() || next_ch == '#' {
                    entity.push(chars.next().unwrap());
                } else {
                    break;
                }
            }
            if entity.starts_with('#') {
                let code_point = if entity.starts_with("#x") || entity.starts_with("#X") {
                    u32::from_str_radix(&entity[2..], 16).ok()
                } else {
                    entity[1..].parse::<u32>().ok()
                };
                if let Some(cp) = code_point.and_then(char::from_u32) {
                    out.push(cp);
                    continue;
                }
            } else {
                match entity.as_str() {
                    "amp" => {
                        out.push('&');
                        continue;
                    }
                    "lt" => {
                        out.push('<');
                        continue;
                    }
                    "gt" => {
                        out.push('>');
                        continue;
                    }
                    "quot" => {
                        out.push('"');
                        continue;
                    }
                    "apos" => {
                        out.push('\'');
                        continue;
                    }
                    "nbsp" => {
                        out.push(' ');
                        continue;
                    }
                    _ => {}
                }
            }
            out.push('&');
            out.push_str(&entity);
        } else {
            out.push(ch);
        }
    }
    out
}

/// Percent-encodes query parameter values for HTTP requests.
pub fn percent_encode(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for byte in text.as_bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(*byte as char)
            }
            _ => out.push_str(&format!("%{byte:02X}")),
        }
    }
    out
}

// -----------------------------------------------------------------------------
// Provider 1: LRCLIB (builtin.lyrics.lrclib)
// -----------------------------------------------------------------------------

pub fn fetch_lrclib(
    agent: &ureq::Agent,
    artist: &str,
    title: &str,
    album: Option<&str>,
    duration_s: Option<f64>,
) -> Vec<LyricsCandidate> {
    let mut url = format!("https://lrclib.net/api/search?track_name={}", percent_encode(title));
    if !artist.trim().is_empty() {
        url.push_str(&format!("&artist_name={}", percent_encode(artist)));
    }
    if let Some(alb) = album.filter(|a| !a.trim().is_empty()) {
        url.push_str(&format!("&album_name={}", percent_encode(alb)));
    }
    let response = agent
        .get(&url)
        .header("User-Agent", "Neiro/0.1 (https://github.com/rheatin/neiro)")
        .header("Accept", "application/json")
        .call();
    let Ok(mut response) = response else {
        return Vec::new();
    };
    let Ok(body) = response.body_mut().read_to_string() else {
        return Vec::new();
    };
    let Ok(rows) = serde_json::from_str::<Vec<serde_json::Value>>(&body) else {
        return Vec::new();
    };

    let _ = duration_s;
    rows.iter()
        .filter_map(|row| {
            let text = |key: &str| row.get(key).and_then(|v| v.as_str()).map(str::to_string);
            let duration = row.get("duration").and_then(|v| v.as_f64()).unwrap_or(0.0);
            let synced = text("syncedLyrics").filter(|t| !t.trim().is_empty());
            let plain = text("plainLyrics").filter(|t| !t.trim().is_empty());
            if synced.is_none() && plain.is_none() {
                return None;
            }
            let raw_id = row.get("id").and_then(|v| v.as_f64()).unwrap_or(0.0);
            Some(LyricsCandidate {
                id: 1_000_000_000.0 + raw_id,
                track: text("trackName").unwrap_or_default(),
                artist: text("artistName").unwrap_or_default(),
                album: text("albumName").unwrap_or_default(),
                duration_s: duration,
                synced,
                plain,
                provider: "builtin.lyrics.lrclib".to_string(),
                provider_name: "LRCLIB".to_string(),
            })
        })
        .take(10)
        .collect()
}

// -----------------------------------------------------------------------------
// Provider 2: NetEase 网易云音乐 (builtin.lyrics.netease)
// -----------------------------------------------------------------------------

pub fn fetch_netease(
    agent: &ureq::Agent,
    artist: &str,
    title: &str,
    _album: Option<&str>,
    _duration_s: Option<f64>,
) -> Vec<LyricsCandidate> {
    let query = if artist.trim().is_empty() {
        title.trim().to_string()
    } else {
        format!("{} {}", title.trim(), artist.trim())
    };
    let search_url = format!(
        "https://music.163.com/api/search/get/web?s={}&type=1&offset=0&total=true&limit=5",
        percent_encode(&query)
    );
    let response = agent
        .get(&search_url)
        .header(
            "User-Agent",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
        )
        .header("Referer", "https://music.163.com/")
        .header("Accept", "application/json")
        .call();
    let Ok(mut response) = response else {
        return Vec::new();
    };
    let Ok(body) = response.body_mut().read_to_string() else {
        return Vec::new();
    };
    let Ok(val) = serde_json::from_str::<serde_json::Value>(&body) else {
        return Vec::new();
    };
    let Some(songs) = val.get("result").and_then(|r| r.get("songs")).and_then(|s| s.as_array()) else {
        return Vec::new();
    };

    let mut candidates = Vec::new();
    for song in songs.iter().take(4) {
        let Some(song_id) = song.get("id").and_then(|v| v.as_i64()) else {
            continue;
        };
        let track_name = song
            .get("name")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let artist_name = song
            .get("artists")
            .and_then(|a| a.as_array())
            .and_then(|arr| arr.first())
            .and_then(|art| art.get("name"))
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let album_name = song
            .get("album")
            .and_then(|a| a.get("name"))
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let duration_ms = song.get("duration").and_then(|v| v.as_f64()).unwrap_or(0.0);

        // Fetch lyrics for this song
        let lyric_url = format!(
            "https://music.163.com/api/song/lyric?os=pc&id={}&lv=-1&kv=-1&tv=-1",
            song_id
        );
        let Ok(mut lyr_resp) = agent
            .get(&lyric_url)
            .header(
                "User-Agent",
                "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
            )
            .header("Referer", "https://music.163.com/")
            .header("Accept", "application/json")
            .call()
        else {
            continue;
        };
        let Ok(lyr_body) = lyr_resp.body_mut().read_to_string() else {
            continue;
        };
        let Ok(lyr_val) = serde_json::from_str::<serde_json::Value>(&lyr_body) else {
            continue;
        };
        let lyric_text = lyr_val
            .get("lrc")
            .and_then(|l| l.get("lyric"))
            .and_then(|v| v.as_str())
            .map(|s| s.trim().to_string());

        let Some(lrc) = lyric_text.filter(|s| !s.is_empty()) else {
            continue;
        };
        let (synced, plain) = if lrc.contains('[') && lrc.contains(']') {
            (Some(lrc), None)
        } else {
            (None, Some(lrc))
        };

        candidates.push(LyricsCandidate {
            id: 2_000_000_000.0 + (song_id as f64),
            track: track_name,
            artist: artist_name,
            album: album_name,
            duration_s: duration_ms / 1000.0,
            synced,
            plain,
            provider: "builtin.lyrics.netease".to_string(),
            provider_name: "网易云".to_string(),
        });
    }
    candidates
}

// -----------------------------------------------------------------------------
// Provider 3: QQ Music QQ音乐 (builtin.lyrics.qqmusic)
// -----------------------------------------------------------------------------

pub fn fetch_qqmusic(
    agent: &ureq::Agent,
    artist: &str,
    title: &str,
    _album: Option<&str>,
    _duration_s: Option<f64>,
) -> Vec<LyricsCandidate> {
    let query = if artist.trim().is_empty() {
        title.trim().to_string()
    } else {
        format!("{} {}", title.trim(), artist.trim())
    };
    let search_url = format!(
        "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?p=1&n=5&w={}&format=json",
        percent_encode(&query)
    );
    let response = agent
        .get(&search_url)
        .header(
            "User-Agent",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
        )
        .header("Referer", "https://y.qq.com/")
        .header("Accept", "application/json")
        .call();
    let Ok(mut response) = response else {
        return Vec::new();
    };
    let Ok(body) = response.body_mut().read_to_string() else {
        return Vec::new();
    };
    let Ok(val) = serde_json::from_str::<serde_json::Value>(&body) else {
        return Vec::new();
    };
    let Some(list) = val
        .get("data")
        .and_then(|d| d.get("song"))
        .and_then(|s| s.get("list"))
        .and_then(|l| l.as_array())
    else {
        return Vec::new();
    };

    let mut candidates = Vec::new();
    for item in list.iter().take(4) {
        let Some(songmid) = item.get("songmid").and_then(|v| v.as_str()) else {
            continue;
        };
        let songid = item.get("songid").and_then(|v| v.as_f64()).unwrap_or(0.0);
        let track_name = item
            .get("songname")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let artist_name = item
            .get("singer")
            .and_then(|s| s.as_array())
            .and_then(|arr| arr.first())
            .and_then(|art| art.get("name"))
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let album_name = item
            .get("albumname")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let duration_s = item.get("interval").and_then(|v| v.as_f64()).unwrap_or(0.0);

        // Fetch lyrics
        let lyr_url = format!(
            "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid={}&format=json&nobase64=1",
            songmid
        );
        let Ok(mut lyr_resp) = agent
            .get(&lyr_url)
            .header(
                "User-Agent",
                "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
            )
            .header("Referer", "https://y.qq.com/")
            .header("Accept", "application/json")
            .call()
        else {
            continue;
        };
        let Ok(lyr_body) = lyr_resp.body_mut().read_to_string() else {
            continue;
        };
        let Ok(lyr_val) = serde_json::from_str::<serde_json::Value>(&lyr_body) else {
            continue;
        };
        let raw_lyric = lyr_val.get("lyric").and_then(|v| v.as_str());
        let Some(raw) = raw_lyric.filter(|s| !s.trim().is_empty()) else {
            continue;
        };

        // If base64 encoded, decode it; then decode HTML entities like &#58; &#10;
        let decoded = if let Some(b64_str) = decode_base64_to_string(raw) {
            decode_html_entities(&b64_str)
        } else {
            decode_html_entities(raw)
        };

        if decoded.trim().is_empty() {
            continue;
        }

        let (synced, plain) = if decoded.contains('[') && decoded.contains(']') {
            (Some(decoded), None)
        } else {
            (None, Some(decoded))
        };

        candidates.push(LyricsCandidate {
            id: 3_000_000_000.0 + songid,
            track: track_name,
            artist: artist_name,
            album: album_name,
            duration_s,
            synced,
            plain,
            provider: "builtin.lyrics.qqmusic".to_string(),
            provider_name: "QQ音乐".to_string(),
        });
    }
    candidates
}

// -----------------------------------------------------------------------------
// Provider 4: Kugou 酷狗音乐 (builtin.lyrics.kugou)
// -----------------------------------------------------------------------------

pub fn fetch_kugou(
    agent: &ureq::Agent,
    artist: &str,
    title: &str,
    _album: Option<&str>,
    _duration_s: Option<f64>,
) -> Vec<LyricsCandidate> {
    let query = if artist.trim().is_empty() {
        title.trim().to_string()
    } else {
        format!("{} {}", title.trim(), artist.trim())
    };
    let search_url = format!(
        "http://mobilecdn.kugou.com/api/v3/search/song?format=json&keyword={}&page=1&pagesize=5",
        percent_encode(&query)
    );
    let response = agent
        .get(&search_url)
        .header(
            "User-Agent",
            "Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X)",
        )
        .header("Accept", "application/json")
        .call();
    let Ok(mut response) = response else {
        return Vec::new();
    };
    let Ok(body) = response.body_mut().read_to_string() else {
        return Vec::new();
    };
    let Ok(val) = serde_json::from_str::<serde_json::Value>(&body) else {
        return Vec::new();
    };
    let Some(info) = val
        .get("data")
        .and_then(|d| d.get("info"))
        .and_then(|i| i.as_array())
    else {
        return Vec::new();
    };

    let mut candidates = Vec::new();
    for (idx, item) in info.iter().take(3).enumerate() {
        let Some(hash) = item.get("hash").and_then(|v| v.as_str()) else {
            continue;
        };
        let songname = item
            .get("songname")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let singername = item
            .get("singername")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let album_name = item
            .get("album_name")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let duration = item.get("duration").and_then(|v| v.as_f64()).unwrap_or(0.0);

        // Step 1: Query lyric candidate id and accesskey by hash
        let search_cand_url = format!(
            "http://krcs.kugou.com/search?ver=1&man=yes&client=mobi&keyword=&duration=&hash={}",
            hash
        );
        let Ok(mut cand_resp) = agent.get(&search_cand_url).call() else {
            continue;
        };
        let Ok(cand_body) = cand_resp.body_mut().read_to_string() else {
            continue;
        };
        let Ok(cand_val) = serde_json::from_str::<serde_json::Value>(&cand_body) else {
            continue;
        };
        let Some(first_cand) = cand_val
            .get("candidates")
            .and_then(|c| c.as_array())
            .and_then(|arr| arr.first())
        else {
            continue;
        };

        let cand_id = first_cand
            .get("id")
            .and_then(|v| {
                if let Some(s) = v.as_str() {
                    Some(s.to_string())
                } else {
                    v.as_i64().map(|n| n.to_string())
                }
            })
            .unwrap_or_default();
        let accesskey = first_cand
            .get("accesskey")
            .and_then(|v| v.as_str())
            .unwrap_or_default();
        if cand_id.is_empty() || accesskey.is_empty() {
            continue;
        }

        // Step 2: Download lyric content
        let dl_url = format!(
            "http://lyrics.kugou.com/download?ver=1&client=pc&id={}&accesskey={}&fmt=lrc&charset=utf8",
            cand_id, accesskey
        );
        let Ok(mut dl_resp) = agent.get(&dl_url).call() else {
            continue;
        };
        let Ok(dl_body) = dl_resp.body_mut().read_to_string() else {
            continue;
        };
        let Ok(dl_val) = serde_json::from_str::<serde_json::Value>(&dl_body) else {
            continue;
        };
        let Some(content_b64) = dl_val.get("content").and_then(|v| v.as_str()) else {
            continue;
        };
        let Some(lrc_text) = decode_base64_to_string(content_b64) else {
            continue;
        };
        if lrc_text.trim().is_empty() {
            continue;
        }

        let (synced, plain) = if lrc_text.contains('[') && lrc_text.contains(']') {
            (Some(lrc_text), None)
        } else {
            (None, Some(lrc_text))
        };

        let num_id = cand_id.parse::<f64>().unwrap_or(idx as f64);
        candidates.push(LyricsCandidate {
            id: 4_000_000_000.0 + num_id,
            track: songname,
            artist: singername,
            album: album_name,
            duration_s: duration,
            synced,
            plain,
            provider: "builtin.lyrics.kugou".to_string(),
            provider_name: "酷狗".to_string(),
        });
    }
    candidates
}

// -----------------------------------------------------------------------------
// Multi-Source Concurrent Lyrics Search Engine
// -----------------------------------------------------------------------------

/// Queries all enabled lyrics providers concurrently and aggregates the candidates.
pub fn search_all_lyrics(
    artist: &str,
    title: &str,
    album: Option<&str>,
    duration_s: Option<f64>,
    enabled_providers: Option<&[String]>,
) -> Vec<LyricsCandidate> {
    let is_enabled = |id: &str| -> bool {
        match enabled_providers {
            None => true,
            Some(list) => {
                if list.is_empty() {
                    false
                } else {
                    list.iter().any(|p| p == id)
                }
            }
        }
    };

    let run_lrclib = is_enabled("builtin.lyrics.lrclib");
    let run_netease = is_enabled("builtin.lyrics.netease");
    let run_qqmusic = is_enabled("builtin.lyrics.qqmusic");
    let run_kugou = is_enabled("builtin.lyrics.kugou");

    let results = Mutex::new(Vec::new());

    std::thread::scope(|s| {
        if run_lrclib {
            s.spawn(|| {
                let agent = create_agent();
                let items = fetch_lrclib(&agent, artist, title, album, duration_s);
                if let Ok(mut guard) = results.lock() {
                    guard.extend(items);
                }
            });
        }
        if run_netease {
            s.spawn(|| {
                let agent = create_agent();
                let items = fetch_netease(&agent, artist, title, album, duration_s);
                if let Ok(mut guard) = results.lock() {
                    guard.extend(items);
                }
            });
        }
        if run_qqmusic {
            s.spawn(|| {
                let agent = create_agent();
                let items = fetch_qqmusic(&agent, artist, title, album, duration_s);
                if let Ok(mut guard) = results.lock() {
                    guard.extend(items);
                }
            });
        }
        if run_kugou {
            s.spawn(|| {
                let agent = create_agent();
                let items = fetch_kugou(&agent, artist, title, album, duration_s);
                if let Ok(mut guard) = results.lock() {
                    guard.extend(items);
                }
            });
        }
    });

    results.into_inner().unwrap_or_default()
}

// -----------------------------------------------------------------------------
// Unit Tests
// -----------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_decode_base64() {
        assert_eq!(decode_base64(""), Some(b"".to_vec()));
        assert_eq!(decode_base64("TmVpcm8="), Some(b"Neiro".to_vec()));
        assert_eq!(decode_base64("SGVsbG8gV29ybGQ="), Some(b"Hello World".to_vec()));
        assert_eq!(
            decode_base64_to_string("WzAwOjAwLjAwXeatjOivjQ=="),
            Some("[00:00.00]歌词".to_string())
        );
        // Whitespace tolerance
        assert_eq!(decode_base64(" T mV\n p c m 8 = "), Some(b"Neiro".to_vec()));
    }

    #[test]
    fn test_decode_html_entities() {
        let input = "&#58;&#46;&#10;&amp;&lt;&gt;&quot;&apos;&#32;";
        assert_eq!(decode_html_entities(input), ":.\n&<>\"' ");

        let lrc_line = "&#91;01&#58;23&#46;45&#93;&#10;";
        assert_eq!(decode_html_entities(lrc_line), "[01:23.45]\n");
    }

    #[test]
    fn test_percent_encode() {
        assert_eq!(percent_encode("Hello World"), "Hello%20World");
        assert_eq!(percent_encode("周杰伦"), "%E5%91%A8%E6%9D%B0%E4%BC%A6");
        assert_eq!(percent_encode("A-Z_.~"), "A-Z_.~");
    }

    #[test]
    fn test_candidate_ids_unique_across_providers() {
        let c1 = LyricsCandidate {
            id: 1_000_000_000.0 + 123.0,
            provider: "builtin.lyrics.lrclib".into(),
            provider_name: "LRCLIB".into(),
            ..Default::default()
        };
        let c2 = LyricsCandidate {
            id: 2_000_000_000.0 + 123.0,
            provider: "builtin.lyrics.netease".into(),
            provider_name: "网易云".into(),
            ..Default::default()
        };
        let c3 = LyricsCandidate {
            id: 3_000_000_000.0 + 123.0,
            provider: "builtin.lyrics.qqmusic".into(),
            provider_name: "QQ音乐".into(),
            ..Default::default()
        };
        let c4 = LyricsCandidate {
            id: 4_000_000_000.0 + 123.0,
            provider: "builtin.lyrics.kugou".into(),
            provider_name: "酷狗".into(),
            ..Default::default()
        };

        let ids = vec![c1.id, c2.id, c3.id, c4.id];
        let mut deduped = ids.clone();
        deduped.sort_by(|a, b| a.partial_cmp(b).unwrap());
        deduped.dedup();
        assert_eq!(ids.len(), deduped.len());
    }
}
