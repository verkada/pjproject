/*
 * Copyright (C) 2010-2011 Teluu Inc. (http://www.teluu.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <pjmedia-codec/ffmpeg_vid_codecs.h>
#include <pjmedia-codec/h264_packetizer.h>
#include <pjmedia/errno.h>
#include <pjmedia/vid_codec_util.h>
#include <pj/assert.h>
#include <pj/list.h>
#include <pj/log.h>
#include <pj/math.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/os.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "nalparse.h"
/*
 * Only build this file if PJMEDIA_HAS_FFMPEG_VID_CODEC != 0 and
 * PJMEDIA_HAS_VIDEO != 0
 */
#if defined(PJMEDIA_HAS_FFMPEG_VID_CODEC) && \
    PJMEDIA_HAS_FFMPEG_VID_CODEC != 0 &&     \
    defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

#define THIS_FILE "ffmpeg_vid_codecs.c"

#include "../pjmedia/ffmpeg_util.h"

/* AVCodec H264 default PT */
#define AVC_H264_PT PJMEDIA_RTP_PT_H264_RSV3

#define INTERCOM_SUBSYSTEM_VSTREAM_VIDEO_PORT 20002

/* Prototypes for FFMPEG codecs factory */
static pj_status_t ffmpeg_test_alloc(pjmedia_vid_codec_factory *factory,
                                     const pjmedia_vid_codec_info *id);
static pj_status_t ffmpeg_default_attr(pjmedia_vid_codec_factory *factory,
                                       const pjmedia_vid_codec_info *info,
                                       pjmedia_vid_codec_param *attr);
static pj_status_t ffmpeg_enum_codecs(pjmedia_vid_codec_factory *factory,
                                      unsigned *count,
                                      pjmedia_vid_codec_info codecs[]);
static pj_status_t ffmpeg_alloc_codec(pjmedia_vid_codec_factory *factory,
                                      const pjmedia_vid_codec_info *info,
                                      pjmedia_vid_codec **p_codec);
static pj_status_t ffmpeg_dealloc_codec(pjmedia_vid_codec_factory *factory,
                                        pjmedia_vid_codec *codec);

/* Prototypes for FFMPEG codecs implementation. */
static pj_status_t ffmpeg_codec_init(pjmedia_vid_codec *codec,
                                     pj_pool_t *pool);
static pj_status_t ffmpeg_codec_open(pjmedia_vid_codec *codec,
                                     pjmedia_vid_codec_param *attr);
static pj_status_t ffmpeg_codec_close(pjmedia_vid_codec *codec);
static pj_status_t ffmpeg_codec_modify(pjmedia_vid_codec *codec,
                                       const pjmedia_vid_codec_param *attr);
static pj_status_t ffmpeg_codec_get_param(pjmedia_vid_codec *codec,
                                          pjmedia_vid_codec_param *param);
static pj_status_t ffmpeg_codec_encode_begin(pjmedia_vid_codec *codec,
                                             const pjmedia_vid_encode_opt *opt,
                                             const pjmedia_frame *input,
                                             unsigned out_size,
                                             pjmedia_frame *output,
                                             pj_bool_t *has_more);
static pj_status_t ffmpeg_codec_encode_more(pjmedia_vid_codec *codec,
                                            unsigned out_size,
                                            pjmedia_frame *output,
                                            pj_bool_t *has_more);

/* Definition for FFMPEG codecs operations. */
static pjmedia_vid_codec_op ffmpeg_op =
    {
        &ffmpeg_codec_init,
        &ffmpeg_codec_open,
        &ffmpeg_codec_close,
        &ffmpeg_codec_modify,
        &ffmpeg_codec_get_param,
        &ffmpeg_codec_encode_begin,
        &ffmpeg_codec_encode_more,
        NULL};

