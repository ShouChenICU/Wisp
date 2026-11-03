#pragma once

// Layout of the board descriptor (board_desc.c). Must match
// components/wisp_core/include/wisp/core/firmware_image.hpp
// (kBoardDesc*), which reads it from uploaded images.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WISP_BOARD_DESC_MAGIC 0x42505357u  // "WSPB" little-endian
#define WISP_BOARD_DESC_VERSION 1u
#define WISP_BOARD_NAME_BYTES 40

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char board[WISP_BOARD_NAME_BYTES];  // NUL-terminated
} wisp_board_desc_t;

extern const wisp_board_desc_t wisp_board_desc;

#ifdef __cplusplus
}
#endif
