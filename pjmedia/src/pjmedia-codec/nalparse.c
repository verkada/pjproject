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

// NOTE: This file is copied from https://github.com/verkada/camera-firmware/blob/next/verkada/camera/vcamera/rtspd/utils/nalparse.cpp
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
                // NOTE: INTERCOM HACK; We need NAL unit headers also which current code skips,
                // so copying extra 4 bytes with hack and not changing the core logic of parser code
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
            if (0 == m) {
                // socket got closed unexpectedly, treat as error
                return -1;
            } else if (-1 == m) {
                // ignore EAGAIN errors, otherwise it's a legit error
                if (EAGAIN == errno) {
                    continue;
                }
                return -1;
            } else {
                // successfully read m bytes
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
        if (parser->fd) {
            close(parser->fd);
        }
        free(parser);
    }
}

/*
    * Returns timestamp from SEI
    *
    * \param[in] buf Pointer to buffer containing SEI
    * \param[in] frameSize Size of frame
    *
    * \return Timestamp in microseconds
    NOTE: This code is copied from https://github.com/verkada/camera-firmware/blob/90db0a40aa52d752768facd61523457581154baa/verkada/camera/vcamera/rtspd/liveMedia/H264StreamSource.cpp#L81
*/
uint64_t get_timestamp_from_sei(uint8_t *buf, int frameSize)
{
    int type = buf[4] & NAL_TYPE_MASK;
    int payloadType = 0;
    uint64_t timestamp = 0;
    if (type == NAL_TYPE_SEI)
    {
        int i = 5;
        while ((u_int8_t)buf[i] == 255 && i < frameSize)
        {
            payloadType += 255;
            i++;
        }
        payloadType += buf[i];

        i++;
        while ((u_int8_t)buf[i] == 255 && i < frameSize)
        {
            i++;
        }
        i++;

        if (payloadType == NAL_TYPE_SEI_CUSTOM_TIMING)
        {
            const char currTime[8] = {buf[i + 10], buf[i + 9], buf[i + 7], buf[i + 6], buf[i + 4], buf[i + 3], buf[i + 1], buf[i]};
            timestamp = *(u_int64_t *)currTime;
        }
    }
    return timestamp;
}

