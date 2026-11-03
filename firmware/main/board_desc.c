// The board descriptor: which board this image was built for, placed by
// the linker right after esp_app_desc_t (section .rodata_custom_desc), so
// it sits at a fixed offset (288) from the start of every app image. The
// firmware upload reads it from an uploaded image's first bytes and
// refuses an image for another board before writing anything
// (connectivity spec 6.5; core::parse_firmware_head(); layout in
// docs/porting-guide.md). WISP_BOARD_NAME comes from firmware/main's
// CMakeLists.txt (the WISP_BOARD cache variable).

#include <stdint.h>

#include "wisp_board_desc.h"

_Static_assert(sizeof(WISP_BOARD_NAME) <= WISP_BOARD_NAME_BYTES, "board name too long");
_Static_assert(sizeof(wisp_board_desc_t) == 48, "board descriptor layout changed");

__attribute__((section(".rodata_custom_desc"), used)) const wisp_board_desc_t wisp_board_desc = {
    .magic = WISP_BOARD_DESC_MAGIC,
    .version = WISP_BOARD_DESC_VERSION,
    .reserved = 0,
    .board = WISP_BOARD_NAME,
};
