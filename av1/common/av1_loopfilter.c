/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <math.h>

#include "./aom_config.h"
#include "./aom_dsp_rtcd.h"
#include "aom_dsp/aom_dsp_common.h"
#include "aom_mem/aom_mem.h"
#include "aom_ports/mem.h"
#include "av1/common/av1_loopfilter.h"
#include "av1/common/onyxc_int.h"
#include "av1/common/reconinter.h"
#include "av1/common/seg_common.h"

static const SEG_LVL_FEATURES seg_lvl_lf_lut[MAX_MB_PLANE][2] = {
  { SEG_LVL_ALT_LF_Y_V, SEG_LVL_ALT_LF_Y_H },
  { SEG_LVL_ALT_LF_U, SEG_LVL_ALT_LF_U },
  { SEG_LVL_ALT_LF_V, SEG_LVL_ALT_LF_V }
};

static const int delta_lf_id_lut[MAX_MB_PLANE][2] = {
  { 0, 1 }, { 2, 2 }, { 3, 3 }
};

typedef enum EDGE_DIR { VERT_EDGE = 0, HORZ_EDGE = 1, NUM_EDGE_DIRS } EDGE_DIR;

// 64 bit masks for left transform size. Each 1 represents a position where
// we should apply a loop filter across the left border of an 8x8 block
// boundary.
//
// In the case of TX_16X16->  ( in low order byte first we end up with
// a mask that looks like this
//
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
//
// A loopfilter should be applied to every other 8x8 horizontally.
static const uint64_t left_64x64_txform_mask[TX_SIZES] = {
  0xffffffffffffffffULL,  // TX_4X4
  0xffffffffffffffffULL,  // TX_8x8
  0x5555555555555555ULL,  // TX_16x16
  0x1111111111111111ULL,  // TX_32x32
  0x0101010101010101ULL,  // TX_64x64
};

// 64 bit masks for above transform size. Each 1 represents a position where
// we should apply a loop filter across the top border of an 8x8 block
// boundary.
//
// In the case of TX_32x32 ->  ( in low order byte first we end up with
// a mask that looks like this
//
//    11111111
//    00000000
//    00000000
//    00000000
//    11111111
//    00000000
//    00000000
//    00000000
//
// A loopfilter should be applied to every other 4 the row vertically.
static const uint64_t above_64x64_txform_mask[TX_SIZES] = {
  0xffffffffffffffffULL,  // TX_4X4
  0xffffffffffffffffULL,  // TX_8x8
  0x00ff00ff00ff00ffULL,  // TX_16x16
  0x000000ff000000ffULL,  // TX_32x32
  0x00000000000000ffULL,  // TX_64x64
};

// 64 bit masks for prediction sizes (left). Each 1 represents a position
// where left border of an 8x8 block. These are aligned to the right most
// appropriate bit, and then shifted into place.
//
// In the case of TX_16x32 ->  ( low order byte first ) we end up with
// a mask that looks like this :
//
//  10000000
//  10000000
//  10000000
//  10000000
//  00000000
//  00000000
//  00000000
//  00000000
static const uint64_t left_prediction_mask[BLOCK_SIZES_ALL] = {
  0x0000000000000001ULL,  // BLOCK_4X4,
  0x0000000000000001ULL,  // BLOCK_4X8,
  0x0000000000000001ULL,  // BLOCK_8X4,
  0x0000000000000001ULL,  // BLOCK_8X8,
  0x0000000000000101ULL,  // BLOCK_8X16,
  0x0000000000000001ULL,  // BLOCK_16X8,
  0x0000000000000101ULL,  // BLOCK_16X16,
  0x0000000001010101ULL,  // BLOCK_16X32,
  0x0000000000000101ULL,  // BLOCK_32X16,
  0x0000000001010101ULL,  // BLOCK_32X32,
  0x0101010101010101ULL,  // BLOCK_32X64,
  0x0000000001010101ULL,  // BLOCK_64X32,
  0x0101010101010101ULL,  // BLOCK_64X64,
  0x0000000000000101ULL,  // BLOCK_4X16,
  0x0000000000000001ULL,  // BLOCK_16X4,
  0x0000000001010101ULL,  // BLOCK_8X32,
  0x0000000000000001ULL,  // BLOCK_32X8,
  0x0101010101010101ULL,  // BLOCK_16X64,
  0x0000000000000101ULL,  // BLOCK_64X16
};

// 64 bit mask to shift and set for each prediction size.
static const uint64_t above_prediction_mask[BLOCK_SIZES_ALL] = {
  0x0000000000000001ULL,  // BLOCK_4X4
  0x0000000000000001ULL,  // BLOCK_4X8
  0x0000000000000001ULL,  // BLOCK_8X4
  0x0000000000000001ULL,  // BLOCK_8X8
  0x0000000000000001ULL,  // BLOCK_8X16,
  0x0000000000000003ULL,  // BLOCK_16X8
  0x0000000000000003ULL,  // BLOCK_16X16
  0x0000000000000003ULL,  // BLOCK_16X32,
  0x000000000000000fULL,  // BLOCK_32X16,
  0x000000000000000fULL,  // BLOCK_32X32,
  0x000000000000000fULL,  // BLOCK_32X64,
  0x00000000000000ffULL,  // BLOCK_64X32,
  0x00000000000000ffULL,  // BLOCK_64X64,
  0x0000000000000001ULL,  // BLOCK_4X16,
  0x0000000000000003ULL,  // BLOCK_16X4,
  0x0000000000000001ULL,  // BLOCK_8X32,
  0x000000000000000fULL,  // BLOCK_32X8,
  0x0000000000000003ULL,  // BLOCK_16X64,
  0x00000000000000ffULL,  // BLOCK_64X16
};
// 64 bit mask to shift and set for each prediction size. A bit is set for
// each 8x8 block that would be in the top left most block of the given block
// size in the 64x64 block.
static const uint64_t size_mask[BLOCK_SIZES_ALL] = {
  0x0000000000000001ULL,  // BLOCK_4X4
  0x0000000000000001ULL,  // BLOCK_4X8
  0x0000000000000001ULL,  // BLOCK_8X4
  0x0000000000000001ULL,  // BLOCK_8X8
  0x0000000000000101ULL,  // BLOCK_8X16,
  0x0000000000000003ULL,  // BLOCK_16X8
  0x0000000000000303ULL,  // BLOCK_16X16
  0x0000000003030303ULL,  // BLOCK_16X32,
  0x0000000000000f0fULL,  // BLOCK_32X16,
  0x000000000f0f0f0fULL,  // BLOCK_32X32,
  0x0f0f0f0f0f0f0f0fULL,  // BLOCK_32X64,
  0x00000000ffffffffULL,  // BLOCK_64X32,
  0xffffffffffffffffULL,  // BLOCK_64X64,
  0x0000000000000101ULL,  // BLOCK_4X16,
  0x0000000000000003ULL,  // BLOCK_16X4,
  0x0000000001010101ULL,  // BLOCK_8X32,
  0x000000000000000fULL,  // BLOCK_32X8,
  0x0303030303030303ULL,  // BLOCK_16X64,
  0x000000000000ffffULL,  // BLOCK_64X16
};

// These are used for masking the left and above 32x32 borders.
static const uint64_t left_border = 0x1111111111111111ULL;
static const uint64_t above_border = 0x000000ff000000ffULL;

// 16 bit masks for uv transform sizes.
static const uint16_t left_64x64_txform_mask_uv[TX_SIZES] = {
  0xffff,  // TX_4X4
  0xffff,  // TX_8x8
  0x5555,  // TX_16x16
  0x1111,  // TX_32x32
  0x0101,  // TX_64x64, never used
};

static const uint16_t above_64x64_txform_mask_uv[TX_SIZES] = {
  0xffff,  // TX_4X4
  0xffff,  // TX_8x8
  0x0f0f,  // TX_16x16
  0x000f,  // TX_32x32
  0x0003,  // TX_64x64, never used
};

// 16 bit left mask to shift and set for each uv prediction size.
static const uint16_t left_prediction_mask_uv[BLOCK_SIZES_ALL] = {
  0x0001,  // BLOCK_4X4,
  0x0001,  // BLOCK_4X8,
  0x0001,  // BLOCK_8X4,
  0x0001,  // BLOCK_8X8,
  0x0001,  // BLOCK_8X16,
  0x0001,  // BLOCK_16X8,
  0x0001,  // BLOCK_16X16,
  0x0011,  // BLOCK_16X32,
  0x0001,  // BLOCK_32X16,
  0x0011,  // BLOCK_32X32,
  0x1111,  // BLOCK_32X64
  0x0011,  // BLOCK_64X32,
  0x1111,  // BLOCK_64X64,
  0x0001,  // BLOCK_4X16,
  0x0001,  // BLOCK_16X4,
  0x0011,  // BLOCK_8X32,
  0x0001,  // BLOCK_32X8,
  0x1111,  // BLOCK_16X64,
  0x0001,  // BLOCK_64X16,
};

// 16 bit above mask to shift and set for uv each prediction size.
static const uint16_t above_prediction_mask_uv[BLOCK_SIZES_ALL] = {
  0x0001,  // BLOCK_4X4
  0x0001,  // BLOCK_4X8
  0x0001,  // BLOCK_8X4
  0x0001,  // BLOCK_8X8
  0x0001,  // BLOCK_8X16,
  0x0001,  // BLOCK_16X8
  0x0001,  // BLOCK_16X16
  0x0001,  // BLOCK_16X32,
  0x0003,  // BLOCK_32X16,
  0x0003,  // BLOCK_32X32,
  0x0003,  // BLOCK_32X64,
  0x000f,  // BLOCK_64X32,
  0x000f,  // BLOCK_64X64,
  0x0001,  // BLOCK_4X16,
  0x0001,  // BLOCK_16X4,
  0x0001,  // BLOCK_8X32,
  0x0003,  // BLOCK_32X8,
  0x0001,  // BLOCK_16X64,
  0x000f,  // BLOCK_64X16
};

// 64 bit mask to shift and set for each uv prediction size
static const uint16_t size_mask_uv[BLOCK_SIZES_ALL] = {
  0x0001,  // BLOCK_4X4
  0x0001,  // BLOCK_4X8
  0x0001,  // BLOCK_8X4
  0x0001,  // BLOCK_8X8
  0x0001,  // BLOCK_8X16,
  0x0001,  // BLOCK_16X8
  0x0001,  // BLOCK_16X16
  0x0011,  // BLOCK_16X32,
  0x0003,  // BLOCK_32X16,
  0x0033,  // BLOCK_32X32,
  0x3333,  // BLOCK_32X64,
  0x00ff,  // BLOCK_64X32,
  0xffff,  // BLOCK_64X64,
  0x0001,  // BLOCK_4X16,
  0x0001,  // BLOCK_16X4,
  0x0011,  // BLOCK_8X32,
  0x0003,  // BLOCK_32X8,
  0x1111,  // BLOCK_16X64,
  0x000f,  // BLOCK_64X16
};
static const uint16_t left_border_uv = 0x1111;
static const uint16_t above_border_uv = 0x000f;

static const int mode_lf_lut[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // INTRA_MODES
  1, 1, 0, 1,                             // INTER_MODES (GLOBALMV == 0)
  1, 1, 1, 1, 1, 1, 0, 1  // INTER_COMPOUND_MODES (GLOBAL_GLOBALMV == 0)
};

