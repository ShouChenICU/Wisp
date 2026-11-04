#!/usr/bin/env python3
"""Self-test for tools/check_layering.py: builds small scratch component
trees under a tempdir and asserts the checker both flags each kind of
violation it claims to catch and stays silent on code that doesn't have
one, so a future edit to the checker that silently drops a check (e.g.
its CMake-whitelist regex matching too loosely) fails loudly here
instead of only showing up as a missed violation much later in some
real component.

Run directly: python3 tools/check_layering_test.py
"""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_layering  # noqa: E402


def write(root, rel_path, text):
    path = root / rel_path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def minimal_cmake(name, idf_requires="", host_libs="", requires=""):
    return (
        f"wisp_component(\n"
        f"    NAME {name}\n"
        f"    SRCS src/{name}.cpp\n"
        f"    INCLUDE_DIRS include\n"
        f"    REQUIRES {requires}\n"
        f"    IDF_REQUIRES {idf_requires}\n"
        f"    HOST_LIBS {host_libs}\n"
        f")\n"
    )


class CheckLayeringTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_clean_tree_has_no_errors(self):
        write(self.root, "components/wisp_base/include/wisp/base/x.hpp", "// nothing\n")
        write(
            self.root,
            "components/wisp_hal/include/wisp/hal/y.hpp",
            '#include "wisp/base/x.hpp"\n',
        )
        write(self.root, "components/wisp_base/CMakeLists.txt", minimal_cmake("wisp_base"))
        self.assertEqual(check_layering.run(self.root), [])

    def test_platform_header_is_flagged(self):
        write(
            self.root,
            "components/wisp_core/include/wisp/core/z.hpp",
            '#include "esp_system.h"\n',
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("esp_system.h", errors[0])

    def test_posix_file_headers_are_not_flagged(self):
        # docs/architecture.md: portable layers may use POSIX file APIs
        # (provided by ESP-IDF's VFS on the device, and natively on the
        # host) -- these must stay allowed, unlike the esp_/driver/...
        # platform headers above.
        write(
            self.root,
            "components/wisp_core/src/storage_manager.cpp",
            "#include <unistd.h>\n#include <sys/stat.h>\n#include <dirent.h>\n",
        )
        self.assertEqual(check_layering.run(self.root), [])

    def test_newly_added_forbidden_headers_are_flagged(self):
        for header in ("nvs.h", "nvs_flash.h", "sdmmc_cmd.h", "rom/ets_sys.h", "lwip/sockets.h",
                        "mbedtls/aes.h", "dev_display_lcd.h", "gen_board_device_handles.h"):
            with self.subTest(header=header):
                write(
                    self.root,
                    "components/wisp_hal/include/wisp/hal/probe.hpp",
                    f'#include "{header}"\n',
                )
                errors = check_layering.run(self.root)
                self.assertEqual(len(errors), 1, f"{header}: {errors}")
                (self.root / "components/wisp_hal/include/wisp/hal/probe.hpp").unlink()

    def test_reverse_dependency_is_flagged_for_every_layer_not_just_os_and_hal(self):
        # AGENTS.md section 3's layering order applies to every portable
        # layer, not just wisp_os/wisp_hal -- a wisp_core -> wisp/ui
        # reverse dependency must be flagged exactly like one from
        # wisp_os or wisp_hal would be.
        write(
            self.root,
            "components/wisp_core/include/wisp/core/z.hpp",
            '#include "wisp/ui/ui_runtime.hpp"\n',
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("reverse dependency", errors[0])

    def test_same_or_lower_layer_includes_are_not_reverse_dependencies(self):
        write(
            self.root,
            "components/wisp_app/include/wisp/app/a.hpp",
            '#include "wisp/app/b.hpp"\n#include "wisp/ui/ui_runtime.hpp"\n'
            '#include "wisp/core/event_bus.hpp"\n',
        )
        self.assertEqual(check_layering.run(self.root), [])

    def test_cmake_idf_requires_outside_whitelist_is_flagged(self):
        write(
            self.root,
            "components/wisp_core/CMakeLists.txt",
            minimal_cmake("wisp_core", idf_requires="esp_timer"),
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("esp_timer", errors[0])

    def test_cmake_host_libs_outside_whitelist_is_flagged(self):
        write(
            self.root,
            "components/wisp_core/CMakeLists.txt",
            minimal_cmake("wisp_core", host_libs="SDL2::SDL2"),
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("SDL2::SDL2", errors[0])

    def test_cmake_requires_of_platform_component_is_flagged(self):
        # wisp_component() forwards REQUIRES straight into
        # idf_component_register() on the ESP build, so a platform
        # component slipped in there would bypass the #include-based
        # checks above entirely.
        write(
            self.root,
            "components/wisp_core/CMakeLists.txt",
            minimal_cmake("wisp_core", requires="esp_timer"),
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("esp_timer", errors[0])

    def test_cmake_requires_of_higher_layer_is_flagged(self):
        # Same hazard as the reverse-#include-dependency check above,
        # but via REQUIRES instead of #include.
        write(
            self.root,
            "components/wisp_core/CMakeLists.txt",
            minimal_cmake("wisp_core", requires="wisp_app"),
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("wisp_app", errors[0])

    def test_cmake_requires_of_strictly_lower_layer_is_not_flagged(self):
        write(
            self.root,
            "components/wisp_core/CMakeLists.txt",
            minimal_cmake("wisp_core", requires="wisp_base wisp_os wisp_hal"),
        )
        self.assertEqual(check_layering.run(self.root), [])

    def test_include_with_whitespace_after_hash_is_detected(self):
        # Some formatters/editors can leave "#  include" (extra
        # whitespace between '#' and 'include'); the checker must still
        # recognize it as an #include directive rather than silently
        # skipping the line.
        write(
            self.root,
            "components/wisp_core/include/wisp/core/z.hpp",
            '#   include "esp_system.h"\n',
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("esp_system.h", errors[0])

    def test_wisp_port_include_is_flagged(self):
        # wisp/port/sim/* compiles fine on the host build this checker
        # runs under (it has no platform headers of its own), so only a
        # firmware build -- which ci.sh never does -- would actually
        # fail; the #include itself must be forbidden here instead.
        write(
            self.root,
            "components/wisp_core/src/z.cpp",
            '#include "wisp/port/esp/esp_display.hpp"\n',
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("wisp/port/esp/esp_display.hpp", errors[0])

    def test_curl_include_is_flagged(self):
        # libcurl is the simulator's HTTP client backend only; wisp_net's
        # portable code must reach it through net::HttpClient instead.
        write(self.root, "components/wisp_net/src/c.cpp", "#include <curl/curl.h>\n")
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("curl/curl.h", errors[0])

    def test_net_sits_between_core_and_ui(self):
        write(
            self.root,
            "components/wisp_net/src/ok.cpp",
            '#include "wisp/core/event_bus.hpp"\n#include "wisp/net/http.hpp"\n',
        )
        write(self.root, "components/wisp_ui/src/ok.cpp", '#include "wisp/net/http.hpp"\n')
        self.assertEqual(check_layering.run(self.root), [])

        write(self.root, "components/wisp_core/src/bad.cpp", '#include "wisp/net/http.hpp"\n')
        write(self.root, "components/wisp_net/src/bad.cpp", '#include "wisp/ui/ui_runtime.hpp"\n')
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 2, errors)
        self.assertTrue(all("reverse dependency" in e for e in errors), errors)

    def test_admin_sits_between_net_and_ui(self):
        write(
            self.root,
            "components/wisp_admin/src/ok.cpp",
            '#include "wisp/net/http.hpp"\n#include "wisp/admin/admin_server.hpp"\n',
        )
        write(self.root, "components/wisp_ui/src/ok.cpp", '#include "wisp/admin/admin_server.hpp"\n')
        write(
            self.root,
            "components/wisp_admin/CMakeLists.txt",
            minimal_cmake("wisp_admin", requires="wisp_base wisp_core wisp_net"),
        )
        self.assertEqual(check_layering.run(self.root), [])

        write(self.root, "components/wisp_net/src/bad.cpp", '#include "wisp/admin/admin_server.hpp"\n')
        write(self.root, "components/wisp_admin/src/bad.cpp", '#include "wisp/ui/ui_runtime.hpp"\n')
        write(
            self.root,
            "components/wisp_admin/CMakeLists.txt",
            minimal_cmake("wisp_admin", requires="wisp_net wisp_ui"),
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 3, errors)
        self.assertEqual(sum("reverse dependency" in e for e in errors), 2, errors)
        self.assertEqual(sum("forbidden REQUIRES <wisp_ui>" in e for e in errors), 1, errors)

    def test_admin_platform_include_is_flagged(self):
        write(self.root, "components/wisp_admin/src/p.cpp", '#include "esp_http_server.h"\n')
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("esp_http_server.h", errors[0])

    def test_scene_sits_between_ui_and_app(self):
        write(self.root, "components/wisp_scene/src/ok.cpp", '#include "wisp/ui/x.hpp"\n')
        write(self.root, "components/wisp_app/src/ok.cpp", '#include "wisp/scene/x.hpp"\n')
        self.assertEqual(check_layering.run(self.root), [])

        write(self.root, "components/wisp_ui/src/bad.cpp", '#include "wisp/scene/x.hpp"\n')
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("reverse dependency", errors[0])
        self.assertIn("wisp/scene/x.hpp", errors[0])

    def test_lua_header_outside_scene_is_flagged(self):
        write(
            self.root,
            "components/wisp_scene/src/ok.cpp",
            '#include "lua.h"\n#include "lauxlib.h"\n#include "lualib.h"\n',
        )
        self.assertEqual(check_layering.run(self.root), [])

        write(self.root, "components/wisp_ui/src/bad.cpp", '#include "lua.h"\n')
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("lua.h", errors[0])

    def test_cmake_requires_lua_only_for_scene(self):
        write(
            self.root,
            "components/wisp_scene/CMakeLists.txt",
            minimal_cmake("wisp_scene", requires="wisp_base wisp_ui lua"),
        )
        self.assertEqual(check_layering.run(self.root), [])

        write(
            self.root,
            "components/wisp_ui/CMakeLists.txt",
            minimal_cmake("wisp_ui", requires="wisp_base lua"),
        )
        errors = check_layering.run(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("forbidden REQUIRES <lua>", errors[0])

    def test_cmake_whitelisted_dependencies_are_not_flagged(self):
        write(
            self.root,
            "components/wisp_base/CMakeLists.txt",
            minimal_cmake("wisp_base", idf_requires="bblanchon__arduinojson",
                          host_libs="ArduinoJson"),
        )
        write(
            self.root,
            "components/wisp_ui/CMakeLists.txt",
            minimal_cmake("wisp_ui", idf_requires="lvgl__lvgl", host_libs="lvgl"),
        )
        self.assertEqual(check_layering.run(self.root), [])


if __name__ == "__main__":
    unittest.main()
