/* SPDX-License-Identifier: MIT */
/*
 * tinfl -- the inflate half of miniz, vendored for the UML qcow2 backend.
 *
 * Upstream: https://github.com/richgel999/miniz, tag 3.0.2.
 * Files folded in here: miniz_tinfl.h (flags/status/struct), the type and
 * macro layers of miniz_common.h.  The code body lives in
 * qcow2_inflate_user.c (from miniz_tinfl.c), where the full MIT license
 * text is kept.
 *
 * Only the low-level one-shot decompressor is kept: tinfl_decompress()
 * driven by a caller-provided tinfl_decompressor, with
 * TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF (the decompressed cluster fits
 * in the output buffer).  No allocation, no stdio, no global state -- this
 * is required to run on the ubd IO helper thread, which must not call
 * kernel functions.
 *
 * MZ_ASSERT is deliberately a no-op: aborting the whole UML process on a
 * corrupt stream is not acceptable; corrupt input surfaces as a non-DONE
 * status instead.
 *
 * One-shot usage (raw deflate stream, no zlib header):
 *     tinfl_init(&dec);
 *     in_len = <compressed bytes available>;
 *     out_len = <output buffer capacity, the cluster size>;
 *     status = tinfl_decompress(&dec, in, &in_len, out, out, &out_len,
 *                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
 * On success status == TINFL_STATUS_DONE and *out_len has been reduced by
 * the number of bytes produced (see the mem_to_mem helper upstream); call
 * it again with the same buffers while status == TINFL_STATUS_HAS_MORE_OUTPUT.
 */
#ifndef __QCOW2_INFLATE_H__
#define __QCOW2_INFLATE_H__

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- types and macros (miniz_common.h) --------------------------------- */

typedef unsigned char mz_uint8;
typedef signed short mz_int16;
typedef unsigned short mz_uint16;
typedef unsigned int mz_uint32;
typedef unsigned int mz_uint;
typedef int64_t mz_int64;
typedef uint64_t mz_uint64;

#define MZ_MACRO_END while (0)
#define MZ_ASSERT(x) ((void)0)
#define MZ_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MZ_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MZ_CLEAR_ARR(obj) memset((obj), 0, sizeof(obj))

/* Little-endian reads, the portable path of miniz_common.h. */
#define MZ_READ_LE16(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U))
#define MZ_READ_LE32(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U) | ((mz_uint32)(((const mz_uint8 *)(p))[2]) << 16U) | ((mz_uint32)(((const mz_uint8 *)(p))[3]) << 24U))

/* Both targets of this port (x86_64 and arm64 hosts) have 64-bit
 * registers; miniz's own probe needs miniz.h, which is not vendored. */
#define MINIZ_HAS_64BIT_REGISTERS 1

/* ---- decompression flags and status (miniz_tinfl.h) -------------------- */

enum
{
    TINFL_FLAG_PARSE_ZLIB_HEADER = 1,
    TINFL_FLAG_HAS_MORE_INPUT = 2,
    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4,
    TINFL_FLAG_COMPUTE_ADLER32 = 8
};

typedef enum {
    TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS = -4,
    TINFL_STATUS_BAD_PARAM = -3,
    TINFL_STATUS_ADLER32_MISMATCH = -2,
    TINFL_STATUS_FAILED = -1,
    TINFL_STATUS_DONE = 0,
    TINFL_STATUS_NEEDS_MORE_INPUT = 1,
    TINFL_STATUS_HAS_MORE_OUTPUT = 2
} tinfl_status;

/* ---- the decompressor state (miniz_tinfl.h) ---------------------------- */

enum
{
    TINFL_MAX_HUFF_TABLES = 3,
    TINFL_MAX_HUFF_SYMBOLS_0 = 288,
    TINFL_MAX_HUFF_SYMBOLS_1 = 32,
    TINFL_MAX_HUFF_SYMBOLS_2 = 19,
    TINFL_FAST_LOOKUP_BITS = 10,
    TINFL_FAST_LOOKUP_SIZE = 1 << TINFL_FAST_LOOKUP_BITS
};

#if MINIZ_HAS_64BIT_REGISTERS
#define TINFL_USE_64BIT_BITBUF 1
#else
#define TINFL_USE_64BIT_BITBUF 0
#endif

#if TINFL_USE_64BIT_BITBUF
typedef mz_uint64 tinfl_bit_buf_t;
#define TINFL_BITBUF_SIZE (64)
#else
typedef mz_uint32 tinfl_bit_buf_t;
#define TINFL_BITBUF_SIZE (32)
#endif

typedef struct tinfl_decompressor_tag tinfl_decompressor;

struct tinfl_decompressor_tag
{
    mz_uint32 m_state, m_num_bits, m_zhdr0, m_zhdr1, m_z_adler32, m_final, m_type, m_check_adler32, m_dist, m_counter, m_num_extra, m_table_sizes[TINFL_MAX_HUFF_TABLES];
    tinfl_bit_buf_t m_bit_buf;
    size_t m_dist_from_out_buf_start;
    mz_int16 m_look_up[TINFL_MAX_HUFF_TABLES][TINFL_FAST_LOOKUP_SIZE];
    mz_int16 m_tree_0[TINFL_MAX_HUFF_SYMBOLS_0 * 2];
    mz_int16 m_tree_1[TINFL_MAX_HUFF_SYMBOLS_1 * 2];
    mz_int16 m_tree_2[TINFL_MAX_HUFF_SYMBOLS_2 * 2];
    mz_uint8 m_code_size_0[TINFL_MAX_HUFF_SYMBOLS_0];
    mz_uint8 m_code_size_1[TINFL_MAX_HUFF_SYMBOLS_1];
    mz_uint8 m_code_size_2[TINFL_MAX_HUFF_SYMBOLS_2];
    mz_uint8 m_raw_header[4], m_len_codes[TINFL_MAX_HUFF_SYMBOLS_0 + TINFL_MAX_HUFF_SYMBOLS_1 + 137];
};

/* Initializes the decompressor to its initial state. */
#define tinfl_init(r)     \
    do                    \
    {                     \
        (r)->m_state = 0; \
    }                     \
    MZ_MACRO_END
#define tinfl_get_adler32(r) (r)->m_check_adler32

tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *pIn_buf_next, size_t *pIn_buf_size, mz_uint8 *pOut_buf_start, mz_uint8 *pOut_buf_next, size_t *pOut_buf_size, const mz_uint32 decomp_flags);

#endif /* __QCOW2_INFLATE_H__ */