/* Definition for FFMPEG codecs factory operations. */
static pjmedia_vid_codec_factory_op ffmpeg_factory_op =
    {
        &ffmpeg_test_alloc,
        &ffmpeg_default_attr,
        &ffmpeg_enum_codecs,
        &ffmpeg_alloc_codec,
        &ffmpeg_dealloc_codec};

/* FFMPEG codecs factory */
static struct ffmpeg_factory
{
    pjmedia_vid_codec_factory base;
    pjmedia_vid_codec_mgr *mgr;
    pj_pool_factory *pf;
    pj_pool_t *pool;
    pj_mutex_t *mutex;
} ffmpeg_factory;

typedef struct ffmpeg_codec_desc ffmpeg_codec_desc;

/* FFMPEG codecs private data. */
typedef struct ffmpeg_private
{
    const ffmpeg_codec_desc *desc;
    pjmedia_vid_codec_param *param; /**< Codec param            */
    pj_pool_t *pool;                /**< Pool for each instance */

    /* Format info and apply format param */
    const pjmedia_video_format_info *enc_vfi;
    pjmedia_video_apply_fmt_param enc_vafp;

    /* Buffers, only needed for multi-packets */
    pj_bool_t whole;
    void *enc_buf;
    unsigned enc_buf_size;
    pj_bool_t enc_buf_is_keyframe;
    unsigned enc_frame_len;
    unsigned enc_processed;

    /* The ffmpeg decoder cannot set the output format, so format conversion
     * may be needed for post-decoding.
     */
    enum AVPixelFormat expected_dec_fmt;
    /**< Expected output format of
         ffmpeg decoder         */

    void *data; /**< Codec specific data    */
    struct nal_parser *parser;
    uint64_t last_pts;

    pj_bool_t first_idr_sent;

} ffmpeg_private;

/* Shortcuts for packetize & unpacketize function declaration,
 * as it has long params and is reused many times!
 */
#define FUNC_PACKETIZE(name)                                       \
    pj_status_t(name)(ffmpeg_private * ff, pj_uint8_t * bits,      \
                      pj_size_t bits_len, unsigned *bits_pos,      \
                      pj_uint8_t *payload, pj_size_t *payload_len, \
                      pj_bool_t is_keyframe)

#define FUNC_FMT_MATCH(name)                                         \
    pj_status_t(name)(pj_pool_t * pool,                              \
                      pjmedia_sdp_media * offer, unsigned o_fmt_idx, \
                      pjmedia_sdp_media *answer, unsigned a_fmt_idx, \
                      unsigned option)

/* Type definition of codec specific functions */
typedef FUNC_PACKETIZE(*func_packetize);
typedef pj_status_t (*func_preopen)(ffmpeg_private *ff);
typedef pj_status_t (*func_postopen)(ffmpeg_private *ff);
typedef FUNC_FMT_MATCH(*func_sdp_fmt_match);

/* FFMPEG codec info */
struct ffmpeg_codec_desc
{
    /* Predefined info */
    pjmedia_vid_codec_info info;
    pjmedia_format_id base_fmt_id; /**< Some codecs may be exactly
                                        same or compatible with
                                        another codec, base format
                                        will tell the initializer
                                        to copy this codec desc
                                        from its base format   */
    pjmedia_rect_size size;
    pjmedia_ratio fps;
    pj_uint32_t avg_bps;
    pj_uint32_t max_bps;
    func_packetize packetize;
    func_preopen preopen;
    func_preopen postopen;
    func_sdp_fmt_match sdp_fmt_match;
    pjmedia_codec_fmtp dec_fmtp;

    /* Init time defined info */
    pj_bool_t enabled;
    const AVCodec *enc;
};

#if PJMEDIA_HAS_FFMPEG_CODEC_H264
static pj_status_t h264_preopen(ffmpeg_private *ff);
static pj_status_t h264_postopen(ffmpeg_private *ff);
static FUNC_PACKETIZE(h264_packetize);
#endif

