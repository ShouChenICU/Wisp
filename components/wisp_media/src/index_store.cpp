#include "index_store.hpp"

#include <algorithm>
#include <string>

#include "wisp/base/json.hpp"
#include "wisp/media/library.hpp"
#include "wisp/media/path.hpp"
#include "wisp/os/os.hpp"

namespace wisp::media {

namespace {

// A parsed index can be a few hundred KB in many small blocks; on the
// device small mallocs land in internal RAM (AGENTS.md section 8).
const JsonAlloc& large_alloc() {
    static const JsonAlloc alloc{
        [](size_t n) { return os::alloc(n, os::Mem::Large); },
        [](void* p) { os::dealloc(p); },
        [](void* p, size_t n) { return os::realloc(p, n, os::Mem::Large); },
    };
    return alloc;
}

void append_field(os::LargeString& out, const char* key, const os::LargeString& value) {
    out.push_back('"');
    out.append(key);
    out.append("\":");
    append_json_string(out, std::string_view(value.data(), value.size()));
}

void append_number(os::LargeString& out, const char* key, const std::string& value) {
    out.push_back('"');
    out.append(key);
    out.append("\":");
    out.append(value);
}

os::LargeString text_of(JsonView v) {
    std::string_view s = v.string();
    return os::LargeString(s.data(), s.size());
}

}  // namespace

void sort_index(Index& index) {
    std::stable_sort(index.begin(), index.end(),
                     [](const IndexEntry& a, const IndexEntry& b) { return a.id < b.id; });
    index.erase(std::unique(index.begin(), index.end(),
                            [](const IndexEntry& a, const IndexEntry& b) { return a.id == b.id; }),
                index.end());
}

os::LargeString encode_index(const Index& index) {
    os::LargeString out;
    out.reserve(32 + index.size() * 160);
    out.append("{\"version\":");
    out.append(std::to_string(kIndexVersion));
    out.append(",\"tracks\":[");
    bool first = true;
    for (const IndexEntry& e : index) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('{');
        append_field(out, "id", e.id);
        out.push_back(',');
        append_field(out, "title", e.title);
        out.push_back(',');
        append_field(out, "artist", e.artist);
        out.push_back(',');
        append_field(out, "album", e.album);
        out.push_back(',');
        append_number(out, "duration_s", std::to_string(e.duration_s));
        out.push_back(',');
        append_number(out, "size", std::to_string(e.size));
        out.push_back(',');
        append_number(out, "mtime", std::to_string(e.mtime));
        out.push_back('}');
    }
    out.append("]}");
    return out;
}

Result<Index> decode_index(std::string_view json) {
    using R = Result<Index>;
    // The document is three levels deep.
    Result<LargeJson> doc = parse_json_large(json, large_alloc(), 4);
    if (!doc) return R::err(ErrorCode::Corrupt, "index: " + doc.error().message);
    JsonView root = doc.value().root();
    if (root.type() != JsonView::Type::Object)
        return R::err(ErrorCode::Corrupt, "index: not an object");
    std::optional<int64_t> version = root.find("version").integer();
    if (!version || *version != kIndexVersion)
        return R::err(ErrorCode::Unsupported, "index: other version");
    JsonView tracks = root.find("tracks");
    if (tracks.type() != JsonView::Type::Array)
        return R::err(ErrorCode::Corrupt, "index: no tracks");

    Index out;
    out.reserve(tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) {
        JsonView t = tracks.at(i);
        JsonView id = t.find("id");
        if (id.type() != JsonView::Type::String) continue;
        std::string_view sid = id.string();
        if (!normalize(sid) || sid.substr(0, 6) != "music/" || !is_track_name(sid)) continue;
        std::optional<int64_t> size = t.find("size").integer();
        std::optional<int64_t> mtime = t.find("mtime").integer();
        std::optional<int64_t> duration = t.find("duration_s").integer();
        if (!size || *size < 0 || !mtime) continue;
        IndexEntry e;
        e.id = text_of(id);
        e.title = text_of(t.find("title"));
        e.artist = text_of(t.find("artist"));
        e.album = text_of(t.find("album"));
        e.duration_s = duration && *duration > 0 && *duration <= UINT32_MAX
                           ? static_cast<uint32_t>(*duration)
                           : 0;
        e.size = static_cast<uint64_t>(*size);
        e.mtime = *mtime;
        out.push_back(std::move(e));
    }
    sort_index(out);
    return R::ok(std::move(out));
}

}  // namespace wisp::media
