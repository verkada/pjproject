/**
 * Parses H.264 NAL units
 *
 * Copyright 2021 Verkada, Inc. All rights reserved.
 */

#ifndef NALPARSE_H
#define NALPARSE_H

#define NAL_TYPE_NONIDR_CODED_SLICE (1)
#define NAL_TYPE_IDR_CODED_SLICE    (5)
#define NAL_TYPE_SEI                (6)
#define NAL_TYPE_SPS                (7)
#define NAL_TYPE_MASK               (0x1F)

#define NAL_TYPE_SEI_CUSTOM_TIMING  (100)

struct nal_parser;

struct nal_parser* nal_parser_create(int src);

int nal_parser_next_nalu(struct nal_parser* parser, unsigned char* dst, size_t cap, int* done);

void nal_parser_destroy(struct nal_parser* parser);

uint64_t get_timestamp_from_sei(uint8_t *buf, int frameSize);

#endif
