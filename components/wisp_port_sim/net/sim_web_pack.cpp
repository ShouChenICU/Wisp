#include "wisp/port/sim/sim_web_pack.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>
#include <system_error>
#include <utility>

#include "wisp/base/sha256.hpp"

namespace wisp::port::sim {

namespace {

// Kept in step with tools/pack_assets.py's MIME_TYPES / GZIP_SUFFIXES.
const std::map<std::string_view, std::string_view>& mime_types() {
    static const std::map<std::string_view, std::string_view> kMime = {
        {"html", "text/html; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"svg", "image/svg+xml"},
        {"json", "application/json"},
        {"txt", "text/plain; charset=utf-8"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"ico", "image/x-icon"},
        {"woff2", "font/woff2"},
        {"wasm", "application/wasm"},
    };
    return kMime;
}

constexpr std::array<std::string_view, 7> kGzipSuffixes = {"html", "js",   "mjs", "css",
                                                           "svg",  "json", "txt"};
constexpr std::string_view kDefaultMime = "application/octet-stream";
constexpr uint8_t kFlagGzip = 0x01;
constexpr size_t kHeaderBytes = 16;

std::string suffix_of(const std::string& path) {
    size_t slash = path.rfind('/');
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string s = path.substr(dot + 1);
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

size_t align4(size_t n) { return (n + 3) & ~size_t{3}; }

void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

// A gzip member (RFC 1952) with mtime 0, like Python's gzip.compress(...,
// mtime=0).
bool gzip_bytes(const std::string& in, std::string& out) {
    z_stream zs{};
    if (deflateInit2(&zs, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    gz_header header{};
    header.os = 255;  // unknown
    deflateSetHeader(&zs, &header);
    out.resize(deflateBound(&zs, static_cast<uLong>(in.size())) + 32);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return rc == Z_STREAM_END;
}

struct Encoded {
    std::string path;
    std::string_view mime;
    uint8_t flags = 0;
    std::string stored;
};

}  // namespace

Result<std::vector<uint8_t>> pack_web_dir(const std::string& dir) {
    using R = Result<std::vector<uint8_t>>;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return R::err(ErrorCode::NotFound, dir + ": not a directory");
    }

    std::vector<Encoded> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        Encoded e;
        e.path = "/" + it->path().lexically_relative(dir).generic_string();
        std::ifstream in(it->path(), std::ios::binary);
        std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (!in.good() && !in.eof()) {
            return R::err(ErrorCode::IoError, "reading " + it->path().string());
        }
        std::string suffix = suffix_of(e.path);
        auto mime = mime_types().find(suffix);
        e.mime = mime != mime_types().end() ? mime->second : kDefaultMime;
        if (std::find(kGzipSuffixes.begin(), kGzipSuffixes.end(), suffix) != kGzipSuffixes.end()) {
            e.flags = kFlagGzip;
            if (!gzip_bytes(data, e.stored)) {
                return R::err(ErrorCode::IoError, "compressing " + e.path);
            }
        } else {
            e.stored = std::move(data);
        }
        files.push_back(std::move(e));
    }
    if (ec) {
        return R::err(ErrorCode::IoError, dir + ": " + ec.message());
    }
    if (files.size() > 0xFFFF) {
        return R::err(ErrorCode::InvalidArgument, "too many files");
    }
    std::sort(files.begin(), files.end(),
              [](const Encoded& a, const Encoded& b) { return a.path < b.path; });

    size_t table_bytes = 0;
    for (const Encoded& e : files) {
        table_bytes += 2 + e.path.size() + 1 + e.mime.size() + 1 + 4 + 4 + 32;
    }
    std::vector<uint8_t> table;
    std::vector<std::pair<size_t, const std::string*>> layout;
    size_t offset = align4(kHeaderBytes + table_bytes);
    for (const Encoded& e : files) {
        put16(table, static_cast<uint16_t>(e.path.size()));
        table.insert(table.end(), e.path.begin(), e.path.end());
        table.push_back(static_cast<uint8_t>(e.mime.size()));
        table.insert(table.end(), e.mime.begin(), e.mime.end());
        table.push_back(e.flags);
        put32(table, static_cast<uint32_t>(offset));
        put32(table, static_cast<uint32_t>(e.stored.size()));
        auto digest = sha256(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(e.stored.data()), e.stored.size()));
        table.insert(table.end(), digest.begin(), digest.end());
        layout.emplace_back(offset, &e.stored);
        offset = align4(offset + e.stored.size());
    }
    if (offset > 0xFFFFFFFFu) {
        return R::err(ErrorCode::InvalidArgument, "pack larger than 4 GiB");
    }

    std::vector<uint8_t> out;
    out.reserve(offset);
    out.insert(out.end(), {'W', 'P', 'A', 'K'});
    put16(out, 1);
    put16(out, static_cast<uint16_t>(files.size()));
    put32(out, static_cast<uint32_t>(table_bytes));
    put32(out, static_cast<uint32_t>(offset));
    out.insert(out.end(), table.begin(), table.end());
    out.resize(offset, 0);
    for (const auto& [at, stored] : layout) {
        std::copy(stored->begin(), stored->end(), out.begin() + static_cast<ptrdiff_t>(at));
    }
    return R::ok(std::move(out));
}

}  // namespace wisp::port::sim
