#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if __has_include(<zstd.h>)
#include <zstd.h>
#define HAVE_ZSTD 1
#else
#define HAVE_ZSTD 0
#endif

/* Simple LZ4 implementation (public domain algorithm) */
#define LZ4_MAX_INPUT_SIZE 0x7E000000
#define LZ4_MEM_COMPRESS   (64 * 1024)
#define LZ4_ACCELERATION_DEFAULT 1

static int lz4_compress(const char *src, size_t src_len, char *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len) return -1;

    /* Min match: 4 bytes. We use a simple hash-based approach. */
    int acceleration = LZ4_ACCELERATION_DEFAULT;
    const uint8_t *ip = (const uint8_t *)src;
    const uint8_t * const iend = ip + src_len;
    const uint8_t *ref;
    uint8_t *op = (uint8_t *)dst;
    const uint8_t *anchor = ip;

    if (src_len < 4) {
        *dst_len = src_len + 1;
        op[0] = (uint8_t)(src_len << 4); /* literal run */
        memcpy(op + 1, src, src_len);
        return 0;
    }

    /* Store original size for decompression */
    memcpy(op, &src_len, sizeof(size_t));
    op += sizeof(size_t);

    uint32_t hash_table[4096];
    memset(hash_table, 0, sizeof(hash_table));

    ip++;

    while (1) {
        const uint8_t *forward_ip = ip;
        size_t step = 1;
        size_t search_match = 0;
        size_t match_len = 0;
        int skip = 0;

        /* Hash 4 bytes */
        uint32_t h = ((uint32_t)forward_ip[0] * 2654435761U) ^
                     ((uint32_t)forward_ip[1] * 2246822519U) ^
                     ((uint32_t)forward_ip[2] * 3266489917U) ^
                     ((uint32_t)forward_ip[3] * 668265263U);
        h = (h >> 5) & 0xFFF;
        ref = (const uint8_t *)src + hash_table[h];
        hash_table[h] = (uint32_t)(forward_ip - (const uint8_t *)src);

        if (ref < anchor || ref + 4 > iend || memcmp(ref, forward_ip, 4)) {
            ip += (++skip);
            continue;
        }

        /* Found match */
        size_t lit_len = forward_ip - anchor;
        if (lit_len > 15) {
            op[0] = 240 | ((lit_len - 15) & 0x0F);
            memcpy(op + 1, anchor, 15);
            size_t extra = lit_len - 15;
            memcpy(op + 16, anchor + 15, extra);
            op += 16 + extra;
        } else {
            op[0] = (uint8_t)(lit_len << 4);
            memcpy(op + 1, anchor, lit_len);
            op += 1 + lit_len;
        }

        /* Match offset */
        size_t match_dist = forward_ip - ref;
        match_len = 4;
        while (forward_ip + match_len < iend && ref + match_len < iend &&
               forward_ip[match_len] == ref[match_len])
            match_len++;
        if (match_len > 18) match_len = 18;

        op[0] = (uint8_t)(match_dist & 0xFF);
        op[1] = (uint8_t)((match_dist >> 8) & 0xFF);
        op[2] = (uint8_t)(((match_len - 4) << 4) & 0xF0);
        op += 3;

        ip = forward_ip + match_len;
        anchor = ip;
        if (ip >= iend - 4) break;
        ip++;
    }

    /* Last literal */
    size_t lit_len = iend - anchor;
    if (lit_len > 0) {
        if (lit_len > 15) {
            op[0] = 240 | ((lit_len - 15) & 0x0F);
            memcpy(op + 1, anchor, 15);
            size_t extra = lit_len - 15;
            memcpy(op + 16, anchor + 15, extra);
            op += 16 + extra;
        } else {
            op[0] = (uint8_t)(lit_len << 4);
            memcpy(op + 1, anchor, lit_len);
            op += 1 + lit_len;
        }
    }

    *dst_len = (size_t)(op - (uint8_t *)dst);
    return 0;
}

static int lz4_decompress(const char *src, size_t src_len, char *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len || src_len < sizeof(size_t) + 1)
        return -1;

    size_t orig_size;
    memcpy(&orig_size, src, sizeof(size_t));
    if (orig_size > *dst_len) return -1;

    const uint8_t *ip = (const uint8_t *)src + sizeof(size_t);
    const uint8_t * const iend = (const uint8_t *)src + src_len;
    uint8_t *op = (uint8_t *)dst;
    const uint8_t * const oend = op + orig_size;

    while (ip < iend && op < oend) {
        uint8_t token = *ip++;
        size_t lit_len = (token >> 4) & 0x0F;
        if (lit_len == 15) {
            uint8_t s;
            do {
                if (ip >= iend) return -1;
                s = *ip++;
                lit_len += s;
            } while (s == 255);
        }

        if (ip + lit_len > iend || op + lit_len > oend) return -1;
        memcpy(op, ip, lit_len);
        ip += lit_len;
        op += lit_len;

        if (ip + 2 > iend) break;

        uint16_t match_dist = ip[0] | ((uint16_t)ip[1] << 8);
        ip += 2;

        size_t match_len = (token & 0x0F) + 4;
        if ((token & 0x0F) == 15) {
            uint8_t s;
            do {
                if (ip >= iend) return -1;
                s = *ip++;
                match_len += s;
            } while (s == 255);
        }

        uint8_t *match = op - match_dist;
        if (match + match_len > oend || match < dst) return -1;

        for (size_t i = 0; i < match_len; i++)
            op[i] = match[i];
        op += match_len;
    }

    *dst_len = (size_t)(op - (uint8_t *)dst);
    return 0;
}

