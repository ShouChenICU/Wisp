// Lua <-> JSON (lua_json.hpp). The encoder calls only Lua APIs that cannot
// raise (lua_type, lua_to* on values of the matching type, lua_next on a
// table nobody changes meanwhile, lua_rawgeti, pushes into slots reserved
// up front), because its caller holds the output string. The decoder runs
// inside protect() and holds no C++ objects with destructors.

#include "wisp/scene/lua_json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>

#include "wisp/scene/vm.hpp"

namespace wisp::scene {

namespace {

// How often both directions look at the VM's limits.
constexpr uint32_t kCheckEvery = 256;

// --- Encoding ------------------------------------------------------------------

struct Enc {
    lua_State* L;
    os::LargeString& out;
    size_t max;
    Vm* vm;
    uint32_t count = 0;
    int depth = 0;
    // The tables being walked, outermost first, to tell a cycle from a
    // table that is merely reached twice.
    const void* path[kLuaJsonMaxDepth] = {};
    const char* err = nullptr;
    // Leave out the outermost table's "id" field (lua_record_to_json).
    bool skip_id = false;
};

// The key at `idx`, of a table `level` tables deep (0: the value
// itself), is the "id" field a record leaves out.
bool skipped_key(const Enc& e, int idx, int level) {
    if (!e.skip_id || level != 0 || lua_type(e.L, idx) != LUA_TSTRING) return false;
    size_t n = 0;
    const char* k = lua_tolstring(e.L, idx, &n);
    return n == 2 && k[0] == 'i' && k[1] == 'd';
}

bool fail(Enc& e, const char* code) {
    e.err = code;
    return false;
}

bool put(Enc& e, const char* s, size_t n) {
    if (n > e.max - e.out.size()) return fail(e, kJsonTooLarge);
    e.out.append(s, n);
    return true;
}

bool put(Enc& e, char c) { return put(e, &c, 1); }

bool tick(Enc& e) {
    if (++e.count % kCheckEvery == 0 && e.vm != nullptr && e.vm->past_limits())
        return fail(e, kJsonLimits);
    return true;
}

// Length of the UTF-8 sequence at s[0..n), or 0 if it is not one
// (overlong forms, surrogates and code points past U+10FFFF included).
size_t utf8_len(const unsigned char* s, size_t n) {
    const unsigned char c = s[0];
    if (c < 0x80) return 1;
    size_t len = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (n < len) return 0;
    for (size_t i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

bool put_string(Enc& e, const char* s, size_t n) {
    const auto* u = reinterpret_cast<const unsigned char*>(s);
    if (!put(e, '"')) return false;
    size_t run = 0;  // start of the bytes not yet written
    size_t i = 0;
    while (i < n) {
        const unsigned char c = u[i];
        if (c >= 0x80) {
            const size_t len = utf8_len(u + i, n - i);
            if (len == 0) return fail(e, kJsonNotSerializable);
            i += len;
            continue;
        }
        if (c >= 0x20 && c != '"' && c != '\\') {
            ++i;
            continue;
        }
        if (!put(e, s + run, i - run)) return false;
        char esc[8] = {'\\', 0};
        size_t len = 2;
        switch (c) {
            case '"':
                esc[1] = '"';
                break;
            case '\\':
                esc[1] = '\\';
                break;
            case '\n':
                esc[1] = 'n';
                break;
            case '\r':
                esc[1] = 'r';
                break;
            case '\t':
                esc[1] = 't';
                break;
            case '\b':
                esc[1] = 'b';
                break;
            case '\f':
                esc[1] = 'f';
                break;
            default: {
                static constexpr char kHex[] = "0123456789abcdef";
                std::memcpy(esc, "\\u00", 4);
                esc[4] = kHex[c >> 4];
                esc[5] = kHex[c & 0xF];
                len = 6;
            }
        }
        if (!put(e, esc, len)) return false;
        run = ++i;
    }
    return put(e, s + run, n - run) && put(e, '"');
}

bool put_number(Enc& e, int idx) {
    char buf[40];
    std::to_chars_result r;
    if (lua_isinteger(e.L, idx)) {
        r = std::to_chars(buf, buf + sizeof(buf), static_cast<int64_t>(lua_tointeger(e.L, idx)));
    } else {
        const double d = static_cast<double>(lua_tonumber(e.L, idx));
        if (!std::isfinite(d)) return fail(e, kJsonNotSerializable);
        // Shortest text that reads back as the same double.
        r = std::to_chars(buf, buf + sizeof(buf) - 2, d);
        if (r.ec != std::errc()) return fail(e, kJsonNotSerializable);
        // "3" would come back as an integer.
        bool plain = true;
        for (const char* p = buf; p < r.ptr; ++p)
            if (*p == '.' || *p == 'e') plain = false;
        if (plain) {
            *r.ptr++ = '.';
            *r.ptr++ = '0';
        }
    }
    if (r.ec != std::errc()) return fail(e, kJsonNotSerializable);
    return put(e, buf, static_cast<size_t>(r.ptr - buf));
}

bool encode(Enc& e, int idx);

// The table at `t` (absolute) is an array of `n` elements.
bool encode_array(Enc& e, int t, lua_Integer n) {
    lua_State* L = e.L;
    if (!put(e, '[')) return false;
    for (lua_Integer i = 1; i <= n; ++i) {
        if (i > 1 && !put(e, ',')) return false;
        lua_rawgeti(L, t, i);
        const bool ok = encode(e, lua_gettop(L));
        lua_pop(L, 1);
        if (!ok) return false;
    }
    return put(e, ']');
}

// The table at `t` (absolute) has only string keys, at least one.
bool encode_object(Enc& e, int t) {
    lua_State* L = e.L;
    if (!put(e, '{')) return false;
    bool first = true;
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        // encode_table() has already counted this table in e.depth.
        if (skipped_key(e, -2, e.depth - 1)) {
            lua_pop(L, 1);
            continue;
        }
        size_t n = 0;
        const char* k = lua_tolstring(L, -2, &n);  // a string: no conversion
        bool ok = (first || put(e, ',')) && put_string(e, k, n) && put(e, ':') &&
                  encode(e, lua_gettop(L));
        first = false;
        lua_pop(L, 1);
        if (!ok) {
            lua_pop(L, 1);
            return false;
        }
    }
    return put(e, '}');
}

bool encode_table(Enc& e, int t) {
    lua_State* L = e.L;
    const void* self = lua_topointer(L, t);
    for (int i = 0; i < e.depth; ++i)
        if (e.path[i] == self) return fail(e, kJsonNotSerializable);
    if (e.depth == kLuaJsonMaxDepth) return fail(e, kJsonTooDeep);

    // Classify the keys: all strings (object), or all integers 1..n.
    lua_Integer n = 0;
    lua_Integer top = 0;
    bool ints = true;
    bool strings = true;
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        lua_pop(L, 1);
        if (skipped_key(e, -1, e.depth)) continue;
        ++n;
        if (lua_type(L, -1) == LUA_TSTRING) {
            ints = false;
        } else if (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 1) {
            strings = false;
            if (lua_tointeger(L, -1) > top) top = lua_tointeger(L, -1);
        } else {
            ints = strings = false;
        }
        if ((!ints && !strings) || !tick(e)) {
            lua_pop(L, 1);
            return e.err != nullptr ? false : fail(e, kJsonNotSerializable);
        }
    }
    if (n == 0) return put(e, "{}", 2);
    // Distinct integer keys >= 1, as many as the largest: exactly 1..n.
    if (ints && top != n) return fail(e, kJsonNotSerializable);

    e.path[e.depth++] = self;
    const bool ok = ints ? encode_array(e, t, n) : encode_object(e, t);
    --e.depth;
    return ok;
}

bool encode(Enc& e, int idx) {
    if (!tick(e)) return false;
    lua_State* L = e.L;
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            return put(e, "null", 4);
        case LUA_TBOOLEAN:
            return lua_toboolean(L, idx) ? put(e, "true", 4) : put(e, "false", 5);
        case LUA_TNUMBER:
            return put_number(e, idx);
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, idx, &n);
            return put_string(e, s, n);
        }
        case LUA_TTABLE:
            return encode_table(e, idx);
        default:
            return fail(e, kJsonNotSerializable);
    }
}

