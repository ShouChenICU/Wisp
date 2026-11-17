// Where McpClient keeps a server's tool list. On the device an ordinary
// new/malloc under 16 KB lands in internal SRAM (AGENTS.md section 8), so
// the cached tools, the parsed tools/list document and the request and
// response buffers must all go through os::alloc(Mem::Large). The
// simulator's os::alloc is plain malloc, which this binary does not count,
// so every byte the counting operator new below still sees alive after
// list_tools() returns is a byte that would stay in internal RAM.
//
// Only the test thread's allocations are counted: wisp.shttp's transient
// copies of the request are SceneHttp's own business (its header).
//
// This binary replaces the global operator new/delete, so it holds only
// these tests.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#include "support/fake_devices.hpp"
#include "support/fake_io_queue.hpp"
#include "support/fake_mcp_server.hpp"
#include "support/temp_dir.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/mcp_servers.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/mcp_client.hpp"
#include "wisp/net/scene_http.hpp"

namespace {

// Every block carries a header saying whether it was allocated while
// counting, so a delete can tell its own blocks from everyone else's.
struct alignas(std::max_align_t) Header {
    size_t size;
    bool counted;
};

thread_local bool t_counting = false;
std::atomic<long> g_live_bytes{0};

void* counted_new(size_t n) {
    auto* h = static_cast<Header*>(std::malloc(sizeof(Header) + n));
    if (h == nullptr) std::abort();
    h->size = n;
    h->counted = t_counting;
    if (h->counted) g_live_bytes.fetch_add(static_cast<long>(n));
    return h + 1;
}

void counted_delete(void* p) {
    if (p == nullptr) return;
    Header* h = static_cast<Header*>(p) - 1;
    if (h->counted) g_live_bytes.fetch_sub(static_cast<long>(h->size));
    std::free(h);
}

}  // namespace

void* operator new(size_t n) { return counted_new(n); }
void* operator new[](size_t n) { return counted_new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void operator delete(void* p) noexcept { counted_delete(p); }
void operator delete[](void* p) noexcept { counted_delete(p); }
void operator delete(void* p, size_t) noexcept { counted_delete(p); }
void operator delete[](void* p, size_t) noexcept { counted_delete(p); }

namespace {

TEST(McpClientHeap, ToolCacheStaysOffTheDefaultHeap) {
    wisp::log::reset_for_test();
    wisp::log::init();
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    registry.add(std::make_shared<wisp::test::FakeStorage>(
        "storage.int", wisp::test::temp_dir().string(), /*removable=*/false));
    bus.start();
    wisp::test::FakeIoQueue io;
    wisp::core::StorageManager storage(registry, bus);
    ASSERT_TRUE(storage.init());
    wisp::core::McpServerService servers(storage, bus, io.submit_io());
    ASSERT_TRUE(servers.init());
    for (const char* name : {"Warmup", "Files"}) {
        wisp::core::McpServerInput in;
        in.name = name;
        in.url = "http://mcp.local/mcp";
        in.headers = {{"Authorization", std::string("Bearer tok-SECRET-123456")}};
        ASSERT_TRUE(servers.create(in));
    }

    wisp::test::FakeMcpServer fake;
    const std::string description(1024, 'd');
    for (int i = 0; i < 128; ++i)
        fake.tools.push_back(
            wisp::test::FakeMcpServer::tool("tool_" + std::to_string(i), description,
                                            R"({"type":"object","properties":{"path":)"
                                            R"({"type":"string","maxLength":256}}})"));
    wisp::net::SceneHttp http(fake);
    ASSERT_TRUE(http.start());
    {
        wisp::net::McpClient client(http, servers, {.client_version = "1.2.3"});
        // Whatever is made once per process (the log ring's first lines,
        // the client's first state slot) is made here, uncounted.
        ASSERT_EQ(client.list_tools("warmup", {}).failure.error, wisp::net::McpError::None);

        g_live_bytes = 0;
        t_counting = true;
        wisp::net::McpListResult r = client.list_tools("files", {});
        t_counting = false;
        ASSERT_EQ(r.failure.error, wisp::net::McpError::None);
        ASSERT_EQ(r.tools.size(), 128u);
        EXPECT_LE(g_live_bytes.load(), 1024) << "bytes left on the default heap";
    }
    http.stop();
    bus.stop();
}

}  // namespace