/* Simple DEFLATE-like implementation */
static int deflate_compress(const uint8_t *in, size_t in_len,
                              uint8_t *out, size_t *out_len)
{
    /* Store with simple RLE + uncompressed fallback */
    if (in_len < 64) {
        /* Store uncompressed with header */
        out[0] = 0x01; /* uncompressed block */
        memcpy(out + 1, &in_len, sizeof(size_t));
        memcpy(out + 1 + sizeof(size_t), in, in_len);
        *out_len = 1 + sizeof(size_t) + in_len;
        return 0;
    }

    /* Simple RLE compression */
    uint8_t *op = out;
    *op++ = 0x02; /* compressed block */
    memcpy(op, &in_len, sizeof(size_t));
    op += sizeof(size_t);

    size_t i = 0;
    while (i < in_len) {
        uint8_t run_byte = in[i];
        size_t run_len = 1;
        while (i + run_len < in_len && in[i + run_len] == run_byte && run_len < 255)
            run_len++;

        if (run_len >= 4) {
            /* Run length encoding */
            *op++ = 0x80 | (uint8_t)run_len;
            *op++ = run_byte;
            i += run_len;
        } else {
            /* Literal */
            *op++ = run_byte;
            i++;
        }
    }

    *out_len = (size_t)(op - out);
    return 0;
}

static int deflate_decompress(const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t *out_len)
{
    if (in_len < 1) return -1;

    if (in[0] == 0x01) {
        /* Uncompressed */
        size_t orig_size;
        memcpy(&orig_size, in + 1, sizeof(size_t));
        if (1 + sizeof(size_t) + orig_size > in_len) return -1;
        memcpy(out, in + 1 + sizeof(size_t), orig_size);
        *out_len = orig_size;
        return 0;
    }

    if (in[0] == 0x02) {
        /* RLE compressed */
        size_t orig_size;
        memcpy(&orig_size, in + 1, sizeof(size_t));
        if (orig_size > *out_len) return -1;

        size_t i = 1 + sizeof(size_t);
        size_t o = 0;
        while (i < in_len && o < orig_size) {
            uint8_t b = in[i++];
            if (b & 0x80) {
                /* Run */
                size_t run_len = b & 0x7F;
                uint8_t run_byte = in[i++];
                for (size_t j = 0; j < run_len && o < orig_size; j++)
                    out[o++] = run_byte;
            } else {
                out[o++] = b;
            }
        }
        *out_len = o;
        return 0;
    }

    return -1;
}

int spermfs_compress(const void *in, size_t in_len, void *out, size_t *out_len,
                       int algo)
{
    if (!in || !out || !out_len) return SPERMAFS_ERR_INVAL;

    switch (algo) {
    case SPERMAFS_COMPRESS_LZ4:
        return lz4_compress(in, in_len, out, out_len);

    case SPERMAFS_COMPRESS_ZSTD:
#if HAVE_ZSTD
        {
            size_t rc = ZSTD_compress(out, *out_len, in, in_len, 3);
            if (ZSTD_isError(rc)) return SPERMAFS_ERR_COMPRESS;
            *out_len = rc;
            return SPERMAFS_OK;
        }
#else
        return deflate_compress(in, in_len, out, out_len);
#endif

    case SPERMAFS_COMPRESS_DEFLATE:
        return deflate_compress(in, in_len, out, out_len);

    default:
        return SPERMAFS_ERR_COMPRESS;
    }
}

int spermfs_decompress(const void *in, size_t in_len, void *out, size_t *out_len,
                         int algo)
{
    if (!in || !out || !out_len) return SPERMAFS_ERR_INVAL;

    switch (algo) {
    case SPERMAFS_COMPRESS_LZ4:
        return lz4_decompress(in, in_len, out, out_len);

    case SPERMAFS_COMPRESS_ZSTD:
#if HAVE_ZSTD
        {
            size_t rc = ZSTD_decompress(out, *out_len, in, in_len);
            if (ZSTD_isError(rc)) return SPERMAFS_ERR_COMPRESS;
            *out_len = rc;
            return SPERMAFS_OK;
        }
#else
        return deflate_decompress(in, in_len, out, out_len);
#endif

    case SPERMAFS_COMPRESS_DEFLATE:
        return deflate_decompress(in, in_len, out, out_len);

    default:
        return SPERMAFS_ERR_COMPRESS;
    }
}

const char *spermfs_compress_name(int algo)
{
    switch (algo) {
    case SPERMAFS_COMPRESS_NONE:    return "none";
    case SPERMAFS_COMPRESS_LZ4:     return "LZ4";
    case SPERMAFS_COMPRESS_ZSTD:    return "ZSTD";
    case SPERMAFS_COMPRESS_DEFLATE:  return "DEFLATE";
    default: return "unknown";
    }
}