// --- Decoding ------------------------------------------------------------------

struct Dec {
    lua_State* L;
    const char* p;
    const char* end;
    const char* begin;
    Vm* vm;
    uint32_t count = 0;
};

[[noreturn]] void bad(Dec& d) {
    luaL_error(d.L, "invalid JSON at byte %d", static_cast<int>(d.p - d.begin));
    std::abort();  // luaL_error does not return
}

void skip_ws(Dec& d) {
    while (d.p < d.end && (*d.p == ' ' || *d.p == '\t' || *d.p == '\n' || *d.p == '\r')) ++d.p;
}

void expect(Dec& d, const char* word, size_t n) {
    if (static_cast<size_t>(d.end - d.p) < n || std::memcmp(d.p, word, n) != 0) bad(d);
    d.p += n;
}

int hex4(Dec& d) {
    if (d.end - d.p < 4) bad(d);
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = *d.p++;
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            bad(d);
        }
    }
    return v;
}

void add_utf8(luaL_Buffer* b, uint32_t cp) {
    char u[4];
    size_t n = 0;
    if (cp < 0x80) {
        u[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        u[n++] = static_cast<char>(0xC0 | (cp >> 6));
        u[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        u[n++] = static_cast<char>(0xE0 | (cp >> 12));
        u[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        u[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        u[n++] = static_cast<char>(0xF0 | (cp >> 18));
        u[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        u[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        u[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    luaL_addlstring(b, u, n);
}

// A string with escapes, from just after its opening quote. Kept out of
// line so its luaL_Buffer is not part of every recursive frame.
[[gnu::noinline]] void decode_escaped(Dec& d) {
    luaL_Buffer b;
    luaL_buffinit(d.L, &b);
    const char* run = d.p;
    while (true) {
        if (d.p >= d.end) bad(d);
        const char c = *d.p;
        if (c == '"') break;
        if (static_cast<unsigned char>(c) < 0x20) bad(d);
        if (c != '\\') {
            ++d.p;
            continue;
        }
        luaL_addlstring(&b, run, static_cast<size_t>(d.p - run));
        ++d.p;
        if (d.p >= d.end) bad(d);
        const char esc = *d.p++;
        switch (esc) {
            case '"':
                luaL_addchar(&b, '"');
                break;
            case '\\':
                luaL_addchar(&b, '\\');
                break;
            case '/':
                luaL_addchar(&b, '/');
                break;
            case 'b':
                luaL_addchar(&b, '\b');
                break;
            case 'f':
                luaL_addchar(&b, '\f');
                break;
            case 'n':
                luaL_addchar(&b, '\n');
                break;
            case 'r':
                luaL_addchar(&b, '\r');
                break;
            case 't':
                luaL_addchar(&b, '\t');
                break;
            case 'u': {
                uint32_t cp = static_cast<uint32_t>(hex4(d));
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // A high surrogate must be followed by a low one.
                    if (d.end - d.p < 2 || d.p[0] != '\\' || d.p[1] != 'u') bad(d);
                    d.p += 2;
                    const uint32_t lo = static_cast<uint32_t>(hex4(d));
                    if (lo < 0xDC00 || lo > 0xDFFF) bad(d);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    bad(d);
                }
                add_utf8(&b, cp);
                break;
            }
            default:
                bad(d);
        }
        run = d.p;
    }
    luaL_addlstring(&b, run, static_cast<size_t>(d.p - run));
    ++d.p;  // the closing quote
    luaL_pushresult(&b);
}

// From the opening quote; pushes the string.
void decode_string(Dec& d) {
    ++d.p;
    const char* s = d.p;
    for (const char* q = s; q < d.end; ++q) {
        const char c = *q;
        if (c == '"') {
            lua_pushlstring(d.L, s, static_cast<size_t>(q - s));
            d.p = q + 1;
            return;
        }
        if (c == '\\') break;
        if (static_cast<unsigned char>(c) < 0x20) {
            d.p = q;
            bad(d);
        }
    }
    decode_escaped(d);
}

bool digit(char c) { return c >= '0' && c <= '9'; }

void decode_number(Dec& d) {
    const char* s = d.p;
    const char* q = s;
    bool integral = true;
    if (q < d.end && *q == '-') ++q;
    if (q >= d.end || !digit(*q)) bad(d);
    if (*q == '0') {
        ++q;
    } else {
        while (q < d.end && digit(*q)) ++q;
    }
    if (q < d.end && *q == '.') {
        integral = false;
        ++q;
        if (q >= d.end || !digit(*q)) bad(d);
        while (q < d.end && digit(*q)) ++q;
    }
    if (q < d.end && (*q == 'e' || *q == 'E')) {
        integral = false;
        ++q;
        if (q < d.end && (*q == '+' || *q == '-')) ++q;
        if (q >= d.end || !digit(*q)) bad(d);
        while (q < d.end && digit(*q)) ++q;
    }
    d.p = q;
    if (integral) {
        int64_t v = 0;
        auto r = std::from_chars(s, q, v);
        if (r.ec == std::errc() && r.ptr == q) {
            lua_pushinteger(d.L, static_cast<lua_Integer>(v));
            return;
        }
        // Past int64: a float, like Lua's own reader.
    }
    double v = 0;
    auto r = std::from_chars(s, q, v);
    if (r.ec != std::errc() || r.ptr != q) {
        d.p = s;
        bad(d);
    }
    lua_pushnumber(d.L, static_cast<lua_Number>(v));
}

void decode_value(Dec& d, int depth);

void decode_array(Dec& d, int depth) {
    lua_State* L = d.L;
    ++d.p;
    lua_createtable(L, 0, 0);
    skip_ws(d);
    if (d.p < d.end && *d.p == ']') {
        ++d.p;
        return;
    }
    for (lua_Integer i = 1;; ++i) {
        decode_value(d, depth);
        lua_rawseti(L, -2, i);  // a null leaves a hole
        skip_ws(d);
        if (d.p >= d.end) bad(d);
        if (*d.p == ']') {
            ++d.p;
            return;
        }
        if (*d.p != ',') bad(d);
        ++d.p;
    }
}

void decode_object(Dec& d, int depth) {
    lua_State* L = d.L;
    ++d.p;
    lua_createtable(L, 0, 0);
    skip_ws(d);
    if (d.p < d.end && *d.p == '}') {
        ++d.p;
        return;
    }
    while (true) {
        skip_ws(d);
        if (d.p >= d.end || *d.p != '"') bad(d);
        decode_string(d);
        skip_ws(d);
        if (d.p >= d.end || *d.p != ':') bad(d);
        ++d.p;
        decode_value(d, depth);
        lua_rawset(L, -3);  // a null removes the field
        skip_ws(d);
        if (d.p >= d.end) bad(d);
        if (*d.p == '}') {
            ++d.p;
            return;
        }
        if (*d.p != ',') bad(d);
        ++d.p;
    }
}

void decode_value(Dec& d, int depth) {
    if (++d.count % kCheckEvery == 0 && d.vm != nullptr) d.vm->check_limits(d.L);
    skip_ws(d);
    if (d.p >= d.end) bad(d);
    switch (*d.p) {
        case 'n':
            expect(d, "null", 4);
            lua_pushnil(d.L);
            return;
        case 't':
            expect(d, "true", 4);
            lua_pushboolean(d.L, 1);
            return;
        case 'f':
            expect(d, "false", 5);
            lua_pushboolean(d.L, 0);
            return;
        case '"':
            decode_string(d);
            return;
        case '[':
        case '{':
            if (depth == kLuaJsonMaxDepth)
                luaL_error(d.L, "JSON nested more than %d deep", kLuaJsonMaxDepth);
            luaL_checkstack(d.L, 4, "JSON");
            if (*d.p == '[') {
                decode_array(d, depth + 1);
            } else {
                decode_object(d, depth + 1);
            }
            return;
        default:
            decode_number(d);
    }
}

struct PushCtx {
    const char* data;
    size_t size;
};

void push_entry(lua_State* L, void* ctx) {
    const auto* c = static_cast<const PushCtx*>(ctx);
    decode_json(L, std::string_view(c->data, c->size));
}

}  // namespace

namespace {

const char* to_json(lua_State* L, int idx, os::LargeString& out, size_t max_bytes, bool skip_id) {
    out.clear();
    idx = lua_absindex(L, idx);
    Enc e{L, out, max_bytes, Vm::of(L)};
    e.skip_id = skip_id;
    if (!lua_checkstack(L, kLuaJsonStackSlots))
        return e.vm != nullptr && e.vm->past_limits() ? kJsonLimits : kJsonTooDeep;
    const int top = lua_gettop(L);
    const bool ok = encode(e, idx);
    lua_settop(L, top);
    if (!ok) {
        out.clear();
        return e.err;
    }
    return nullptr;
}

}  // namespace

const char* lua_to_json(lua_State* L, int idx, os::LargeString& out, size_t max_bytes) {
    return to_json(L, idx, out, max_bytes, false);
}

const char* lua_record_to_json(lua_State* L, int idx, os::LargeString& out, size_t max_bytes) {
    return to_json(L, idx, out, max_bytes, true);
}

void decode_json(lua_State* L, std::string_view json) {
    Dec d{L, json.data(), json.data() + json.size(), json.data(), Vm::of(L)};
    decode_value(d, 0);
    skip_ws(d);
    if (d.p != d.end) bad(d);
}

bool push_json(lua_State* L, std::string_view json) {
    if (!lua_checkstack(L, 3)) return false;
    const int top = lua_gettop(L);
    PushCtx ctx{json.data(), json.size()};
    if (protect(L, push_entry, &ctx) != LUA_OK) {
        lua_settop(L, top);
        return false;
    }
    return true;
}

}  // namespace wisp::scene