/* Internal codec info */
static ffmpeg_codec_desc codec_desc[] =
    {
#if PJMEDIA_HAS_FFMPEG_CODEC_H264
        {
            {PJMEDIA_FORMAT_H264, AVC_H264_PT, {"H264", 4}, {"Constrained Baseline (level=30, pack=1)", 39}},
            0,
            {720, 480},
            {15, 1},
            256000,
            256000,
            &h264_packetize,
            &h264_preopen,
            &h264_postopen,
            &pjmedia_vid_codec_h264_match_sdp,
            /* Leading space for better compatibility (strange indeed!) */
            {2, {
                    {{"profile-level-id", 16}, {"42e01e", 6}},
                    {{" packetization-mode", 19}, {"1", 1}},
                }},
        },
#endif
};

#if PJMEDIA_HAS_FFMPEG_CODEC_H264

typedef struct h264_data
{
    pjmedia_vid_codec_h264_fmtp fmtp;
    pjmedia_h264_packetizer *pktz;
} h264_data;

static pj_status_t h264_preopen(ffmpeg_private *ff)
{
    h264_data *data;
    pjmedia_h264_packetizer_cfg pktz_cfg;
    pj_status_t status;

    data = PJ_POOL_ZALLOC_T(ff->pool, h264_data);
    ff->data = data;
    /* Create packetizer */
    pktz_cfg.mtu = ff->param->enc_mtu; // 1304
    pktz_cfg.unpack_nal_start = 0;
    /* Better always send in single NAL mode for better compatibility */
    pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED; // PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL;

    status = pjmedia_h264_packetizer_create(ff->pool, &pktz_cfg, &data->pktz);
    if (status != PJ_SUCCESS)
        return status;

    return PJ_SUCCESS;
}

static pj_status_t h264_postopen(ffmpeg_private *ff)
{
    h264_data *data = (h264_data *)ff->data;
    PJ_UNUSED_ARG(data);
    return PJ_SUCCESS;
}

static FUNC_PACKETIZE(h264_packetize)
{
    PJ_UNUSED_ARG(is_keyframe);
    h264_data *data = (h264_data *)ff->data;
    pj_status_t status;
    pj_uint8_t *outbuf = payload;
    pj_size_t out_size = *payload_len;
    status = pjmedia_h264_packetize(data->pktz, bits, bits_len, bits_pos,
                                    (const pj_uint8_t **)&payload, payload_len);
    if (status != PJ_SUCCESS)
        return status;
    if (out_size < *payload_len)
        return PJMEDIA_CODEC_EFRMTOOSHORT;
    pj_memcpy(outbuf, payload, *payload_len);
    return PJ_SUCCESS;
}

#endif /* PJMEDIA_HAS_FFMPEG_CODEC_H264 */

static ffmpeg_codec_desc *find_codec_desc_by_info(
    const pjmedia_vid_codec_info *info)
{
    unsigned i;

    for (i = 0; i < PJ_ARRAY_SIZE(codec_desc); ++i)
    {
        ffmpeg_codec_desc *desc = &codec_desc[i];

        if (desc->enabled &&
            (desc->info.fmt_id == info->fmt_id) &&
            ((desc->info.dir & info->dir) == info->dir) &&
            (desc->info.pt == info->pt) &&
            (desc->info.packings & info->packings))
        {
            return desc;
        }
    }

    return NULL;
}

static void init_codec(pj_bool_t is_encoder,
                       pj_bool_t is_decoder)
{
    ffmpeg_codec_desc *desc;

    desc = &codec_desc[0];

    /* Get raw/decoded format ids in the encoder */
    pjmedia_format_id raw_fmt[PJMEDIA_VID_CODEC_MAX_DEC_FMT_CNT] = {PJMEDIA_FORMAT_I420, PJMEDIA_FORMAT_NV12, PJMEDIA_FORMAT_I422};
    unsigned raw_fmt_cnt = sizeof(raw_fmt) / sizeof(raw_fmt[0]);

    desc->info.dec_fmt_id_cnt = raw_fmt_cnt;
    pj_memcpy(desc->info.dec_fmt_id, raw_fmt,
              sizeof(raw_fmt[0]) * raw_fmt_cnt);

    desc->info.dir |= PJMEDIA_DIR_ENCODING;

    desc->enabled = PJ_TRUE;
    desc->info.clock_rate = 90000;
    desc->info.packings |= (PJMEDIA_VID_PACKING_WHOLE | PJMEDIA_VID_PACKING_PACKETS);
}

