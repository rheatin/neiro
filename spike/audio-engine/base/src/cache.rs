//! The Base analysis cache.
//!
//! One owner: the analysis worker behind this cache. The UI asks for facts and
//! receives `AnalysisReady`; it never decodes a file itself and never writes a
//! cache entry.
//!
//! The key includes the file's size and modification time as well as the
//! analyzer version. A re-tagged file or a newer analyzer invalidates the old
//! entry without anyone having to remember to clear it.

use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use neiro_audio::Analysis;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct CacheKey {
    pub path: PathBuf,
    pub len: u64,
    pub modified_ns: Option<u128>,
    pub analyzer_version: u32,
}

impl CacheKey {
    pub fn for_path(path: &Path, analyzer_version: u32) -> Self {
        let (len, modified_ns) = match std::fs::metadata(path) {
            Ok(meta) => {
                let modified = meta
                    .modified()
                    .ok()
                    .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                    .map(|d| d.as_nanos());
                (meta.len(), modified)
            }
            Err(_) => (0, None),
        };
        Self {
            path: path.to_path_buf(),
            len,
            modified_ns,
            analyzer_version,
        }
    }
}

/// A bounded LRU. The bound matters because a library scan can outlive any one
/// track; an unbounded map would turn "analyze everything" into a leak.
pub struct Cache {
    map: HashMap<CacheKey, Analysis>,
    order: VecDeque<CacheKey>,
    max_entries: usize,
}

impl Default for Cache {
    fn default() -> Self {
        Self::with_capacity(256)
    }
}

impl Cache {
    pub fn with_capacity(max_entries: usize) -> Self {
        Self {
            map: HashMap::new(),
            order: VecDeque::new(),
            max_entries: max_entries.max(1),
        }
    }

    pub fn get(&mut self, key: &CacheKey) -> Option<Analysis> {
        let value = self.map.get(key)?.clone();
        self.touch(key);
        Some(value)
    }

    pub fn put(&mut self, key: CacheKey, value: Analysis) {
        if !self.map.contains_key(&key) {
            while self.map.len() >= self.max_entries {
                match self.order.pop_front() {
                    Some(old) => {
                        self.map.remove(&old);
                    }
                    None => break,
                }
            }
        }
        self.map.insert(key.clone(), value);
        self.touch(&key);
    }

    fn touch(&mut self, key: &CacheKey) {
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
        }
        self.order.push_back(key.clone());
    }
}
