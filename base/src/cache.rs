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

    /// Make room for `count` more entries, without ever shrinking.
    ///
    /// A library scan of 5000 tracks against a 256-entry cache would evict the
    /// first 4744 results as it produced them -- a scan that costs ten minutes
    /// and leaves a fortieth of it behind. Growing is bounded by what the caller
    /// actually asked for, and a later scan of a smaller set does not shrink it
    /// back, because the entries are still worth keeping.
    ///
    /// The memory is proportional to the library: an `Analysis` is a few hundred
    /// bytes, so 5000 entries is single-digit megabytes.
    pub fn reserve(&mut self, count: usize) {
        self.max_entries = self.max_entries.max(count.max(1));
    }

    /// The current bound, for a check that needs to see it grow.
    #[cfg(test)]
    pub fn capacity(&self) -> usize {
        self.max_entries
    }

    fn touch(&mut self, key: &CacheKey) {
        if let Some(pos) = self.order.iter().position(|k| k == key) {
            self.order.remove(pos);
        }
        self.order.push_back(key.clone());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Growing is the whole point: a scan of N against a 256-entry cache would
    /// evict its own early results as it produced them.
    #[test]
    fn reserve_grows_and_never_shrinks() {
        let mut cache = Cache::default();
        assert_eq!(cache.capacity(), 256, "the default bound");

        cache.reserve(300);
        assert_eq!(cache.capacity(), 300, "grown to the batch");

        // A later, smaller batch must not shrink it: those 300 entries are
        // still worth keeping, and dropping them to honour a smaller request
        // would make the second scan throw away the first one's work.
        cache.reserve(10);
        assert_eq!(cache.capacity(), 300, "a smaller batch does not shrink it");
    }

    /// Past the bound, the oldest entry is the one that goes.
    #[test]
    fn eviction_keeps_the_most_recently_used() {
        let mut cache = Cache::with_capacity(2);
        let key = |n: u32| CacheKey {
            path: PathBuf::from(format!("/music/{n}.flac")),
            len: 0,
            modified_ns: None,
            analyzer_version: 1,
        };
        let value = Analysis::default();
        cache.put(key(0), value.clone());
        cache.put(key(1), value.clone());
        // Touching 0 makes 1 the least recently used.
        assert!(cache.get(&key(0)).is_some());
        cache.put(key(2), value);
        assert!(cache.get(&key(0)).is_some(), "the touched entry survived");
        assert!(cache.get(&key(1)).is_none(), "the untouched one was evicted");
    }
}