/*
 * Initialize and register FFMPEG codec factory to pjmedia endpoint.
 */
PJ_DEF(pj_status_t)
pjmedia_codec_intercom_vid_init(pjmedia_vid_codec_mgr *mgr,
                               pj_pool_factory *pf)
{
    pj_pool_t *pool;
    pj_status_t status;
    unsigned i;

    if (ffmpeg_factory.pool != NULL)
    {
        /* Already initialized. */
        return PJ_SUCCESS;
    }

    if (!mgr)
        mgr = pjmedia_vid_codec_mgr_instance();
    PJ_ASSERT_RETURN(mgr, PJ_EINVAL);

    /* Create FFMPEG codec factory. */
    ffmpeg_factory.base.op = &ffmpeg_factory_op;
    ffmpeg_factory.base.factory_data = NULL;
    ffmpeg_factory.mgr = mgr;
    ffmpeg_factory.pf = pf;

    pool = pj_pool_create(pf, "ffmpeg codec factory", 256, 256, NULL);
    if (!pool)
        return PJ_ENOMEM;

    /* Create mutex. */
    status = pj_mutex_create_simple(pool, "ffmpeg codec factory",
                                    &ffmpeg_factory.mutex);
    if (status != PJ_SUCCESS)
        goto on_error;

    pjmedia_ffmpeg_add_ref();
    init_codec(PJ_TRUE, PJ_FALSE);

    /* Review all codecs for applying base format, registering format match for
     * SDP negotiation, etc.
     */
    for (i = 0; i < PJ_ARRAY_SIZE(codec_desc); ++i)
    {
        ffmpeg_codec_desc *desc = &codec_desc[i];

        desc->info.packings |= (PJMEDIA_VID_PACKING_WHOLE | PJMEDIA_VID_PACKING_PACKETS);
        desc->enabled = PJ_TRUE;
        desc->info.clock_rate = 90000;
        status = pjmedia_sdp_neg_register_fmt_match_cb(
            &desc->info.encoding_name,
            desc->sdp_fmt_match);
        pj_assert(status == PJ_SUCCESS);
    }

    /* Register codec factory to codec manager. */
    status = pjmedia_vid_codec_mgr_register_factory(mgr,
                                                    &ffmpeg_factory.base);
    if (status != PJ_SUCCESS)
        goto on_error;

    ffmpeg_factory.pool = pool;

    /* Done. */
    return PJ_SUCCESS;

on_error:
    pj_pool_release(pool);
    return status;
}

/*
 * Unregister FFMPEG codecs factory from pjmedia endpoint.
 */
PJ_DEF(pj_status_t)
pjmedia_codec_intercom_vid_deinit(void)
{
    pj_status_t status = PJ_SUCCESS;

    if (ffmpeg_factory.pool == NULL)
    {
        /* Already deinitialized */
        return PJ_SUCCESS;
    }

    pj_mutex_lock(ffmpeg_factory.mutex);

    /* Unregister FFMPEG codecs factory. */
    status = pjmedia_vid_codec_mgr_unregister_factory(ffmpeg_factory.mgr,
                                                      &ffmpeg_factory.base);

    /* Destroy mutex. */
    pj_mutex_unlock(ffmpeg_factory.mutex);
    pj_mutex_destroy(ffmpeg_factory.mutex);
    ffmpeg_factory.mutex = NULL;

    /* Destroy pool. */
    pj_pool_release(ffmpeg_factory.pool);
    ffmpeg_factory.pool = NULL;

    pjmedia_ffmpeg_dec_ref();

    return status;
}

