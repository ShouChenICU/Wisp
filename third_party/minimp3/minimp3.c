/*
 * Wisp's own file: compiles the implementation of the vendored minimp3
 * (minimp3-1428237/, brought in verbatim by tools/vendor_minimp3.sh)
 * exactly once, including the minimp3_ex.h layer (seekable decoder with
 * ID3, Xing/Info and VBRI parsing). Never edit anything under
 * minimp3-1428237/; build-time adjustments belong here or in
 * CMakeLists.txt.
 *
 * Built as C, as upstream is written and tested.
 */
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
