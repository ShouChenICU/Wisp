/*
 * Wisp's own file: compiles the implementation of the vendored
 * stb_image_write.h (stb-aeb6907/, v1.16, brought in verbatim by
 * tools/vendor_stb.sh) exactly once. Never edit anything under
 * stb-aeb6907/; build-time adjustments belong here or in CMakeLists.txt.
 *
 * Built as C, as upstream is written and tested: the single-header
 * implementation leans on implicit void* conversions and other C idioms
 * that a C++ build would warn about. Its declarations are extern "C"
 * under C++, so wisp_base's C++ wrapper (wisp::encode_jpeg()) links
 * against this object directly.
 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