/*
 * Check if factory can allocate the specified codec.
 */
static pj_status_t ffmpeg_test_alloc(pjmedia_vid_codec_factory *factory,
                                     const pjmedia_vid_codec_info *info)
{
    const ffmpeg_codec_desc *desc;

    PJ_ASSERT_RETURN(factory == &ffmpeg_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc)
    {
        return PJMEDIA_CODEC_EUNSUP;
    }

    return PJ_SUCCESS;
}

/*
 * Generate default attribute.
 */
static pj_status_t ffmpeg_default_attr(pjmedia_vid_codec_factory *factory,
                                       const pjmedia_vid_codec_info *info,
                                       pjmedia_vid_codec_param *attr)
{
    ffmpeg_codec_desc *desc;

    PJ_ASSERT_RETURN(factory == &ffmpeg_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info && attr, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc)
    {
        return PJMEDIA_CODEC_EUNSUP;
    }

    pj_bzero(attr, sizeof(pjmedia_vid_codec_param));

    /* Scan the requested packings and use the lowest number */
    attr->packing = PJMEDIA_VID_PACKING_PACKETS;

    /* Direction */
    attr->dir = desc->info.dir;
    desc->fps.num = 60;
    /* Encoded format */
    pjmedia_format_init_video(&attr->enc_fmt, desc->info.fmt_id,
                              desc->size.w, desc->size.h,
                              desc->fps.num, desc->fps.denum);

    /* Decoded format */
    pjmedia_format_init_video(&attr->dec_fmt, PJMEDIA_FORMAT_I420, // desc->info.dec_fmt_id[0], // PJMEDIA_FORMAT_I420
                              desc->size.w, desc->size.h,
                              desc->fps.num, desc->fps.denum);

    /* Decoding fmtp */
    attr->dec_fmtp = desc->dec_fmtp;

    /* Bitrate */
    attr->enc_fmt.det.vid.avg_bps = desc->avg_bps;
    attr->enc_fmt.det.vid.max_bps = desc->max_bps;

    /* Encoding MTU */
    attr->enc_mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

    return PJ_SUCCESS;
}

/*
 * Enum codecs supported by this factory.
 */
static pj_status_t ffmpeg_enum_codecs(pjmedia_vid_codec_factory *factory,
                                      unsigned *count,
                                      pjmedia_vid_codec_info codecs[])
{
    unsigned i, max_cnt;

    PJ_ASSERT_RETURN(codecs && *count > 0, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &ffmpeg_factory.base, PJ_EINVAL);

    max_cnt = PJ_MIN(*count, PJ_ARRAY_SIZE(codec_desc));
    *count = 0;

    for (i = 0; i < max_cnt; ++i)
    {
        if (codec_desc[i].enabled)
        {
            pj_memcpy(&codecs[*count], &codec_desc[i].info,
                      sizeof(pjmedia_vid_codec_info));
            (*count)++;
        }
    }

    return PJ_SUCCESS;
}

/*
 * Allocate a new codec instance.
 */