#if LOOP_FILTER_BITMASK
// 256 bit masks (64x64 / 4x4) for left transform size for Y plane.
// We use 4 uint64_t to represent the 256 bit.
// Each 1 represents a position where we should apply a loop filter
// across the left border of an 4x4 block boundary.
//
// In the case of TX_8x8->  ( in low order byte first we end up with
// a mask that looks like this (-- and | are used for better view)
//
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    -----------------
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//    10101010|10101010
//
// A loopfilter should be applied to every other 4x4 horizontally.
// TODO(chengchen): make these tables static
const FilterMaskY left_txform_mask[TX_SIZES] = {
  { { 0xffffffffffffffffULL,  // TX_4X4,
      0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL } },

  { { 0x5555555555555555ULL,  // TX_8X8,
      0x5555555555555555ULL, 0x5555555555555555ULL, 0x5555555555555555ULL } },

  { { 0x1111111111111111ULL,  // TX_16X16,
      0x1111111111111111ULL, 0x1111111111111111ULL, 0x1111111111111111ULL } },

  { { 0x0101010101010101ULL,  // TX_32X32,
      0x0101010101010101ULL, 0x0101010101010101ULL, 0x0101010101010101ULL } },

  { { 0x0001000100010001ULL,  // TX_64X64,
      0x0001000100010001ULL, 0x0001000100010001ULL, 0x0001000100010001ULL } },
};

