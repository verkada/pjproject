/**
 * Parses H.264 NAL units
 *
 * Copyright 2021 Verkada, Inc. All rights reserved.
 */

#include <stdio.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/errno.h>

#include "nalparse.h"

enum nal_parser_state {
    HAVE_NOTHING = 0,
    HAVE_LEFT_00,
    HAVE_LEFT_00_00,
    HAVE_LEFT_00_00_01,
    HAVE_RIGHT_00,
    HAVE_RIGHT_00_00,
    HAVE_RIGHT_00_00_01
};

struct nal_parser {
    int fd; // byte stream source file descriptor

    enum nal_parser_state state;

    unsigned char* buf;
    size_t cap; // capacity of buffer
    size_t pagesize; // system memory page size

    unsigned char* curr; // current parse point
    unsigned char* end; // end of buffered data
    unsigned char* left; // left prefix code

    uint8_t left_code_zeros;
    uint8_t right_code_zeros;

    bool have_media_frame;
};

/**
 * Returns lesser of two integers
 */
inline static int imin(int a, int b) { return a < b ? a : b; }

/**
 * Instantiates new H.264 NAL unit parser
 *
 * \param[in] src Source file descriptor.
 *
 * \return Pointer to new parser instance. NULL if insufficient memory.
 */
struct nal_parser* nal_parser_create(int src)
{
    long pagesize;
    struct nal_parser* parser;

    pagesize = sysconf(_SC_PAGESIZE);
    if (-1 == pagesize) {
        return NULL;
    }

    parser = (struct nal_parser*)calloc(1, sizeof(struct nal_parser));
    if (parser) {
        parser->fd = src;
        parser->buf = parser->curr = parser->end = (unsigned char*)calloc(1, pagesize);
        if (parser->buf) {
            parser->state = HAVE_NOTHING;
            parser->cap = (size_t)pagesize;
            parser->pagesize = (size_t)pagesize;
            parser->left = NULL;
            parser->have_media_frame = false;
        } else {
            free(parser);
            return NULL;
        }
    }

    return parser;
}

/**
 * Parses and returns next H.264 NAL unit from byte stream
 *
 * Reads pages from a previously opened source file descriptor into an
 * internal buffer, growing buffer as necessary. parser->left
 * is used to find the left start prefix codes denoting NAL
 * unit boundaries.
 *
 * \param[in] parser Pointer to parser instance to free.
 * \param[out] dst Pointer to destination buffer.
 * \param[in] cap Capacity of destination buffer.
 *
 * \return Size of parsed frame, 0 if no frame parsed yet. Negative on error.
 */
int nal_parser_next_nalu(struct nal_parser* parser, unsigned char* dst, size_t cap, int* done)
{
    int n = 0;
    unsigned char* right;

    while (!*done && !n) {
        if (parser->curr < parser->end) {
            switch (parser->state) {
            case HAVE_NOTHING:
                parser->left_code_zeros = 0;
                if (0 == *parser->curr) {
                    parser->state = HAVE_LEFT_00;
                }
                parser->curr++;
                break;

            case HAVE_LEFT_00:
                parser->left_code_zeros += 1;
                if (0 == *parser->curr) {
                    parser->state = HAVE_LEFT_00_00;
                } else {
                    parser->state = HAVE_NOTHING;
                }
                parser->curr++;
                break;

            case HAVE_LEFT_00_00:
                parser->left_code_zeros += 1;
                if (1 == *parser->curr) {
                    parser->left = parser->curr + 1; // exclude start code
                    parser->state = HAVE_LEFT_00_00_01;
                } else if (0 != *parser->curr) {
                   	parser->state = HAVE_NOTHING;
                }
                parser->curr++;
                break;

            case HAVE_LEFT_00_00_01:
                parser->right_code_zeros = 0;
                if (0 == *parser->curr) {
                    parser->state = HAVE_RIGHT_00;
                }
                parser->curr++;
                break;

            case HAVE_RIGHT_00:
                parser->right_code_zeros += 1;
                if (0 == *parser->curr) {
                    parser->state = HAVE_RIGHT_00_00;
                } else {
                    parser->state = HAVE_LEFT_00_00_01;
                }
                parser->curr++;
                break;

            case HAVE_RIGHT_00_00:
                parser->right_code_zeros += 1;
                if (1 == *parser->curr) {
                    parser->state = HAVE_RIGHT_00_00_01;
                } else if (0 != *parser->curr) {
                    parser->state = HAVE_LEFT_00_00_01;
                }
                parser->curr++;
                break;

            case HAVE_RIGHT_00_00_01:
                right = parser->curr - imin(parser->right_code_zeros, 3) - 1;
                n = imin(right - parser->left, cap);
                // copy frame to destination
                // DDD Patch to accommodate 4 byte start code
                memcpy(dst, parser->left - 4, n + 4);

                if (n < right - parser->left) {
                    parser->left += n;
                } else {
                    // discard frame (shift unparsed data)
                    memcpy(parser->buf, right, parser->end - right);
                    parser->end = parser->buf + (parser->end - right);
                    parser->curr = parser->buf;
                    parser->left = NULL;

                    parser->state = HAVE_NOTHING;
                }

                break;
            }
        } else {
            // double capacity if needed
            if (parser->cap - (parser->end - parser->buf) < parser->pagesize) {
                unsigned char* tmp = parser->buf;
                parser->buf = (unsigned char*) realloc(parser->buf, 2 * parser->cap);
                if (!parser->buf) {
                    return -1;
                }
                parser->cap *= 2;

                if (parser->left) {
                    parser->left = parser->buf + (parser->left - tmp);
                }
                if (parser->curr) {
                    parser->curr = parser->buf + (parser->curr - tmp);
                }
                if (parser->end) {
                    parser->end = parser->buf + (parser->end - tmp);
                }
            }

            // read more data
            int m = read(parser->fd, parser->end, parser->pagesize);
            if (-1 == m) {
                return 0;
            }
            if (m > 0) {
                parser->end += m;
            }
        }
    }

    return n;
}

/**
 * Frees parser instance.
 *
 * \param[in] parser Pointer to parser instance to free.
 */
void nal_parser_destroy(struct nal_parser* parser)
{
    if (parser) {
        if (parser->buf) {
            free(parser->buf);
        }
        free(parser);
    }
}