#pragma once
// The track index in memory and its JSON form in <root>/.index.json
// (media spec 3.3). Every string lives in Mem::Large: the index holds a few
// strings per track for every track on the card.

#include <cstdint>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/os/large.hpp"

namespace wisp::media {

struct IndexEntry {
    os::LargeString id, title, artist, album;
    uint32_t duration_s = 0;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool operator==(const IndexEntry&) const = default;
};

// Sorted by id, ids unique.
using Index = os::LargeVector<IndexEntry>;

// {"version":1,"tracks":[{"id","title","artist","album","duration_s","size","mtime"}]}
os::LargeString encode_index(const Index& index);

// The entries of an index file, sorted and without duplicates. Corrupt
// when it is not JSON of the shape above, Unsupported for another version.
// Entries whose id is not a track path under music/ are dropped.
Result<Index> decode_index(std::string_view json);

// Sorts by id and keeps the first of each id.
void sort_index(Index& index);

}  // namespace wisp::media