// 256 bit masks (64x64 / 4x4) for above transform size for Y plane.
// We use 4 uint64_t to represent the 256 bit.
// Each 1 represents a position where we should apply a loop filter
// across the top border of an 4x4 block boundary.
//
// In the case of TX_8x8->  ( in low order byte first we end up with
// a mask that looks like this
//
//    11111111|11111111
//    00000000|00000000
//    11111111|11111111
//    00000000|00000000
//    11111111|11111111
//    00000000|00000000
//    11111111|11111111
//    00000000|00000000
//    -----------------
//    11111111|11111111
//    00000000|00000000
//    11111111|11111111
//    00000000|00000000
//    11111111|11111111
//    00000000|00000000
//    11111111|11111111
//    00000000|00000000
//
// A loopfilter should be applied to every other 4x4 horizontally.
const FilterMaskY above_txform_mask[TX_SIZES] = {
  { { 0xffffffffffffffffULL,  // TX_4X4
      0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL } },

  { { 0x0000ffff0000ffffULL,  // TX_8X8
      0x0000ffff0000ffffULL, 0x0000ffff0000ffffULL, 0x0000ffff0000ffffULL } },

  { { 0x000000000000ffffULL,  // TX_16X16
      0x000000000000ffffULL, 0x000000000000ffffULL, 0x000000000000ffffULL } },

  { { 0x000000000000ffffULL,  // TX_32X32
      0x0000000000000000ULL, 0x000000000000ffffULL, 0x0000000000000000ULL } },

  { { 0x000000000000ffffULL,  // TX_64X64
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },
};

// 64 bit mask to shift and set for each prediction size. A bit is set for
// each 4x4 block that would be in the top left most block of the given block
// size in the 64x64 block.
const FilterMaskY size_mask_y[BLOCK_SIZES_ALL] = {
  { { 0x0000000000000001ULL,  // BLOCK_4X4
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x0000000000010001ULL,  // BLOCK_4X8
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x0000000000000003ULL,  // BLOCK_8X4
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x0000000000030003ULL,  // BLOCK_8X8
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x0003000300030003ULL,  // BLOCK_8X16
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x00000000000f000fULL,  // BLOCK_16X8
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x000f000f000f000fULL,  // BLOCK_16X16
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x000f000f000f000fULL,  // BLOCK_16X32
      0x000f000f000f000fULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x00ff00ff00ff00ffULL,  // BLOCK_32X16
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x00ff00ff00ff00ffULL,  // BLOCK_32X32
      0x00ff00ff00ff00ffULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x00ff00ff00ff00ffULL,  // BLOCK_32X64
      0x00ff00ff00ff00ffULL, 0x00ff00ff00ff00ffULL, 0x00ff00ff00ff00ffULL } },

  { { 0xffffffffffffffffULL,  // BLOCK_64X32
      0xffffffffffffffffULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0xffffffffffffffffULL,  // BLOCK_64X64
      0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL } },
  // Y plane max coding block size is 128x128, but the codec divides it
  // into 4 64x64 blocks.
  // BLOCK_64X128
  { { 0x0ULL, 0x0ULL, 0x0ULL, 0x0ULL } },
  // BLOCK_128X64
  { { 0x0ULL, 0x0ULL, 0x0ULL, 0x0ULL } },
  // BLOCK_128X128
  { { 0x0ULL, 0x0ULL, 0x0ULL, 0x0ULL } },

  { { 0x0001000100010001ULL,  // BLOCK_4X16
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x000000000000000fULL,  // BLOCK_16X4
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x0003000300030003ULL,  // BLOCK_8X32
      0x0003000300030003ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x0000000000ff00ffULL,  // BLOCK_32X8
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } },

  { { 0x000f000f000f000fULL,  // BLOCK_16X64
      0x000f000f000f000fULL, 0x000f000f000f000fULL, 0x000f000f000f000fULL } },

  { { 0xffffffffffffffffULL,  // BLOCK_64X16
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL } }
};

// U/V plane max transform size is 32x32 (format 420).
// 64 bit masks (32x32 / 4x4) for left transform size for U/V plane.
// We use one uint64_t to represent the 64 bit.
// Each 1 represents a position where we should apply a loop filter
// across the left border of an 4x4 block boundary.
//
// In the case of TX_8x8->  ( in low order byte first we end up with
// a mask that looks like this
//
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
//    10101010
const FilterMaskUV left_txform_mask_uv[TX_SIZES - 1] = {
  0xffffffffffffffffULL,  // TX_4X4
  0x5555555555555555ULL,  // TX_8X8
  0x1111111111111111ULL,  // TX_16X16
  0x0101010101010101ULL,  // TX_32X32
};

// 64 bit masks (32x32 / 4x4) for above transform size for U/V plane.
// We use one uint64_t to represent the 64 bit.
// Each 1 represents a position where we should apply a loop filter
// across the top border of an 4x4 block boundary.
//
// In the case of TX_8x8->  ( in low order byte first we end up with
// a mask that looks like this
//
//    11111111
//    00000000
//    11111111
//    00000000
//    11111111
//    00000000
//    11111111
//    00000000
const FilterMaskUV above_txform_mask_uv[TX_SIZES - 1] = {
  0xffffffffffffffffULL,  // TX_4X4
  0x00ff00ff00ff00ffULL,  // TX_8X8
  0x000000ff000000ffULL,  // TX_16X16
  0x00000000000000ffULL,  // TX_32X32
};

// Y plane max coding block size is 128x128, but the codec divides it
// into 4 64x64 blocks. U/V plane follows the pattern and size is
// halved accordingly (format 420).
const FilterMaskUV size_mask_u_v[BLOCK_SIZES_ALL] = {
  0x0000000000000001ULL,  // BLOCK_4X4
  0x0000000000000101ULL,  // BLOCK_4X8
  0x0000000000000003ULL,  // BLOCK_8X4
  0x0000000000000303ULL,  // BLOCK_8X8
  0x0000000003030303ULL,  // BLOCK_8X16,
  0x0000000000000f0fULL,  // BLOCK_16X8
  0x000000000f0f0f0fULL,  // BLOCK_16X16
  0x0f0f0f0f0f0f0f0fULL,  // BLOCK_16X32,
  0x00000000ffffffffULL,  // BLOCK_32X16,
  0xffffffffffffffffULL,  // BLOCK_32X32,
  0xffffffffffffffffULL,  // BLOCK_32X64,
  0xffffffffffffffffULL,  // BLOCK_64X32,
  0xffffffffffffffffULL,  // BLOCK_64X64,
  0xffffffffffffffffULL,  // BLOCK_64X128,
  0xffffffffffffffffULL,  // BLOCK_128X64,
  0xffffffffffffffffULL,  // BLOCK_128X128,
  0x0000000001010101ULL,  // BLOCK_4X16,
  0x000000000000000fULL,  // BLOCK_16X4,
  0x0303030303030303ULL,  // BLOCK_8X32,
  0x000000000000ffffULL,  // BLOCK_32X8,
  0x0f0f0f0f0f0f0f0fULL,  // BLOCK_16X64,
  0x00000000ffffffffULL,  // BLOCK_64X16
};

static LoopFilterMask *get_loop_filter_mask(AV1_COMMON *const cm, int mi_row,
                                            int mi_col) {
  assert(cm->lf.lfm != NULL);
  const int sb_row = mi_row >> MAX_MIB_SIZE_LOG2;
  const int sb_col = mi_col >> MAX_MIB_SIZE_LOG2;
  return &cm->lf.lfm[sb_row * cm->lf.lfm_stride + sb_col];
}
#endif  // LOOP_FILTER_BITMASK

static void update_sharpness(loop_filter_info_n *lfi, int sharpness_lvl) {
  int lvl;

  // For each possible value for the loop filter fill out limits
  for (lvl = 0; lvl <= MAX_LOOP_FILTER; lvl++) {
    // Set loop filter parameters that control sharpness.
    int block_inside_limit = lvl >> ((sharpness_lvl > 0) + (sharpness_lvl > 4));

    if (sharpness_lvl > 0) {
      if (block_inside_limit > (9 - sharpness_lvl))
        block_inside_limit = (9 - sharpness_lvl);
    }

    if (block_inside_limit < 1) block_inside_limit = 1;

    memset(lfi->lfthr[lvl].lim, block_inside_limit, SIMD_WIDTH);
    memset(lfi->lfthr[lvl].mblim, (2 * (lvl + 2) + block_inside_limit),
           SIMD_WIDTH);
  }
}
static uint8_t get_filter_level(const AV1_COMMON *cm,
                                const loop_filter_info_n *lfi_n,
                                const int dir_idx, int plane,
                                const MB_MODE_INFO *mbmi) {
  const int segment_id = mbmi->segment_id;
  if (cm->delta_lf_present_flag) {
    int delta_lf;
    if (cm->delta_lf_multi) {
      const int delta_lf_idx = delta_lf_id_lut[plane][dir_idx];
      delta_lf = mbmi->curr_delta_lf[delta_lf_idx];
    } else {
      delta_lf = mbmi->current_delta_lf_from_base;
    }
    int base_level;
    if (plane == 0)
      base_level = cm->lf.filter_level[dir_idx];
    else if (plane == 1)
      base_level = cm->lf.filter_level_u;
    else
      base_level = cm->lf.filter_level_v;
    int lvl_seg = clamp(delta_lf + base_level, 0, MAX_LOOP_FILTER);
    assert(plane >= 0 && plane <= 2);
    const int seg_lf_feature_id = seg_lvl_lf_lut[plane][dir_idx];
    if (segfeature_active(&cm->seg, segment_id, seg_lf_feature_id)) {
      const int data = get_segdata(&cm->seg, segment_id, seg_lf_feature_id);
      lvl_seg = clamp(lvl_seg + data, 0, MAX_LOOP_FILTER);
    }

    if (cm->lf.mode_ref_delta_enabled) {
      const int scale = 1 << (lvl_seg >> 5);
      lvl_seg += cm->lf.ref_deltas[mbmi->ref_frame[0]] * scale;
      if (mbmi->ref_frame[0] > INTRA_FRAME)
        lvl_seg += cm->lf.mode_deltas[mode_lf_lut[mbmi->mode]] * scale;
      lvl_seg = clamp(lvl_seg, 0, MAX_LOOP_FILTER);
    }
    return lvl_seg;
  } else {
    return lfi_n
        ->lvl[segment_id][dir_idx][mbmi->ref_frame[0]][mode_lf_lut[mbmi->mode]];
  }
}

void av1_loop_filter_init(AV1_COMMON *cm) {
  assert(MB_MODE_COUNT == NELEMENTS(mode_lf_lut));
  loop_filter_info_n *lfi = &cm->lf_info;
  struct loopfilter *lf = &cm->lf;
  int lvl;

  // init limits for given sharpness
  update_sharpness(lfi, lf->sharpness_level);

  // init hev threshold const vectors
  for (lvl = 0; lvl <= MAX_LOOP_FILTER; lvl++)
    memset(lfi->lfthr[lvl].hev_thr, (lvl >> 4), SIMD_WIDTH);
}

void av1_loop_filter_frame_init(AV1_COMMON *cm, int default_filt_lvl,
                                int default_filt_lvl_r, int plane) {
  int seg_id;
  // n_shift is the multiplier for lf_deltas
  // the multiplier is 1 for when filter_lvl is between 0 and 31;
  // 2 when filter_lvl is between 32 and 63
  loop_filter_info_n *const lfi = &cm->lf_info;
  struct loopfilter *const lf = &cm->lf;
  const struct segmentation *const seg = &cm->seg;

  // update sharpness limits
  update_sharpness(lfi, lf->sharpness_level);

  for (seg_id = 0; seg_id < MAX_SEGMENTS; seg_id++) {
    for (int dir = 0; dir < 2; ++dir) {
      int lvl_seg = (dir == 0) ? default_filt_lvl : default_filt_lvl_r;
      assert(plane >= 0 && plane <= 2);
      const int seg_lf_feature_id = seg_lvl_lf_lut[plane][dir];
      if (segfeature_active(seg, seg_id, seg_lf_feature_id)) {
        const int data = get_segdata(&cm->seg, seg_id, seg_lf_feature_id);
        lvl_seg = clamp(lvl_seg + data, 0, MAX_LOOP_FILTER);
      }

      if (!lf->mode_ref_delta_enabled) {
        // we could get rid of this if we assume that deltas are set to
        // zero when not in use; encoder always uses deltas
        memset(lfi->lvl[seg_id][dir], lvl_seg, sizeof(lfi->lvl[seg_id][dir]));
      } else {
        int ref, mode;
        const int scale = 1 << (lvl_seg >> 5);
        const int intra_lvl = lvl_seg + lf->ref_deltas[INTRA_FRAME] * scale;
        lfi->lvl[seg_id][dir][INTRA_FRAME][0] =
            clamp(intra_lvl, 0, MAX_LOOP_FILTER);

        for (ref = LAST_FRAME; ref < REF_FRAMES; ++ref) {
          for (mode = 0; mode < MAX_MODE_LF_DELTAS; ++mode) {
            const int inter_lvl = lvl_seg + lf->ref_deltas[ref] * scale +
                                  lf->mode_deltas[mode] * scale;
            lfi->lvl[seg_id][dir][ref][mode] =
                clamp(inter_lvl, 0, MAX_LOOP_FILTER);
          }
        }
      }
    }
  }

#if LOOP_FILTER_BITMASK
  memset(lf->neighbor_sb_lpf_info.tx_size_y_above, TX_64X64,
         sizeof(TX_SIZE) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.tx_size_y_left, TX_64X64,
         sizeof(TX_SIZE) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.tx_size_uv_above, TX_64X64,
         sizeof(TX_SIZE) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.tx_size_uv_left, TX_64X64,
         sizeof(TX_SIZE) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.y_level_above, 0,
         sizeof(uint8_t) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.y_level_left, 0,
         sizeof(uint8_t) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.u_level_above, 0,
         sizeof(uint8_t) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.u_level_left, 0,
         sizeof(uint8_t) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.v_level_above, 0,
         sizeof(uint8_t) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.v_level_left, 0,
         sizeof(uint8_t) * MI_SIZE_64X64);
  memset(lf->neighbor_sb_lpf_info.skip, 0, sizeof(uint8_t) * MI_SIZE_64X64);
#endif  // LOOP_FILTER_BITMASK
}

#if LOOP_FILTER_BITMASK
// A 64x64 tx block requires 256 bits to represent each 4x4 tx block.
// Every 4 rows is represented by one uint64_t mask. Hence,
// there are 4 uint64_t bitmask[4] to represent the 64x64 block.
//
// Given a location by (idx, idy), This function returns the index
// 0, 1, 2, 3 to select which bitmask[] to use.
// Then the pointer y_shift contains the shift value in the bit mask.
// Function returns y_shift; y_index contains the index.
//
// For example, idy is the offset of pixels,
// (idy >> MI_SIZE_LOG2) converts to 4x4 unit.
// ((idy >> MI_SIZE_LOG2) / 4) returns which uint64_t.
// After locating which uint64_t, (idy >> MI_SIZE_LOG2) % 4 is the
// row offset, and each row has 16 = 1 << stride_log2 4x4 units.
// Therefore, shift = (row << stride_log2) + (idx >> MI_SIZE_LOG2);
static int get_y_index_shift(int idx, int idy, int *y_index) {
  // idy_unit = idy >> MI_SIZE_LOG2;
  // idx_unit = idx >> MI_SIZE_LOG2;
  // *y_index = idy_unit >> 2;
  // rows = idy_unit % 4;
  // stride_log2 = 4;
  // shift = (rows << stride_log2) + idx_unit;

  *y_index = idy >> 4;
  return ((idy & 12) << 2) | (idx >> 2);
}

// Largest tx size of U/V plane is 32x32.
// We need one uint64_t bitmask to present all 4x4 tx block.
// ss_x, ss_y: subsampling. for 420 format, ss_x = 1, ss_y = 1.
// Each row has 8 = (1 << stride_log2) 4x4 units.
static int get_uv_index_shift(int idx, int idy) {
  // stride_log2 = 3;
  // idy_unit = (idy >> (MI_SIZE_LOG2 + ss_y));
  // idx_unit = (idx >> (MI_SIZE_LOG2 + ss_x));
  // shift = (idy_unit << stride_log2) + idx_unit;
  return (idy & ~7) | (idx >> 3);
}

static void check_mask_y(const FilterMaskY *lfm) {
#ifndef NDEBUG
  int i;
  for (i = 0; i < 4; ++i) {
    assert(!(lfm[TX_4X4].bits[i] & lfm[TX_8X8].bits[i]));
    assert(!(lfm[TX_4X4].bits[i] & lfm[TX_16X16].bits[i]));
    assert(!(lfm[TX_4X4].bits[i] & lfm[TX_32X32].bits[i]));
    assert(!(lfm[TX_4X4].bits[i] & lfm[TX_64X64].bits[i]));
    assert(!(lfm[TX_8X8].bits[i] & lfm[TX_16X16].bits[i]));
    assert(!(lfm[TX_8X8].bits[i] & lfm[TX_32X32].bits[i]));
    assert(!(lfm[TX_8X8].bits[i] & lfm[TX_64X64].bits[i]));
    assert(!(lfm[TX_16X16].bits[i] & lfm[TX_32X32].bits[i]));
    assert(!(lfm[TX_16X16].bits[i] & lfm[TX_64X64].bits[i]));
    assert(!(lfm[TX_32X32].bits[i] & lfm[TX_64X64].bits[i]));
  }
#else
  (void)lfm;
#endif
}

static void check_mask_uv(const FilterMaskUV *lfm) {
#ifndef NDEBUG
  int i;
  for (i = 0; i < 4; ++i) {
    assert(!(lfm[TX_4X4] & lfm[TX_8X8]));
    assert(!(lfm[TX_4X4] & lfm[TX_16X16]));
    assert(!(lfm[TX_4X4] & lfm[TX_32X32]));
    assert(!(lfm[TX_8X8] & lfm[TX_16X16]));
    assert(!(lfm[TX_8X8] & lfm[TX_32X32]));
    assert(!(lfm[TX_16X16] & lfm[TX_32X32]));
  }
#else
  (void)lfm;
#endif
}

static void check_loop_filter_masks(const LoopFilterMask *lfm) {
  for (int i = 0; i < LOOP_FILTER_MASK_NUM; ++i) {
    // Assert if we try to apply 2 different loop filters at the same position.
    check_mask_y(lfm->lfm_info[i].left_y);
    check_mask_y(lfm->lfm_info[i].above_y);
    check_mask_uv(lfm->lfm_info[i].left_u);
    check_mask_uv(lfm->lfm_info[i].above_u);
    check_mask_uv(lfm->lfm_info[i].left_v);
    check_mask_uv(lfm->lfm_info[i].above_v);
  }
}

// if superblock size is 128x128, we need to specify which lpf mask info.
int get_mask_idx_inside_sb(AV1_COMMON *const cm, int mi_row, int mi_col) {
  if (cm->seq_params.mib_size == MI_SIZE_64X64) return 0;
  const int r = (mi_row % cm->seq_params.mib_size) >> 4;
  const int c = (mi_col % cm->seq_params.mib_size) >> 4;
  return (r << 1) + c;
}

static void setup_masks(AV1_COMMON *const cm, int mi_row, int mi_col, int plane,
                        int subsampling_x, int subsampling_y, TX_SIZE tx_size,
                        LoopFilterMask *lfm) {
  if (mi_row == 0 && mi_col == 0) return;

  const int idx = mi_col << MI_SIZE_LOG2;
  const int idy = mi_row << MI_SIZE_LOG2;
  MB_MODE_INFO **mi = cm->mi_grid_visible + mi_row * cm->mi_stride + mi_col;
  const MB_MODE_INFO *const mbmi = mi[0];
  const int curr_skip = mbmi->skip && is_inter_block(mbmi);
  int y_index = 0;
  const int shift = plane ? get_uv_index_shift(idx, idy)
                          : get_y_index_shift(idx, idy, &y_index);
  const int mask_idx = get_mask_idx_inside_sb(cm, mi_row, mi_col);
  LoopFilterMaskInfo *const lfm_info = &lfm->lfm_info[mask_idx];

  // decide whether current vertical/horizontal edge needs loop filtering
  EDGE_DIR dir;
  for (dir = VERT_EDGE; dir <= HORZ_EDGE; ++dir) {
    const int row_or_col = dir == VERT_EDGE ? mi_col : mi_row;
    if (row_or_col == 0) continue;  // do not filter frame boundary

    MB_MODE_INFO **mi_prev =
        (dir == VERT_EDGE) ? mi - (tx_size_wide_unit[tx_size] << subsampling_x)
                           : mi - ((tx_size_high_unit[tx_size] * cm->mi_stride)
                                   << subsampling_y);
    const MB_MODE_INFO *const mbmi_prev = mi_prev[0];
    const uint8_t level = get_filter_level(cm, &cm->lf_info, dir, plane, mbmi);
    const uint8_t level_prev =
        get_filter_level(cm, &cm->lf_info, dir, plane, mbmi_prev);
    const int prev_skip = mbmi_prev->skip && is_inter_block(mbmi_prev);
    const BLOCK_SIZE bsize =
        ss_size_lookup[mbmi->sb_type][subsampling_x][subsampling_y];
    const int prediction_masks = dir == VERT_EDGE ? block_size_wide[bsize] - 1
                                                  : block_size_high[bsize] - 1;
    const int is_coding_block_border = !(row_or_col & prediction_masks);
    const int is_edge = (level || level_prev) &&
                        (!curr_skip || !prev_skip || is_coding_block_border);
    if (is_edge) {
      const TX_SIZE prev_tx_size =
          plane ? av1_get_uv_tx_size(mbmi_prev, subsampling_x, subsampling_y)
                : mbmi_prev->tx_size;
      const TX_SIZE min_tx_size =
          (dir == VERT_EDGE)
              ? AOMMIN(txsize_horz_map[tx_size], txsize_horz_map[prev_tx_size])
              : AOMMIN(txsize_vert_map[tx_size], txsize_vert_map[prev_tx_size]);
      assert(min_tx_size < TX_SIZES);

      // set mask on corresponding bit
      if (dir == VERT_EDGE) {
        switch (plane) {
          case 0:
            lfm_info->left_y[min_tx_size].bits[y_index] |= (1 << shift);
            break;
          case 1: lfm_info->left_u[min_tx_size] |= (1 << shift); break;
          case 2: lfm_info->left_v[min_tx_size] |= (1 << shift); break;
          default: assert(plane <= 2);
        }
      } else {
        switch (plane) {
          case 0:
            lfm_info->above_y[min_tx_size].bits[y_index] |= (1 << shift);
            break;
          case 1: lfm_info->above_u[min_tx_size] |= (1 << shift); break;
          case 2: lfm_info->above_v[min_tx_size] |= (1 << shift); break;
          default: assert(plane <= 2);
        }
      }
    }
  }
}

static void setup_tx_block_mask(AV1_COMMON *const cm, int mi_row, int mi_col,
                                int blk_row, int blk_col, int plane_bsize,
                                TX_SIZE tx_size, int plane, int subsampling_x,
                                int subsampling_y, LoopFilterMask *lfm) {
  MB_MODE_INFO **mi = cm->mi_grid_visible + mi_row * cm->mi_stride + mi_col;
  const MB_MODE_INFO *const mbmi = mi[0];
  // For Y plane:
  // If intra block, tx size is univariant.
  // If inter block, tx size follows inter_tx_size.
  // For U/V plane: tx_size is always the largest size.
  TX_SIZE plane_tx_size;
  const int is_inter = is_inter_block(mbmi);
  if (is_inter) {
    plane_tx_size = plane
                        ? av1_get_uv_tx_size(mbmi, subsampling_x, subsampling_y)
                        : mbmi->inter_tx_size[av1_get_txb_size_index(
                              plane_bsize, blk_row, blk_col)];
  } else {
    plane_tx_size = plane
                        ? av1_get_uv_tx_size(mbmi, subsampling_x, subsampling_y)
                        : mbmi->tx_size;
  }

  if (plane) assert(plane_tx_size == tx_size);

  if (plane_tx_size == tx_size) {
    setup_masks(cm, mi_row, mi_col, plane, subsampling_x, subsampling_y,
                tx_size, lfm);
  } else {
    const TX_SIZE sub_txs = sub_tx_size_map[is_inter][tx_size];
    const int bsw = tx_size_wide_unit[sub_txs];
    const int bsh = tx_size_high_unit[sub_txs];
    for (int row = 0; row < tx_size_high_unit[tx_size]; row += bsh) {
      for (int col = 0; col < tx_size_wide_unit[tx_size]; col += bsw) {
        const int offsetr = blk_row + row;
        const int offsetc = blk_col + col;

        if (mi_row + offsetr >= cm->mi_rows || mi_col + offsetc >= cm->mi_cols)
          continue;

        setup_tx_block_mask(cm, mi_row, mi_col, offsetr, offsetc, plane_bsize,
                            sub_txs, plane, subsampling_x, subsampling_y, lfm);
      }
    }
  }
}

static void setup_fix_block_mask(AV1_COMMON *const cm, int mi_row, int mi_col,
                                 int block_width, int block_height, int plane,
                                 int subsampling_x, int subsampling_y,
                                 LoopFilterMask *lfm) {
  MB_MODE_INFO **mi = cm->mi_grid_visible + mi_row * cm->mi_stride + mi_col;
  const MB_MODE_INFO *const mbmi = mi[0];

  const BLOCK_SIZE bsize = mbmi->sb_type;
  const BLOCK_SIZE bsizec =
      scale_chroma_bsize(bsize, subsampling_x, subsampling_y);
  const BLOCK_SIZE plane_bsize =
      ss_size_lookup[bsizec][subsampling_x][subsampling_y];
  TX_SIZE max_txsize = get_max_rect_tx_size(plane_bsize);
  // The decoder is designed so that it can process 64x64 luma pixels at a
  // time. If this is a chroma plane with subsampling and bsize corresponds to
  // a subsampled BLOCK_128X128 then the lookup above will give TX_64X64. That
  // mustn't be used for the subsampled plane (because it would be bigger than
  // a 64x64 luma block) so we round down to TX_32X32.
  if ((subsampling_x || subsampling_y) &&
      txsize_sqr_up_map[max_txsize] == TX_64X64) {
    if (max_txsize == TX_16X64)
      max_txsize = TX_16X32;
    else if (max_txsize == TX_64X16)
      max_txsize = TX_32X16;
    else
      max_txsize = TX_32X32;
  }

  const BLOCK_SIZE txb_size = txsize_to_bsize[max_txsize];
  const int bw = block_size_wide[txb_size] >> tx_size_wide_log2[0];
  const int bh = block_size_high[txb_size] >> tx_size_wide_log2[0];
  const BLOCK_SIZE max_unit_bsize =
      ss_size_lookup[BLOCK_64X64][subsampling_x][subsampling_y];
  int mu_blocks_wide = block_size_wide[max_unit_bsize] >> tx_size_wide_log2[0];
  int mu_blocks_high = block_size_high[max_unit_bsize] >> tx_size_high_log2[0];

  mu_blocks_wide = AOMMIN(block_width, mu_blocks_wide);
  mu_blocks_high = AOMMIN(block_height, mu_blocks_high);

  // Largest tx_size is 64x64, while superblock size can be 128x128.
  // Here we ensure that setup_tx_block_mask process at most a 64x64 block.
  for (int idy = 0; idy < block_height; idy += mu_blocks_high) {
    for (int idx = 0; idx < block_width; idx += mu_blocks_wide) {
      const int unit_height = AOMMIN(mu_blocks_high + idy, block_height);
      const int unit_width = AOMMIN(mu_blocks_wide + idx, block_width);
      for (int blk_row = idy; blk_row < unit_height; blk_row += bh) {
        for (int blk_col = idx; blk_col < unit_width; blk_col += bw) {
          setup_tx_block_mask(cm, mi_row, mi_col, blk_row, blk_col, plane_bsize,
                              max_txsize, plane, subsampling_x, subsampling_y,
                              lfm);
        }
      }
    }
  }
}

static void setup_block_mask(AV1_COMMON *const cm, int mi_row, int mi_col,
                             BLOCK_SIZE bsize, int plane, int subsampling_x,
                             int subsampling_y, LoopFilterMask *lfm) {
  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols) return;

  const PARTITION_TYPE partition = get_partition(cm, mi_row, mi_col, bsize);
  const BLOCK_SIZE subsize = get_subsize(bsize, partition);
  const int hbs = mi_size_wide[bsize] / 2;
  const int quarter_step = mi_size_wide[bsize] / 4;
  const int bw = mi_size_wide[bsize];
  const int bh = mi_size_high[bsize];
  int i;

  switch (partition) {
    case PARTITION_NONE:
      setup_fix_block_mask(cm, mi_row, mi_col, bw, bh, plane, subsampling_x,
                           subsampling_y, lfm);
      break;
    case PARTITION_HORZ:
      setup_fix_block_mask(cm, mi_row, mi_col, bw, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      if (mi_row + hbs < cm->mi_rows)
        setup_fix_block_mask(cm, mi_row + hbs, mi_col, bw, bh >> 1, plane,
                             subsampling_x, subsampling_y, lfm);
      break;
    case PARTITION_VERT:
      setup_fix_block_mask(cm, mi_row, mi_col, bw >> 1, bh, plane,
                           subsampling_x, subsampling_y, lfm);
      if (mi_col + hbs < cm->mi_cols)
        setup_fix_block_mask(cm, mi_row, mi_col + hbs, bw >> 1, bh, plane,
                             subsampling_x, subsampling_y, lfm);
      break;
    case PARTITION_SPLIT:
      setup_block_mask(cm, mi_row, mi_col, subsize, plane, subsampling_x,
                       subsampling_y, lfm);
      setup_block_mask(cm, mi_row, mi_col + hbs, subsize, plane, subsampling_x,
                       subsampling_y, lfm);
      setup_block_mask(cm, mi_row + hbs, mi_col, subsize, plane, subsampling_x,
                       subsampling_y, lfm);
      setup_block_mask(cm, mi_row + hbs, mi_col + hbs, subsize, plane,
                       subsampling_x, subsampling_y, lfm);
      break;
    case PARTITION_HORZ_A:
      setup_fix_block_mask(cm, mi_row, mi_col, bw >> 1, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row, mi_col + hbs, bw >> 1, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row + hbs, mi_col, bw, bh, plane,
                           subsampling_x, subsampling_y, lfm);
      break;
    case PARTITION_HORZ_B:
      setup_fix_block_mask(cm, mi_row, mi_col, bw, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row + hbs, mi_col, bw >> 1, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row + hbs, mi_col + hbs, bw >> 1, bh >> 1,
                           plane, subsampling_x, subsampling_y, lfm);
      break;
    case PARTITION_VERT_A:
      setup_fix_block_mask(cm, mi_row, mi_col, bw >> 1, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row + hbs, mi_col, bw >> 1, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row, mi_col + hbs, bw >> 1, bh, plane,
                           subsampling_x, subsampling_y, lfm);
      break;
    case PARTITION_VERT_B:
      setup_fix_block_mask(cm, mi_row, mi_col, bw >> 1, bh, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row, mi_col + hbs, bw >> 1, bh >> 1, plane,
                           subsampling_x, subsampling_y, lfm);
      setup_fix_block_mask(cm, mi_row + hbs, mi_col + hbs, bw >> 1, bh >> 1,
                           plane, subsampling_x, subsampling_y, lfm);
      break;
    case PARTITION_HORZ_4:
      for (i = 0; i < 4; ++i) {
        int this_mi_row = mi_row + i * quarter_step;
        if (i > 0 && this_mi_row >= cm->mi_rows) break;

        setup_fix_block_mask(cm, this_mi_row, mi_col, bw, bh >> 2, plane,
                             subsampling_x, subsampling_y, lfm);
      }
      break;
    case PARTITION_VERT_4:
      for (i = 0; i < 4; ++i) {
        int this_mi_col = mi_col + i * quarter_step;
        if (i > 0 && this_mi_col >= cm->mi_cols) break;

        setup_fix_block_mask(cm, mi_row, this_mi_col, bw >> 2, bh, plane,
                             subsampling_x, subsampling_y, lfm);
      }
      break;
    default: assert(0);
  }
}

// TODO(chengchen): if lossless, do not need to setup mask. But when
// segments enabled, each segment has different lossless settings.
void av1_setup_bitmask(AV1_COMMON *const cm, int mi_row, int mi_col, int plane,
                       int subsampling_x, int subsampling_y,
                       LoopFilterMask *lfm) {
  assert(lfm != NULL);

  // set up bitmask for each superblock
  setup_block_mask(cm, mi_row, mi_col, cm->seq_params.sb_size, plane,
                   subsampling_x, subsampling_y, lfm);

  // check if the mask is valid
  check_loop_filter_masks(lfm);

  {
    // place hoder: for potential special case handling.
    // 64x64 (Y) or 32x32 (U/V) boundaries must be filtered.
    const int num_64x64 = MAX_MIB_SIZE == MI_SIZE_64X64 ? 1 : 4;
    if (plane == 0) {
      for (int i = 0; i < num_64x64; ++i) {
        for (int j = 0; j < 4; ++j) {
          if (mi_col || i & 1)
            lfm->lfm_info[i].left_y[TX_64X64].bits[j] |=
                left_txform_mask[TX_64X64].bits[j];
          if (mi_row || i > 1)
            lfm->lfm_info[i].above_y[TX_64X64].bits[j] |=
                above_txform_mask[TX_64X64].bits[j];
        }
      }
    } else {
      for (int i = 0; i < num_64x64; ++i) {
        if (mi_col || i & 1) {
          lfm->lfm_info[i].left_u[TX_32X32] |= left_txform_mask_uv[TX_32X32];
          lfm->lfm_info[i].left_v[TX_32X32] |= left_txform_mask_uv[TX_32X32];
        }
        if (mi_row || i > 1) {
          lfm->lfm_info[i].above_u[TX_32X32] |= above_txform_mask_uv[TX_32X32];
          lfm->lfm_info[i].above_v[TX_32X32] |= above_txform_mask_uv[TX_32X32];
        }
      }
    }

    // Let 16x16 hold 32x32 (Y/U/V) and 64x64(Y only).
    // Even tx size is greater, we only apply max length filter, which is 16.
    for (int i = 0; i < LOOP_FILTER_MASK_NUM; ++i) {
      if (plane == 0) {
        for (int j = 0; j < 4; ++j) {
          lfm->lfm_info[i].left_y[TX_16X16].bits[j] |=
              lfm->lfm_info[i].left_y[TX_32X32].bits[j];
          lfm->lfm_info[i].left_y[TX_16X16].bits[j] |=
              lfm->lfm_info[i].left_y[TX_64X64].bits[j];
          lfm->lfm_info[i].above_y[TX_16X16].bits[j] |=
              lfm->lfm_info[i].above_y[TX_32X32].bits[j];
          lfm->lfm_info[i].above_y[TX_16X16].bits[j] |=
              lfm->lfm_info[i].above_y[TX_64X64].bits[j];
        }
      } else if (plane == 1) {
        lfm->lfm_info[i].left_u[TX_16X16] |= lfm->lfm_info[i].left_u[TX_32X32];
        lfm->lfm_info[i].above_u[TX_16X16] |=
            lfm->lfm_info[i].above_u[TX_32X32];
      } else {
        lfm->lfm_info[i].left_v[TX_16X16] |= lfm->lfm_info[i].left_v[TX_32X32];
        lfm->lfm_info[i].above_v[TX_16X16] |=
            lfm->lfm_info[i].above_v[TX_32X32];
      }
    }
  }
}
#endif  // LOOP_FILTER_BITMASK

// This function ors into the current lfm structure, where to do loop
// filters for the specific mi we are looking at. It uses information
// including the block_size_type (32x16, 32x32, etc.), the transform size,
// whether there were any coefficients encoded, and the loop filter strength
// block we are currently looking at. Shift is used to position the
// 1's we produce.
// TODO(JBB) Need another function for different resolution color..
static void build_masks(AV1_COMMON *const cm,
                        const loop_filter_info_n *const lfi_n,
                        const MB_MODE_INFO *mbmi, const int shift_y,
                        const int shift_uv, LOOP_FILTER_MASK *lfm) {
  const BLOCK_SIZE block_size = mbmi->sb_type;
  // TODO(debargha): Check if masks can be setup correctly when
  // rectangular transfroms are used with the EXT_TX expt.
  const TX_SIZE tx_size_y = txsize_sqr_map[mbmi->tx_size];
  const TX_SIZE tx_size_y_left = txsize_horz_map[mbmi->tx_size];
  const TX_SIZE tx_size_y_above = txsize_vert_map[mbmi->tx_size];
  const TX_SIZE tx_size_uv_actual = av1_get_uv_tx_size(mbmi, 1, 1);
  const TX_SIZE tx_size_uv = txsize_sqr_map[tx_size_uv_actual];
  const TX_SIZE tx_size_uv_left = txsize_horz_map[tx_size_uv_actual];
  const TX_SIZE tx_size_uv_above = txsize_vert_map[tx_size_uv_actual];
  const int filter_level = get_filter_level(cm, lfi_n, 0, 0, mbmi);
  uint64_t *const left_y = &lfm->left_y[tx_size_y_left];
  uint64_t *const above_y = &lfm->above_y[tx_size_y_above];
  uint64_t *const int_4x4_y = &lfm->int_4x4_y;
  uint16_t *const left_uv = &lfm->left_uv[tx_size_uv_left];
  uint16_t *const above_uv = &lfm->above_uv[tx_size_uv_above];
  uint16_t *const int_4x4_uv = &lfm->left_int_4x4_uv;
  int i;

  // If filter level is 0 we don't loop filter.
  if (!filter_level) {
    return;
  } else {
    const int w = num_8x8_blocks_wide_lookup[block_size];
    const int h = num_8x8_blocks_high_lookup[block_size];
    const int row = (shift_y >> MAX_MIB_SIZE_LOG2);
    const int col = shift_y - (row << MAX_MIB_SIZE_LOG2);

    for (i = 0; i < h; i++) memset(&lfm->lfl_y[row + i][col], filter_level, w);
  }

  // These set 1 in the current block size for the block size edges.
  // For instance if the block size is 32x16, we'll set:
  //    above =   1111
  //              0000
  //    and
  //    left  =   1000
  //          =   1000
  // NOTE : In this example the low bit is left most ( 1000 ) is stored as
  //        1,  not 8...
  //
  // U and V set things on a 16 bit scale.
  //
  *above_y |= above_prediction_mask[block_size] << shift_y;
  *above_uv |= above_prediction_mask_uv[block_size] << shift_uv;
  *left_y |= left_prediction_mask[block_size] << shift_y;
  *left_uv |= left_prediction_mask_uv[block_size] << shift_uv;

  // If the block has no coefficients and is not intra we skip applying
  // the loop filter on block edges.
  if (mbmi->skip && is_inter_block(mbmi)) return;

  // Here we are adding a mask for the transform size. The transform
  // size mask is set to be correct for a 64x64 prediction block size. We
  // mask to match the size of the block we are working on and then shift it
  // into place..
  *above_y |= (size_mask[block_size] & above_64x64_txform_mask[tx_size_y_above])
              << shift_y;
  *above_uv |=
      (size_mask_uv[block_size] & above_64x64_txform_mask_uv[tx_size_uv_above])
      << shift_uv;

  *left_y |= (size_mask[block_size] & left_64x64_txform_mask[tx_size_y_left])
             << shift_y;
  *left_uv |=
      (size_mask_uv[block_size] & left_64x64_txform_mask_uv[tx_size_uv_left])
      << shift_uv;

  // Here we are trying to determine what to do with the internal 4x4 block
  // boundaries.  These differ from the 4x4 boundaries on the outside edge of
  // an 8x8 in that the internal ones can be skipped and don't depend on
  // the prediction block size.
  if (tx_size_y == TX_4X4)
    *int_4x4_y |= (size_mask[block_size] & 0xffffffffffffffffULL) << shift_y;

  if (tx_size_uv == TX_4X4)
    *int_4x4_uv |= (size_mask_uv[block_size] & 0xffff) << shift_uv;
}

// This function does the same thing as the one above with the exception that
// it only affects the y masks. It exists because for blocks < 16x16 in size,
// we only update u and v masks on the first block.
static void build_y_mask(AV1_COMMON *const cm,
                         const loop_filter_info_n *const lfi_n,
                         const MB_MODE_INFO *mbmi, const int shift_y,
                         LOOP_FILTER_MASK *lfm) {
  const TX_SIZE tx_size_y = txsize_sqr_map[mbmi->tx_size];
  const TX_SIZE tx_size_y_left = txsize_horz_map[mbmi->tx_size];
  const TX_SIZE tx_size_y_above = txsize_vert_map[mbmi->tx_size];
  const BLOCK_SIZE block_size = mbmi->sb_type;
  const int filter_level = get_filter_level(cm, lfi_n, 0, 0, mbmi);
  uint64_t *const left_y = &lfm->left_y[tx_size_y_left];
  uint64_t *const above_y = &lfm->above_y[tx_size_y_above];
  uint64_t *const int_4x4_y = &lfm->int_4x4_y;
  int i;

  if (!filter_level) {
    return;
  } else {
    const int w = num_8x8_blocks_wide_lookup[block_size];
    const int h = num_8x8_blocks_high_lookup[block_size];
    const int row = (shift_y >> MAX_MIB_SIZE_LOG2);
    const int col = shift_y - (row << MAX_MIB_SIZE_LOG2);

    for (i = 0; i < h; i++) memset(&lfm->lfl_y[row + i][col], filter_level, w);
  }

  *above_y |= above_prediction_mask[block_size] << shift_y;
  *left_y |= left_prediction_mask[block_size] << shift_y;

  if (mbmi->skip && is_inter_block(mbmi)) return;

  *above_y |= (size_mask[block_size] & above_64x64_txform_mask[tx_size_y_above])
              << shift_y;

  *left_y |= (size_mask[block_size] & left_64x64_txform_mask[tx_size_y_left])
             << shift_y;

  if (tx_size_y == TX_4X4)
    *int_4x4_y |= (size_mask[block_size] & 0xffffffffffffffffULL) << shift_y;
}

// This function sets up the bit masks for the entire 64x64 region represented
// by mi_row, mi_col.
// TODO(JBB): This function only works for yv12.
void av1_setup_mask(AV1_COMMON *const cm, int mi_row, int mi_col,
                    MB_MODE_INFO **mi, int mode_info_stride,
                    LOOP_FILTER_MASK *lfm) {
  assert(0 && "Not yet updated");
  int idx_32, idx_16, idx_8;
  const loop_filter_info_n *const lfi_n = &cm->lf_info;
  MB_MODE_INFO **mip = mi;
  MB_MODE_INFO **mip2 = mi;

  // These are offsets to the next mi in the 64x64 block. It is what gets
  // added to the mi ptr as we go through each loop. It helps us to avoid
  // setting up special row and column counters for each index. The last step
  // brings us out back to the starting position.
  const int offset_32[] = { 4, (mode_info_stride << 2) - 4, 4,
                            -(mode_info_stride << 2) - 4 };
  const int offset_16[] = { 2, (mode_info_stride << 1) - 2, 2,
                            -(mode_info_stride << 1) - 2 };
  const int offset[] = { 1, mode_info_stride - 1, 1, -mode_info_stride - 1 };

  // Following variables represent shifts to position the current block
  // mask over the appropriate block. A shift of 36 to the left will move
  // the bits for the final 32 by 32 block in the 64x64 up 4 rows and left
  // 4 rows to the appropriate spot.
  const int shift_32_y[] = { 0, 4, 32, 36 };
  const int shift_16_y[] = { 0, 2, 16, 18 };
  const int shift_8_y[] = { 0, 1, 8, 9 };
  const int shift_32_uv[] = { 0, 2, 8, 10 };
  const int shift_16_uv[] = { 0, 1, 4, 5 };
  int i;
  const int max_rows = AOMMIN(cm->mi_rows - mi_row, MAX_MIB_SIZE);
  const int max_cols = AOMMIN(cm->mi_cols - mi_col, MAX_MIB_SIZE);

  av1_zero(*lfm);
  assert(mip[0] != NULL);

  // TODO(jimbankoski): Try moving most of the following code into decode
  // loop and storing lfm in the mbmi structure so that we don't have to go
  // through the recursive loop structure multiple times.
  switch (mip[0]->sb_type) {
    case BLOCK_64X64: build_masks(cm, lfi_n, mip[0], 0, 0, lfm); break;
    case BLOCK_64X32:
      build_masks(cm, lfi_n, mip[0], 0, 0, lfm);
      mip2 = mip + mode_info_stride * 4;
      if (4 >= max_rows) break;
      build_masks(cm, lfi_n, mip2[0], 32, 8, lfm);
      break;
    case BLOCK_32X64:
      build_masks(cm, lfi_n, mip[0], 0, 0, lfm);
      mip2 = mip + 4;
      if (4 >= max_cols) break;
      build_masks(cm, lfi_n, mip2[0], 4, 2, lfm);
      break;
    default:
      for (idx_32 = 0; idx_32 < 4; mip += offset_32[idx_32], ++idx_32) {
        const int shift_y_32 = shift_32_y[idx_32];
        const int shift_uv_32 = shift_32_uv[idx_32];
        const int mi_32_col_offset = ((idx_32 & 1) << 2);
        const int mi_32_row_offset = ((idx_32 >> 1) << 2);
        if (mi_32_col_offset >= max_cols || mi_32_row_offset >= max_rows)
          continue;
        switch (mip[0]->sb_type) {
          case BLOCK_32X32:
            build_masks(cm, lfi_n, mip[0], shift_y_32, shift_uv_32, lfm);
            break;
          case BLOCK_32X16:
            build_masks(cm, lfi_n, mip[0], shift_y_32, shift_uv_32, lfm);
            if (mi_32_row_offset + 2 >= max_rows) continue;
            mip2 = mip + mode_info_stride * 2;
            build_masks(cm, lfi_n, mip2[0], shift_y_32 + 16, shift_uv_32 + 4,
                        lfm);
            break;
          case BLOCK_16X32:
            build_masks(cm, lfi_n, mip[0], shift_y_32, shift_uv_32, lfm);
            if (mi_32_col_offset + 2 >= max_cols) continue;
            mip2 = mip + 2;
            build_masks(cm, lfi_n, mip2[0], shift_y_32 + 2, shift_uv_32 + 1,
                        lfm);
            break;
          default:
            for (idx_16 = 0; idx_16 < 4; mip += offset_16[idx_16], ++idx_16) {
              const int shift_y_32_16 = shift_y_32 + shift_16_y[idx_16];
              const int shift_uv_32_16 = shift_uv_32 + shift_16_uv[idx_16];
              const int mi_16_col_offset =
                  mi_32_col_offset + ((idx_16 & 1) << 1);
              const int mi_16_row_offset =
                  mi_32_row_offset + ((idx_16 >> 1) << 1);

              if (mi_16_col_offset >= max_cols || mi_16_row_offset >= max_rows)
                continue;

              switch (mip[0]->sb_type) {
                case BLOCK_16X16:
                  build_masks(cm, lfi_n, mip[0], shift_y_32_16, shift_uv_32_16,
                              lfm);
                  break;
                case BLOCK_16X8:
                  build_masks(cm, lfi_n, mip[0], shift_y_32_16, shift_uv_32_16,
                              lfm);
                  if (mi_16_row_offset + 1 >= max_rows) continue;
                  mip2 = mip + mode_info_stride;
                  build_y_mask(cm, lfi_n, mip2[0], shift_y_32_16 + 8, lfm);
                  break;
                case BLOCK_8X16:
                  build_masks(cm, lfi_n, mip[0], shift_y_32_16, shift_uv_32_16,
                              lfm);
                  if (mi_16_col_offset + 1 >= max_cols) continue;
                  mip2 = mip + 1;
                  build_y_mask(cm, lfi_n, mip2[0], shift_y_32_16 + 1, lfm);
                  break;
                default: {
                  const int shift_y_32_16_8_zero = shift_y_32_16 + shift_8_y[0];
                  build_masks(cm, lfi_n, mip[0], shift_y_32_16_8_zero,
                              shift_uv_32_16, lfm);
                  mip += offset[0];
                  for (idx_8 = 1; idx_8 < 4; mip += offset[idx_8], ++idx_8) {
                    const int shift_y_32_16_8 =
                        shift_y_32_16 + shift_8_y[idx_8];
                    const int mi_8_col_offset =
                        mi_16_col_offset + ((idx_8 & 1));
                    const int mi_8_row_offset =
                        mi_16_row_offset + ((idx_8 >> 1));

                    if (mi_8_col_offset >= max_cols ||
                        mi_8_row_offset >= max_rows)
                      continue;
                    build_y_mask(cm, lfi_n, mip[0], shift_y_32_16_8, lfm);
                  }
                  break;
                }
              }
            }
            break;
        }
      }
      break;
  }
  // The largest loopfilter we have is 16x16 so we use the 16x16 mask
  // for 32x32 transforms also.
  lfm->left_y[TX_16X16] |= lfm->left_y[TX_32X32];
  lfm->above_y[TX_16X16] |= lfm->above_y[TX_32X32];
  lfm->left_uv[TX_16X16] |= lfm->left_uv[TX_32X32];
  lfm->above_uv[TX_16X16] |= lfm->above_uv[TX_32X32];

  // We do at least 8 tap filter on every 32x32 even if the transform size
  // is 4x4. So if the 4x4 is set on a border pixel add it to the 8x8 and
  // remove it from the 4x4.
  lfm->left_y[TX_8X8] |= lfm->left_y[TX_4X4] & left_border;
  lfm->left_y[TX_4X4] &= ~left_border;
  lfm->above_y[TX_8X8] |= lfm->above_y[TX_4X4] & above_border;
  lfm->above_y[TX_4X4] &= ~above_border;
  lfm->left_uv[TX_8X8] |= lfm->left_uv[TX_4X4] & left_border_uv;
  lfm->left_uv[TX_4X4] &= ~left_border_uv;
  lfm->above_uv[TX_8X8] |= lfm->above_uv[TX_4X4] & above_border_uv;
  lfm->above_uv[TX_4X4] &= ~above_border_uv;

  // We do some special edge handling.
  if (mi_row + MAX_MIB_SIZE > cm->mi_rows) {
    const uint64_t rows = cm->mi_rows - mi_row;

    // Each pixel inside the border gets a 1,
    const uint64_t mask_y = (((uint64_t)1 << (rows << MAX_MIB_SIZE_LOG2)) - 1);
    const uint16_t mask_uv =
        (((uint16_t)1 << (((rows + 1) >> 1) << (MAX_MIB_SIZE_LOG2 - 1))) - 1);

    // Remove values completely outside our border.
    for (i = 0; i < TX_32X32; i++) {
      lfm->left_y[i] &= mask_y;
      lfm->above_y[i] &= mask_y;
      lfm->left_uv[i] &= mask_uv;
      lfm->above_uv[i] &= mask_uv;
    }
    lfm->int_4x4_y &= mask_y;
    lfm->above_int_4x4_uv = lfm->left_int_4x4_uv & mask_uv;

    // We don't apply a wide loop filter on the last uv block row. If set
    // apply the shorter one instead.
    if (rows == 1) {
      lfm->above_uv[TX_8X8] |= lfm->above_uv[TX_16X16];
      lfm->above_uv[TX_16X16] = 0;
    }
    if (rows == 5) {
      lfm->above_uv[TX_8X8] |= lfm->above_uv[TX_16X16] & 0xff00;
      lfm->above_uv[TX_16X16] &= ~(lfm->above_uv[TX_16X16] & 0xff00);
    }
  } else {
    lfm->above_int_4x4_uv = lfm->left_int_4x4_uv;
  }

  if (mi_col + MAX_MIB_SIZE > cm->mi_cols) {
    const uint64_t columns = cm->mi_cols - mi_col;

    // Each pixel inside the border gets a 1, the multiply copies the border
    // to where we need it.
    const uint64_t mask_y = (((1 << columns) - 1)) * 0x0101010101010101ULL;
    const uint16_t mask_uv = ((1 << ((columns + 1) >> 1)) - 1) * 0x1111;

    // Internal edges are not applied on the last column of the image so
    // we mask 1 more for the internal edges
    const uint16_t mask_uv_int = ((1 << (columns >> 1)) - 1) * 0x1111;

    // Remove the bits outside the image edge.
    for (i = 0; i < TX_32X32; i++) {
      lfm->left_y[i] &= mask_y;
      lfm->above_y[i] &= mask_y;
      lfm->left_uv[i] &= mask_uv;
      lfm->above_uv[i] &= mask_uv;
    }
    lfm->int_4x4_y &= mask_y;
    lfm->left_int_4x4_uv &= mask_uv_int;

    // We don't apply a wide loop filter on the last uv column. If set
    // apply the shorter one instead.
    if (columns == 1) {
      lfm->left_uv[TX_8X8] |= lfm->left_uv[TX_16X16];
      lfm->left_uv[TX_16X16] = 0;
    }
    if (columns == 5) {
      lfm->left_uv[TX_8X8] |= (lfm->left_uv[TX_16X16] & 0xcccc);
      lfm->left_uv[TX_16X16] &= ~(lfm->left_uv[TX_16X16] & 0xcccc);
    }
  }
  // We don't apply a loop filter on the first column in the image, mask that
  // out.
  if (mi_col == 0) {
    for (i = 0; i < TX_32X32; i++) {
      lfm->left_y[i] &= 0xfefefefefefefefeULL;
      lfm->left_uv[i] &= 0xeeee;
    }
  }

  // Assert if we try to apply 2 different loop filters at the same position.
  assert(!(lfm->left_y[TX_16X16] & lfm->left_y[TX_8X8]));
  assert(!(lfm->left_y[TX_16X16] & lfm->left_y[TX_4X4]));
  assert(!(lfm->left_y[TX_8X8] & lfm->left_y[TX_4X4]));
  assert(!(lfm->int_4x4_y & lfm->left_y[TX_16X16]));
  assert(!(lfm->left_uv[TX_16X16] & lfm->left_uv[TX_8X8]));
  assert(!(lfm->left_uv[TX_16X16] & lfm->left_uv[TX_4X4]));
  assert(!(lfm->left_uv[TX_8X8] & lfm->left_uv[TX_4X4]));
  assert(!(lfm->left_int_4x4_uv & lfm->left_uv[TX_16X16]));
  assert(!(lfm->above_y[TX_16X16] & lfm->above_y[TX_8X8]));
  assert(!(lfm->above_y[TX_16X16] & lfm->above_y[TX_4X4]));
  assert(!(lfm->above_y[TX_8X8] & lfm->above_y[TX_4X4]));
  assert(!(lfm->int_4x4_y & lfm->above_y[TX_16X16]));
  assert(!(lfm->above_uv[TX_16X16] & lfm->above_uv[TX_8X8]));
  assert(!(lfm->above_uv[TX_16X16] & lfm->above_uv[TX_4X4]));
  assert(!(lfm->above_uv[TX_8X8] & lfm->above_uv[TX_4X4]));
  assert(!(lfm->above_int_4x4_uv & lfm->above_uv[TX_16X16]));
}

typedef struct {
  unsigned int m16x16;
  unsigned int m8x8;
  unsigned int m4x4;
} FilterMasks;

static const uint32_t av1_transform_masks[NUM_EDGE_DIRS][TX_SIZES_ALL] = {
  {
      4 - 1,   // TX_4X4
      8 - 1,   // TX_8X8
      16 - 1,  // TX_16X16
      32 - 1,  // TX_32X32
      64 - 1,  // TX_64X64
      4 - 1,   // TX_4X8
      8 - 1,   // TX_8X4
      8 - 1,   // TX_8X16
      16 - 1,  // TX_16X8
      16 - 1,  // TX_16X32
      32 - 1,  // TX_32X16
      32 - 1,  // TX_32X64
      64 - 1,  // TX_64X32
      4 - 1,   // TX_4X16
      16 - 1,  // TX_16X4
      8 - 1,   // TX_8X32
      32 - 1,  // TX_32X8
      16 - 1,  // TX_16X64
      64 - 1,  // TX_64X16
  },
  {
      4 - 1,   // TX_4X4
      8 - 1,   // TX_8X8
      16 - 1,  // TX_16X16
      32 - 1,  // TX_32X32
      64 - 1,  // TX_64X64
      8 - 1,   // TX_4X8
      4 - 1,   // TX_8X4
      16 - 1,  // TX_8X16
      8 - 1,   // TX_16X8
      32 - 1,  // TX_16X32
      16 - 1,  // TX_32X16
      64 - 1,  // TX_32X64
      32 - 1,  // TX_64X32
      16 - 1,  // TX_4X16
      4 - 1,   // TX_16X4
      32 - 1,  // TX_8X32
      8 - 1,   // TX_32X8
      64 - 1,  // TX_16X64
      16 - 1,  // TX_64X16
  }
};

static TX_SIZE av1_get_transform_size(
    const MB_MODE_INFO *const mbmi, const EDGE_DIR edge_dir, const int mi_row,
    const int mi_col, const int plane,
    const struct macroblockd_plane *plane_ptr) {
  assert(mbmi != NULL);
  TX_SIZE tx_size = (plane == AOM_PLANE_Y)
                        ? mbmi->tx_size
                        : av1_get_uv_tx_size(mbmi, plane_ptr->subsampling_x,
                                             plane_ptr->subsampling_y);
  assert(tx_size < TX_SIZES_ALL);
  if ((plane == AOM_PLANE_Y) && is_inter_block(mbmi) && !mbmi->skip) {
    const BLOCK_SIZE sb_type = mbmi->sb_type;
    const int blk_row = mi_row & (mi_size_high[sb_type] - 1);
    const int blk_col = mi_col & (mi_size_wide[sb_type] - 1);
    const TX_SIZE mb_tx_size =
        mbmi->inter_tx_size[av1_get_txb_size_index(sb_type, blk_row, blk_col)];
    assert(mb_tx_size < TX_SIZES_ALL);
    tx_size = mb_tx_size;
  }

  // since in case of chrominance or non-square transorm need to convert
  // transform size into transform size in particular direction.
  // for vertical edge, filter direction is horizontal, for horizontal
  // edge, filter direction is vertical.
  tx_size = (VERT_EDGE == edge_dir) ? txsize_horz_map[tx_size]
                                    : txsize_vert_map[tx_size];
  return tx_size;
}

typedef struct AV1_DEBLOCKING_PARAMETERS {
  // length of the filter applied to the outer edge
  uint32_t filter_length;
  // deblocking limits
  const uint8_t *lim;
  const uint8_t *mblim;
  const uint8_t *hev_thr;
} AV1_DEBLOCKING_PARAMETERS;

// Return TX_SIZE from av1_get_transform_size(), so it is plane and direction
// awared
static TX_SIZE set_lpf_parameters(
    AV1_DEBLOCKING_PARAMETERS *const params, const ptrdiff_t mode_step,
    const AV1_COMMON *const cm, const EDGE_DIR edge_dir, const uint32_t x,
    const uint32_t y, const int plane,
    const struct macroblockd_plane *const plane_ptr) {
  // reset to initial values
  params->filter_length = 0;

  // no deblocking is required
  const uint32_t width = plane_ptr->dst.width;
  const uint32_t height = plane_ptr->dst.height;
  if ((width <= x) || (height <= y)) {
    // just return the smallest transform unit size
    return TX_4X4;
  }

  const uint32_t scale_horz = plane_ptr->subsampling_x;
  const uint32_t scale_vert = plane_ptr->subsampling_y;
  // for sub8x8 block, chroma prediction mode is obtained from the bottom/right
  // mi structure of the co-located 8x8 luma block. so for chroma plane, mi_row
  // and mi_col should map to the bottom/right mi structure, i.e, both mi_row
  // and mi_col should be odd number for chroma plane.
  const int mi_row = scale_vert | ((y << scale_vert) >> MI_SIZE_LOG2);
  const int mi_col = scale_horz | ((x << scale_horz) >> MI_SIZE_LOG2);
  MB_MODE_INFO **mi = cm->mi_grid_visible + mi_row * cm->mi_stride + mi_col;
  const MB_MODE_INFO *mbmi = mi[0];
  // If current mbmi is not correctly setup, return an invalid value to stop
  // filtering. One example is that if this tile is not coded, then its mbmi
  // it not set up.
  if (mbmi == NULL) return TX_INVALID;

  const TX_SIZE ts =
      av1_get_transform_size(mi[0], edge_dir, mi_row, mi_col, plane, plane_ptr);

  {
    const uint32_t coord = (VERT_EDGE == edge_dir) ? (x) : (y);
    const int32_t tu_edge =
        (coord & av1_transform_masks[edge_dir][ts]) ? (0) : (1);

    if (!tu_edge) return ts;

    // prepare outer edge parameters. deblock the edge if it's an edge of a TU
    {
      const uint32_t curr_level =
          get_filter_level(cm, &cm->lf_info, edge_dir, plane, mbmi);
      const int curr_skipped = mbmi->skip && is_inter_block(mbmi);
      uint32_t level = curr_level;
      if (coord) {
        {
          const MB_MODE_INFO *const mi_prev = *(mi - mode_step);
          if (mi_prev == NULL) return TX_INVALID;
          const int pv_row =
              (VERT_EDGE == edge_dir) ? (mi_row) : (mi_row - (1 << scale_vert));
          const int pv_col =
              (VERT_EDGE == edge_dir) ? (mi_col - (1 << scale_horz)) : (mi_col);
          const TX_SIZE pv_ts = av1_get_transform_size(
              mi_prev, edge_dir, pv_row, pv_col, plane, plane_ptr);

          const uint32_t pv_lvl =
              get_filter_level(cm, &cm->lf_info, edge_dir, plane, mi_prev);

          const int pv_skip = mi_prev->skip && is_inter_block(mi_prev);
          const BLOCK_SIZE bsize =
              ss_size_lookup[mbmi->sb_type][scale_horz][scale_vert];
          const int prediction_masks = edge_dir == VERT_EDGE
                                           ? block_size_wide[bsize] - 1
                                           : block_size_high[bsize] - 1;
          const int32_t pu_edge = !(coord & prediction_masks);
          // if the current and the previous blocks are skipped,
          // deblock the edge if the edge belongs to a PU's edge only.
          if ((curr_level || pv_lvl) &&
              (!pv_skip || !curr_skipped || pu_edge)) {
            const TX_SIZE min_ts = AOMMIN(ts, pv_ts);
            if (TX_4X4 >= min_ts) {
              params->filter_length = 4;
            } else if (TX_8X8 == min_ts) {
              if (plane != 0)
                params->filter_length = 6;
              else
                params->filter_length = 8;
            } else {
              params->filter_length = 14;
              // No wide filtering for chroma plane
              if (plane != 0) {
                params->filter_length = 6;
              }
            }

            // update the level if the current block is skipped,
            // but the previous one is not
            level = (curr_level) ? (curr_level) : (pv_lvl);
          }
        }
      }
      // prepare common parameters
      if (params->filter_length) {
        const loop_filter_thresh *const limits = cm->lf_info.lfthr + level;
        params->lim = limits->lim;
        params->mblim = limits->mblim;
        params->hev_thr = limits->hev_thr;
      }
    }
  }

  return ts;
}

static void filter_block_plane_vert(const AV1_COMMON *const cm, const int plane,
                                    const MACROBLOCKD_PLANE *const plane_ptr,
                                    const uint32_t mi_row,
                                    const uint32_t mi_col) {
  const int row_step = MI_SIZE >> MI_SIZE_LOG2;
  const uint32_t scale_horz = plane_ptr->subsampling_x;
  const uint32_t scale_vert = plane_ptr->subsampling_y;
  uint8_t *const dst_ptr = plane_ptr->dst.buf;
  const int dst_stride = plane_ptr->dst.stride;
  const int y_range = (MAX_MIB_SIZE >> scale_vert);
  const int x_range = (MAX_MIB_SIZE >> scale_horz);
  for (int y = 0; y < y_range; y += row_step) {
    uint8_t *p = dst_ptr + y * MI_SIZE * dst_stride;
    for (int x = 0; x < x_range;) {
      // inner loop always filter vertical edges in a MI block. If MI size
      // is 8x8, it will filter the vertical edge aligned with a 8x8 block.
      // If 4x4 trasnform is used, it will then filter the internal edge
      //  aligned with a 4x4 block
      const uint32_t curr_x = ((mi_col * MI_SIZE) >> scale_horz) + x * MI_SIZE;
      const uint32_t curr_y = ((mi_row * MI_SIZE) >> scale_vert) + y * MI_SIZE;
      uint32_t advance_units;
      TX_SIZE tx_size;
      AV1_DEBLOCKING_PARAMETERS params;
      memset(&params, 0, sizeof(params));

      tx_size = set_lpf_parameters(&params, ((ptrdiff_t)1 << scale_horz), cm,
                                   VERT_EDGE, curr_x, curr_y, plane, plane_ptr);
      if (tx_size == TX_INVALID) {
        params.filter_length = 0;
        tx_size = TX_4X4;
      }

      switch (params.filter_length) {
        // apply 4-tap filtering
        case 4:
          if (cm->use_highbitdepth)
            aom_highbd_lpf_vertical_4(CONVERT_TO_SHORTPTR(p), dst_stride,
                                      params.mblim, params.lim, params.hev_thr,
                                      cm->bit_depth);
          else
            aom_lpf_vertical_4(p, dst_stride, params.mblim, params.lim,
                               params.hev_thr);
          break;
        case 6:  // apply 6-tap filter for chroma plane only
          assert(plane != 0);
          if (cm->use_highbitdepth)
            aom_highbd_lpf_vertical_6(CONVERT_TO_SHORTPTR(p), dst_stride,
                                      params.mblim, params.lim, params.hev_thr,
                                      cm->bit_depth);
          else
            aom_lpf_vertical_6(p, dst_stride, params.mblim, params.lim,
                               params.hev_thr);
          break;
        // apply 8-tap filtering
        case 8:
          if (cm->use_highbitdepth)
            aom_highbd_lpf_vertical_8(CONVERT_TO_SHORTPTR(p), dst_stride,
                                      params.mblim, params.lim, params.hev_thr,
                                      cm->bit_depth);
          else
            aom_lpf_vertical_8(p, dst_stride, params.mblim, params.lim,
                               params.hev_thr);
          break;
        // apply 14-tap filtering
        case 14:
          if (cm->use_highbitdepth)
            aom_highbd_lpf_vertical_14(CONVERT_TO_SHORTPTR(p), dst_stride,
                                       params.mblim, params.lim, params.hev_thr,
                                       cm->bit_depth);
          else
            aom_lpf_vertical_14(p, dst_stride, params.mblim, params.lim,
                                params.hev_thr);
          break;
        // no filtering
        default: break;
      }
      // advance the destination pointer
      advance_units = tx_size_wide_unit[tx_size];
      x += advance_units;
      p += advance_units * MI_SIZE;
    }
  }
}

static void filter_block_plane_horz(const AV1_COMMON *const cm, const int plane,
                                    const MACROBLOCKD_PLANE *const plane_ptr,
                                    const uint32_t mi_row,
                                    const uint32_t mi_col) {
  const int col_step = MI_SIZE >> MI_SIZE_LOG2;
  const uint32_t scale_horz = plane_ptr->subsampling_x;
  const uint32_t scale_vert = plane_ptr->subsampling_y;
  uint8_t *const dst_ptr = plane_ptr->dst.buf;
  const int dst_stride = plane_ptr->dst.stride;
  const int y_range = (MAX_MIB_SIZE >> scale_vert);
  const int x_range = (MAX_MIB_SIZE >> scale_horz);
  for (int x = 0; x < x_range; x += col_step) {
    uint8_t *p = dst_ptr + x * MI_SIZE;
    for (int y = 0; y < y_range;) {
      // inner loop always filter vertical edges in a MI block. If MI size
      // is 8x8, it will first filter the vertical edge aligned with a 8x8
      // block. If 4x4 trasnform is used, it will then filter the internal
      // edge aligned with a 4x4 block
      const uint32_t curr_x = ((mi_col * MI_SIZE) >> scale_horz) + x * MI_SIZE;
      const uint32_t curr_y = ((mi_row * MI_SIZE) >> scale_vert) + y * MI_SIZE;
      uint32_t advance_units;
      TX_SIZE tx_size;
      AV1_DEBLOCKING_PARAMETERS params;
      memset(&params, 0, sizeof(params));

      tx_size = set_lpf_parameters(&params, (cm->mi_stride << scale_vert), cm,
                                   HORZ_EDGE, curr_x, curr_y, plane, plane_ptr);
      if (tx_size == TX_INVALID) {
        params.filter_length = 0;
        tx_size = TX_4X4;
      }

      switch (params.filter_length) {
        // apply 4-tap filtering
        case 4:
          if (cm->use_highbitdepth)
            aom_highbd_lpf_horizontal_4(CONVERT_TO_SHORTPTR(p), dst_stride,
                                        params.mblim, params.lim,
                                        params.hev_thr, cm->bit_depth);
          else
            aom_lpf_horizontal_4(p, dst_stride, params.mblim, params.lim,
                                 params.hev_thr);
          break;
        // apply 6-tap filtering
        case 6:
          assert(plane != 0);
          if (cm->use_highbitdepth)
            aom_highbd_lpf_horizontal_6(CONVERT_TO_SHORTPTR(p), dst_stride,
                                        params.mblim, params.lim,
                                        params.hev_thr, cm->bit_depth);
          else
            aom_lpf_horizontal_6(p, dst_stride, params.mblim, params.lim,
                                 params.hev_thr);
          break;
        // apply 8-tap filtering
        case 8:
          if (cm->use_highbitdepth)
            aom_highbd_lpf_horizontal_8(CONVERT_TO_SHORTPTR(p), dst_stride,
                                        params.mblim, params.lim,
                                        params.hev_thr, cm->bit_depth);
          else
            aom_lpf_horizontal_8(p, dst_stride, params.mblim, params.lim,
                                 params.hev_thr);
          break;
        // apply 14-tap filtering
        case 14:
          if (cm->use_highbitdepth)
            aom_highbd_lpf_horizontal_14(CONVERT_TO_SHORTPTR(p), dst_stride,
                                         params.mblim, params.lim,
                                         params.hev_thr, cm->bit_depth);
          else
            aom_lpf_horizontal_14(p, dst_stride, params.mblim, params.lim,
                                  params.hev_thr);
          break;
        // no filtering
        default: break;
      }

      // advance the destination pointer
      advance_units = tx_size_high_unit[tx_size];
      y += advance_units;
      p += advance_units * dst_stride * MI_SIZE;
    }
  }
}

#if LOOP_FILTER_BITMASK
static INLINE enum lf_path get_loop_filter_path(
    int plane, struct macroblockd_plane pd[MAX_MB_PLANE]) {
  if (pd[plane].subsampling_y == 1 && pd[plane].subsampling_x == 1)
    return LF_PATH_420;
  else if (pd[plane].subsampling_y == 0 && pd[plane].subsampling_x == 0)
    return LF_PATH_444;
  else
    return LF_PATH_SLOW;
}

static void loop_filter_block_plane_vert(AV1_COMMON *const cm,
                                         struct macroblockd_plane *pd, int pl,
                                         int mi_row, int mi_col,
                                         enum lf_path path,
                                         LoopFilterMask *lf_mask) {
  MB_MODE_INFO **mi = cm->mi_grid_visible + mi_row * cm->mi_stride + mi_col;
  switch (path) {
    case LF_PATH_420:
      av1_filter_block_plane_ss00_ver(cm, pd, pl, mi_row, lf_mask);
      break;
    case LF_PATH_444:
      av1_filter_block_plane_ss11_ver(cm, pd, pl, mi_row, lf_mask);
      break;
    case LF_PATH_SLOW:
      av1_filter_block_plane_non420_ver(cm, pd, mi, mi_row, mi_col, pl);
      break;
  }
}

static void loop_filter_block_plane_horz(AV1_COMMON *const cm,
                                         struct macroblockd_plane *pd, int pl,
                                         int mi_row, int mi_col,
                                         enum lf_path path,
                                         LoopFilterMask *lf_mask) {
  MB_MODE_INFO **mi = cm->mi_grid_visible + mi_row * cm->mi_stride + mi_col;
  switch (path) {
    case LF_PATH_420:
      av1_filter_block_plane_ss00_hor(cm, pd, pl, mi_row, lf_mask);
      break;
    case LF_PATH_444:
      av1_filter_block_plane_ss11_hor(cm, pd, pl, mi_row, lf_mask);
      break;
    case LF_PATH_SLOW:
      av1_filter_block_plane_non420_hor(cm, pd, mi, mi_row, mi_col, pl);
      break;
  }
}
#endif  // LOOP_FILTER_BITMASK

void av1_loop_filter_rows(YV12_BUFFER_CONFIG *frame_buffer, AV1_COMMON *cm,
                          struct macroblockd_plane *pd, int start, int stop,
                          int plane) {
  const int num_planes = av1_num_planes(cm);
  const int col_start = 0;
  const int col_end = cm->mi_cols;
  int mi_row, mi_col;

#if LOOP_FILTER_BITMASK
  enum lf_path path = get_loop_filter_path(plane, pd);

  // filter all vertical edges in every super block
  for (mi_row = start; mi_row < stop; mi_row += MAX_MIB_SIZE) {
    for (mi_col = col_start; mi_col < col_end; mi_col += MAX_MIB_SIZE) {
      av1_setup_dst_planes(pd, cm->seq_params.sb_size, frame_buffer, mi_row,
                           mi_col, num_planes);

      LoopFilterMask *lf_mask = get_loop_filter_mask(cm, mi_row, mi_col);
      av1_setup_bitmask(cm, mi_row, mi_col, plane, pd[plane].subsampling_x,
                        pd[plane].subsampling_y, lf_mask);
      loop_filter_block_plane_vert(cm, pd, plane, mi_row, mi_col, path,
                                   lf_mask);
    }
  }

  // filter all horizontal edges in every super block
  for (mi_row = start; mi_row < stop; mi_row += MAX_MIB_SIZE) {
    for (mi_col = col_start; mi_col < col_end; mi_col += MAX_MIB_SIZE) {
      av1_setup_dst_planes(pd, cm->seq_params.sb_size, frame_buffer, mi_row,
                           mi_col, num_planes);

      LoopFilterMask *lf_mask = get_loop_filter_mask(cm, mi_row, mi_col);
      loop_filter_block_plane_horz(cm, pd, plane, mi_row, mi_col, path,
                                   lf_mask);
    }
  }
#else
  // filter all vertical edges in every 64x64 super block
  for (mi_row = start; mi_row < stop; mi_row += MAX_MIB_SIZE) {
    for (mi_col = col_start; mi_col < col_end; mi_col += MAX_MIB_SIZE) {
      av1_setup_dst_planes(pd, cm->seq_params.sb_size, frame_buffer, mi_row,
                           mi_col, num_planes);
      filter_block_plane_vert(cm, plane, &pd[plane], mi_row, mi_col);
    }
  }

  // filter all horizontal edges in every 64x64 super block
  for (mi_row = start; mi_row < stop; mi_row += MAX_MIB_SIZE) {
    for (mi_col = col_start; mi_col < col_end; mi_col += MAX_MIB_SIZE) {
      av1_setup_dst_planes(pd, cm->seq_params.sb_size, frame_buffer, mi_row,
                           mi_col, num_planes);
      filter_block_plane_horz(cm, plane, &pd[plane], mi_row, mi_col);
    }
  }
#endif  // LOOP_FILTER_BITMASK
}

void av1_loop_filter_frame(YV12_BUFFER_CONFIG *frame, AV1_COMMON *cm,
                           MACROBLOCKD *xd, int frame_filter_level,
                           int frame_filter_level_r, int plane,
                           int partial_frame) {
  int start_mi_row, end_mi_row, mi_rows_to_filter;

  if (!frame_filter_level && !frame_filter_level_r) return;
  start_mi_row = 0;
  mi_rows_to_filter = cm->mi_rows;
  if (partial_frame && cm->mi_rows > 8) {
    start_mi_row = cm->mi_rows >> 1;
    start_mi_row &= 0xfffffff8;
    mi_rows_to_filter = AOMMAX(cm->mi_rows / 8, 8);
  }
  end_mi_row = start_mi_row + mi_rows_to_filter;
  av1_loop_filter_frame_init(cm, frame_filter_level, frame_filter_level_r,
                             plane);
  av1_loop_filter_rows(frame, cm, xd->plane, start_mi_row, end_mi_row, plane);
}
