#pragma once
// Absolute path of a committed media test file (tests/data/media/, made by
// tools/gen_media_test_data.sh). The directory comes from the build, so no
// test depends on the working directory.
#include <string>
#include <string_view>

#ifndef WISP_MEDIA_DATA_DIR
#error "WISP_MEDIA_DATA_DIR must be defined by the test's CMake target"
#endif

namespace wisp::test {

inline std::string media_data(std::string_view name) {
    return std::string(WISP_MEDIA_DATA_DIR) + "/" + std::string(name);
}

}  // namespace wisp::test
