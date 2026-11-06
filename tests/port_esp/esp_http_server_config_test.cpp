// Unit test for EspHttpServerConfig's default (see
// wisp/port/esp/esp_http_server.hpp). Compiles on the host: the struct
// and the net::HttpServer base it declares pull in no ESP-IDF include,
// and this test never instantiates EspHttpServer itself.

#include <gtest/gtest.h>

#include "wisp/port/esp/esp_http_server.hpp"

// Only wisp.fs (core::StorageWorker) opts into flash access; every httpd
// instance -- admin backend and captive redirect alike -- relies on this
// default (AGENTS.md section 5).
TEST(EspHttpServerConfig, DefaultsToNoFlashAccess) {
    EXPECT_FALSE(wisp::port::esp::EspHttpServerConfig{}.flash_access);
    EXPECT_FALSE(wisp::port::esp::EspHttpServerConfig{}.hot_stack);
}