static pj_status_t ffmpeg_alloc_codec(pjmedia_vid_codec_factory *factory,
                                      const pjmedia_vid_codec_info *info,
                                      pjmedia_vid_codec **p_codec)
{
    ffmpeg_private *ff;
    const ffmpeg_codec_desc *desc;
    pjmedia_vid_codec *codec;
    pj_pool_t *pool = NULL;
    pj_status_t status = PJ_SUCCESS;

    PJ_ASSERT_RETURN(factory && info && p_codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &ffmpeg_factory.base, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc)
    {
        return PJMEDIA_CODEC_EUNSUP;
    }

    /* Create pool for codec instance */
    pool = pj_pool_create(ffmpeg_factory.pf, "ffmpeg codec", 512, 512, NULL);
    if (!pool)
    {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec = PJ_POOL_ZALLOC_T(pool, pjmedia_vid_codec);
    if (!codec)
    {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec->op = &ffmpeg_op;
    codec->factory = factory;
    ff = PJ_POOL_ZALLOC_T(pool, ffmpeg_private);
    if (!ff)
    {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec->codec_data = ff;
    ff->pool = pool;
    ff->desc = desc;

    *p_codec = codec;
    return PJ_SUCCESS;

on_error:
    if (pool)
        pj_pool_release(pool);
    return status;
}

/*
 * Free codec.
 */
static pj_status_t ffmpeg_dealloc_codec(pjmedia_vid_codec_factory *factory,
                                        pjmedia_vid_codec *codec)
{
    ffmpeg_private *ff;
    pj_pool_t *pool;

    PJ_ASSERT_RETURN(factory && codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &ffmpeg_factory.base, PJ_EINVAL);

    /* Close codec, if it's not closed. */
    ff = (ffmpeg_private *)codec->codec_data;
    pool = ff->pool;
    codec->codec_data = NULL;
    pj_pool_release(pool);

    return PJ_SUCCESS;
}

/*
 * Socket init
 * connect to intercom subsystem ( process handling camera capture and encoding ) using TCP port.
 * process sends video stream on multiple ports. ports of interest in this case is 20002 for Vstream
 */
static int socket_init()
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in servaddr;
    if (sockfd == -1)
    {
        PJ_LOG(3, (THIS_FILE, "message=\"socket creation failed... err: %s\"", strerror(errno)));
        return -1;
    }
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    servaddr.sin_port = htons(INTERCOM_SUBSYSTEM_VSTREAM_VIDEO_PORT);

    // This is purely for testing purposes to switch between SD and HD streams
    // Keeping this commented for sometime, eventually will be removed
    // FILE *fp = fopen("/tmp/stream_port", "r");
    // if (fp != NULL)
    // {
    //     PJ_LOG(3, (THIS_FILE, "failed to open /tmp/stream_port file, err: %s", strerror(errno)));
    //     char port[10];
    //     fgets(port, 10, fp);
    //     fclose(fp);
    //     servaddr.sin_port = htons(atoi(port));
    // }

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0)
    {
        PJ_LOG(3, (THIS_FILE, "connection with the server failed, port: %d, err: %s", servaddr.sin_port, strerror(errno)));
        return -1;
    }
    return sockfd;
}

/*
 * Init codec.
 */
static pj_status_t ffmpeg_codec_init(pjmedia_vid_codec *codec,
                                     pj_pool_t *pool)
{
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    ffmpeg_private *ff;
    ff = (ffmpeg_private *)codec->codec_data;
    int sockfd = socket_init();
    ff->parser = nal_parser_create(sockfd);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    int64_t timestamp_microseconds = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    ff->last_pts = timestamp_microseconds;
    ff->first_idr_sent = PJ_FALSE;
    return PJ_SUCCESS;
}

static pj_status_t open_ffmpeg_codec(ffmpeg_private *ff,
                                     pj_mutex_t *ff_mutex)
{
    enum AVPixelFormat pix_fmt;
    pj_status_t status;

    /* Get decoded pixel format */
    status = pjmedia_format_id_to_PixelFormat(ff->param->dec_fmt.id,
                                              &pix_fmt);
    if (status != PJ_SUCCESS)
        return status;
    ff->expected_dec_fmt = pix_fmt;

    /* Override generic params or apply specific params before opening
     * the codec.
     */
    if (ff->desc->preopen)
    {
        status = (*ff->desc->preopen)(ff);
        if (status != PJ_SUCCESS)
            goto on_error;
    }

    /* Let the codec apply specific params after the codec opened */
    if (ff->desc->postopen)
    {
        status = (*ff->desc->postopen)(ff);
        if (status != PJ_SUCCESS)
            goto on_error;
    }

    return PJ_SUCCESS;

on_error:
    return status;
}

/*
 * Open codec.
 */
static pj_status_t ffmpeg_codec_open(pjmedia_vid_codec *codec,
                                     pjmedia_vid_codec_param *attr)
{
    ffmpeg_private *ff;
    pj_status_t status;
    pj_mutex_t *ff_mutex;

    PJ_ASSERT_RETURN(codec && attr, PJ_EINVAL);
    ff = (ffmpeg_private *)codec->codec_data;

    ff->param = pjmedia_vid_codec_param_clone(ff->pool, attr);

    /* Normalize encoding MTU in codec param */
    if (ff->param->enc_mtu > PJMEDIA_MAX_VID_PAYLOAD_SIZE)
        ff->param->enc_mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

    /* Open the codec */
    ff_mutex = ((struct ffmpeg_factory *)codec->factory)->mutex;
    status = open_ffmpeg_codec(ff, ff_mutex);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Init format info and apply-param of encoder */
    ff->enc_vfi = pjmedia_get_video_format_info(NULL, ff->param->dec_fmt.id);
    if (!ff->enc_vfi)
    {
        status = PJ_EINVAL;
        goto on_error;
    }
    pj_bzero(&ff->enc_vafp, sizeof(ff->enc_vafp));
    ff->enc_vafp.size = ff->param->enc_fmt.det.vid.size;
    ff->enc_vafp.buffer = NULL;
    status = (*ff->enc_vfi->apply_fmt)(ff->enc_vfi, &ff->enc_vafp);
    if (status != PJ_SUCCESS)
    {
        goto on_error;
    }

    /* Alloc buffers if needed */
    ff->whole = (ff->param->packing == PJMEDIA_VID_PACKING_WHOLE);
    if (!ff->whole)
    {
        ff->enc_buf_size = (unsigned)ff->enc_vafp.framebytes;
        ff->enc_buf = pj_pool_alloc(ff->pool, ff->enc_buf_size);
    }

    /* Update codec attributes, e.g: encoding format may be changed by
     * SDP fmtp negotiation.
     */
    pj_memcpy(attr, ff->param, sizeof(*attr));

    return PJ_SUCCESS;

on_error:
    ffmpeg_codec_close(codec);
    return status;
}

/*
 * Close codec.
 */
static pj_status_t ffmpeg_codec_close(pjmedia_vid_codec *codec)
{
    ffmpeg_private *ff;
    pj_mutex_t *ff_mutex;

    PJ_ASSERT_RETURN(codec, PJ_EINVAL);
    ff = (ffmpeg_private *)codec->codec_data;
    ff_mutex = ((struct ffmpeg_factory *)codec->factory)->mutex;

    pj_mutex_lock(ff_mutex);
    pj_mutex_unlock(ff_mutex);

    nal_parser_destroy(ff->parser);

    return PJ_SUCCESS;
}

/*
 * Modify codec settings.
 */
static pj_status_t ffmpeg_codec_modify(pjmedia_vid_codec *codec,
                                       const pjmedia_vid_codec_param *attr)
{
    ffmpeg_private *ff = (ffmpeg_private *)codec->codec_data;

    PJ_UNUSED_ARG(attr);
    PJ_UNUSED_ARG(ff);

    return PJ_ENOTSUP;
}

static pj_status_t ffmpeg_codec_get_param(pjmedia_vid_codec *codec,
                                          pjmedia_vid_codec_param *param)
{
    ffmpeg_private *ff;

    PJ_ASSERT_RETURN(codec && param, PJ_EINVAL);

    ff = (ffmpeg_private *)codec->codec_data;
    pj_memcpy(param, ff->param, sizeof(*param));

    return PJ_SUCCESS;
}

static pj_status_t ffmpeg_packetize(pjmedia_vid_codec *codec,
                                    pj_uint8_t *bits,
                                    pj_size_t bits_len,
                                    unsigned *bits_pos,
                                    pj_uint8_t *payload,
                                    pj_size_t *payload_len,
                                    pj_bool_t is_keyframe)
{
    ffmpeg_private *ff = (ffmpeg_private *)codec->codec_data;

    if (ff->desc->packetize)
    {
        return (*ff->desc->packetize)(ff, bits, bits_len, bits_pos,
                                      payload, payload_len, is_keyframe);
    }

    return PJ_ENOTSUP;
}

static pj_status_t ffmpeg_codec_encode_begin(pjmedia_vid_codec *codec,
                                             const pjmedia_vid_encode_opt *opt,
                                             const pjmedia_frame *input,
                                             unsigned out_size,
                                             pjmedia_frame *output,
                                             pj_bool_t *has_more)
{
    ffmpeg_private *ff = (ffmpeg_private *)codec->codec_data;
    pj_status_t status;
    int frame_size = 0;
    int done = 0;
    int nal_type = 0;

    *has_more = PJ_FALSE;

    ff->enc_processed = 0;
    ff->enc_frame_len = 0;

    while (1)
    {
        frame_size = nal_parser_next_nalu(ff->parser, ff->enc_buf, ff->enc_buf_size, &done);

        if (frame_size <= 0)
        {
            return PJ_EEOF;
        }
        // NOTE: INTERCOM HACK; original parse function didn't include NAL header
        // Added a hack to copy header in buffer, but changing return size alters the core
        // parse logic, so adjusting the size here as a workaround. This needs to be fixed later
        frame_size += 4;
        ff->enc_frame_len = frame_size;
        nal_type = (((unsigned char *)ff->enc_buf)[4]) & NAL_TYPE_MASK;
        ff->enc_buf_is_keyframe = nal_type == NAL_TYPE_IDR_CODED_SLICE;

        if (ff->first_idr_sent)
        {
            break;
        }

        if (nal_type == NAL_TYPE_SPS)
        {
            ff->first_idr_sent = PJ_TRUE;
            break;
        }

        system("test_encode --stream 4 --force-idr");
    }

    status = ffmpeg_codec_encode_more(codec, out_size, output, has_more);

    *has_more = ff->enc_processed < ff->enc_frame_len;

    uint64_t new_timestamp = get_timestamp_from_sei(ff->enc_buf, ff->enc_frame_len);

    if (new_timestamp != 0)
    {
        output->timestamp.u64 = new_timestamp;
        ff->last_pts = new_timestamp;
    }
    else
    {
        output->timestamp.u64 = ff->last_pts;
    }
    return status;
}

static pj_status_t ffmpeg_codec_encode_more(pjmedia_vid_codec *codec,
                                            unsigned out_size,
                                            pjmedia_frame *output,
                                            pj_bool_t *has_more)
{
    ffmpeg_private *ff = (ffmpeg_private *)codec->codec_data;
    pj_uint8_t *payload = (pj_uint8_t *)output->buf;
    pj_size_t payload_len = (pj_size_t)out_size;
    pj_status_t status;

    *has_more = PJ_FALSE;

    if (ff->enc_processed >= ff->enc_frame_len)
    {
        /* No more frame */
        return PJ_EEOF;
    }

    status = ffmpeg_packetize(codec, (pj_uint8_t *)ff->enc_buf,
                              ff->enc_frame_len, &ff->enc_processed,
                              payload, &payload_len, ff->enc_buf_is_keyframe);
    if (status != PJ_SUCCESS)
        return status;

    output->type = PJMEDIA_FRAME_TYPE_VIDEO;
    output->size = payload_len;

    if (ff->enc_buf_is_keyframe)
        output->bit_info |= PJMEDIA_VID_FRM_KEYFRAME;

    *has_more = (ff->enc_processed < ff->enc_frame_len);
    output->timestamp.u64 = ff->last_pts;
    return PJ_SUCCESS;
}

#endif /* PJMEDIA_HAS_FFMPEG_VID_CODEC */
