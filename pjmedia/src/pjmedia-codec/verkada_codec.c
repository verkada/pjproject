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


/*
 * Only build this file if PJMEDIA_HAS_FFMPEG_VID_CODEC != 0 and 
 * PJMEDIA_HAS_VIDEO != 0
 */
#if defined(PJMEDIA_HAS_FFMPEG_VID_CODEC) && \
            PJMEDIA_HAS_FFMPEG_VID_CODEC != 0 && \
    defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

#define THIS_FILE   "ffmpeg_vid_codecs.c"

#include "../pjmedia/ffmpeg_util.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#if LIBAVCODEC_VER_AT_LEAST(53,20)
  /* Needed by 264 so far, on libavcodec 53.20 */
# include <libavutil/opt.h>
#endif


/* Various compatibility */

#if LIBAVCODEC_VER_AT_LEAST(53,20)
#  define AVCODEC_OPEN(ctx,c)           avcodec_open2(ctx,c,NULL)
#else
#  define AVCODEC_OPEN(ctx,c)           avcodec_open(ctx,c)
#endif

#if LIBAVCODEC_VER_AT_LEAST(53,61)
#  if LIBAVCODEC_VER_AT_LEAST(54,59)
   /* Not sure when AVCodec::encode is obsoleted/removed. */
#    define AVCODEC_HAS_ENCODE(c)       (c->encode2 != (void*)0)
#  else
   /* Not sure when AVCodec::encode2 is introduced. It appears in 
    * libavcodec 53.61 where some codecs actually still use AVCodec::encode
    * (e.g: H263, H264).
    */
#    define AVCODEC_HAS_ENCODE(c)       (c->encode != (void*)0 || \
                                         c->encode2 != (void*)0)
#  endif
#  define AV_OPT_SET(obj,name,val,opt)  (av_opt_set(obj,name,val,opt)==0)
#  define AV_OPT_SET_INT(obj,name,val)  (av_opt_set_int(obj,name,val,0)==0)
#else
#  define AVCODEC_HAS_ENCODE(c)         (c->encode != (void*)0)
#  define AV_OPT_SET(obj,name,val,opt)  (av_set_string3(obj,name,val,opt,NULL)==0)
#  define AV_OPT_SET_INT(obj,name,val)  (av_set_int(obj,name,val)!=NULL)
#endif
#define AVCODEC_HAS_DECODE(c)           (c->decode != (void*)0)

/* AVCodec H264 default PT */
#define AVC_H264_PT                       PJMEDIA_RTP_PT_H264_RSV3

/* Prototypes for FFMPEG codecs factory */
static pj_status_t ffmpeg_test_alloc( pjmedia_vid_codec_factory *factory, 
                                      const pjmedia_vid_codec_info *id );
static pj_status_t ffmpeg_default_attr( pjmedia_vid_codec_factory *factory, 
                                        const pjmedia_vid_codec_info *info, 
                                        pjmedia_vid_codec_param *attr );
static pj_status_t ffmpeg_enum_codecs( pjmedia_vid_codec_factory *factory, 
                                       unsigned *count, 
                                       pjmedia_vid_codec_info codecs[]);
static pj_status_t ffmpeg_alloc_codec( pjmedia_vid_codec_factory *factory, 
                                       const pjmedia_vid_codec_info *info, 
                                       pjmedia_vid_codec **p_codec);
static pj_status_t ffmpeg_dealloc_codec( pjmedia_vid_codec_factory *factory, 
                                         pjmedia_vid_codec *codec );

/* Prototypes for FFMPEG codecs implementation. */
static pj_status_t  ffmpeg_codec_init( pjmedia_vid_codec *codec, 
                                       pj_pool_t *pool );
static pj_status_t  ffmpeg_codec_open( pjmedia_vid_codec *codec, 
                                       pjmedia_vid_codec_param *attr );
static pj_status_t  ffmpeg_codec_close( pjmedia_vid_codec *codec );
static pj_status_t  ffmpeg_codec_modify(pjmedia_vid_codec *codec, 
                                        const pjmedia_vid_codec_param *attr );
static pj_status_t  ffmpeg_codec_get_param(pjmedia_vid_codec *codec,
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
    NULL
};

/* Definition for FFMPEG codecs factory operations. */
static pjmedia_vid_codec_factory_op ffmpeg_factory_op =
{
    &ffmpeg_test_alloc,
    &ffmpeg_default_attr,
    &ffmpeg_enum_codecs,
    &ffmpeg_alloc_codec,
    &ffmpeg_dealloc_codec
};


/* FFMPEG codecs factory */
static struct ffmpeg_factory {
    pjmedia_vid_codec_factory    base;
    pjmedia_vid_codec_mgr       *mgr;
    pj_pool_factory             *pf;
    pj_pool_t                   *pool;
    pj_mutex_t                  *mutex;
} ffmpeg_factory;


typedef struct ffmpeg_codec_desc ffmpeg_codec_desc;


/* FFMPEG codecs private data. */
typedef struct ffmpeg_private
{
    const ffmpeg_codec_desc         *desc;
    pjmedia_vid_codec_param         *param;     /**< Codec param            */
    pj_pool_t                       *pool;      /**< Pool for each instance */

    /* Format info and apply format param */
    const pjmedia_video_format_info *enc_vfi;
    pjmedia_video_apply_fmt_param    enc_vafp;

    /* Buffers, only needed for multi-packets */
    pj_bool_t                        whole;
    void                            *enc_buf;
    unsigned                         enc_buf_size;
    pj_bool_t                        enc_buf_is_keyframe;
    unsigned                         enc_frame_len;
    unsigned                         enc_processed;
    void                            *dec_buf;
    unsigned                         dec_buf_size;
    pj_timestamp                     last_dec_keyframe_ts; 

    /* The ffmpeg codec states. */
    const AVCodec                   *enc;
    AVCodecContext                  *enc_ctx;

    /* The ffmpeg decoder cannot set the output format, so format conversion
     * may be needed for post-decoding.
     */
    enum AVPixelFormat               expected_dec_fmt;
                                                /**< Expected output format of 
                                                     ffmpeg decoder         */

    void                            *data;      /**< Codec specific data    */
} ffmpeg_private;


/* Shortcuts for packetize & unpacketize function declaration,
 * as it has long params and is reused many times!
 */
#define FUNC_PACKETIZE(name) \
    pj_status_t(name)(ffmpeg_private *ff, pj_uint8_t *bits, \
                      pj_size_t bits_len, unsigned *bits_pos, \
                      pj_uint8_t *payload, pj_size_t *payload_len, \
                      pj_bool_t is_keyframe)

#define FUNC_UNPACKETIZE(name) \
    pj_status_t(name)(ffmpeg_private *ff, const pj_uint8_t *payload, \
                      pj_size_t payload_len, pj_uint8_t *bits, \
                      pj_size_t bits_len, unsigned *bits_pos)

#define FUNC_FMT_MATCH(name) \
    pj_status_t(name)(pj_pool_t *pool, \
                      pjmedia_sdp_media *offer, unsigned o_fmt_idx, \
                      pjmedia_sdp_media *answer, unsigned a_fmt_idx, \
                      unsigned option)


/* Type definition of codec specific functions */
typedef FUNC_PACKETIZE(*func_packetize);
typedef FUNC_UNPACKETIZE(*func_unpacketize);
typedef pj_status_t (*func_preopen)     (ffmpeg_private *ff);
typedef pj_status_t (*func_postopen)    (ffmpeg_private *ff);
typedef FUNC_FMT_MATCH(*func_sdp_fmt_match);


/* FFMPEG codec info */
struct ffmpeg_codec_desc
{
    /* Predefined info */
    pjmedia_vid_codec_info       info;
    pjmedia_format_id            base_fmt_id;   /**< Some codecs may be exactly
                                                     same or compatible with
                                                     another codec, base format
                                                     will tell the initializer
                                                     to copy this codec desc
                                                     from its base format   */
    pjmedia_rect_size            size;
    pjmedia_ratio                fps;
    pj_uint32_t                  avg_bps;
    pj_uint32_t                  max_bps;
    func_packetize               packetize;
    func_unpacketize             unpacketize;
    func_preopen                 preopen;
    func_preopen                 postopen;
    func_sdp_fmt_match           sdp_fmt_match;
    pjmedia_codec_fmtp           dec_fmtp;

    /* Init time defined info */
    pj_bool_t                    enabled;
    const AVCodec               *enc;
};


#if PJMEDIA_HAS_FFMPEG_CODEC_H264
static pj_status_t h264_preopen(ffmpeg_private *ff);
static pj_status_t h264_postopen(ffmpeg_private *ff);
static FUNC_PACKETIZE(h264_packetize);
static FUNC_UNPACKETIZE(h264_unpacketize);
#endif


/* Internal codec info */
static ffmpeg_codec_desc codec_desc[] =
{
#if PJMEDIA_HAS_FFMPEG_CODEC_H264
    {
        {PJMEDIA_FORMAT_H264, AVC_H264_PT, {"H264",4},
         {"Constrained Baseline (level=30, pack=1)", 39}},
        0,
        {720, 480},     {15, 1},        256000, 256000,
        &h264_packetize, &h264_unpacketize, &h264_preopen, &h264_postopen,
        &pjmedia_vid_codec_h264_match_sdp,
        /* Leading space for better compatibility (strange indeed!) */
        {2, { {{"profile-level-id",16},    {"42e01e",6}}, 
              {{" packetization-mode",19},  {"1",1}}, } },
    },
#endif
};

#if PJMEDIA_HAS_FFMPEG_CODEC_H264

typedef struct h264_data
{
    pjmedia_vid_codec_h264_fmtp  fmtp;
    pjmedia_h264_packetizer     *pktz;
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
    pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED; //PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL;

    status = pjmedia_h264_packetizer_create(ff->pool, &pktz_cfg, &data->pktz);
    if (status != PJ_SUCCESS)
        return status;

    return PJ_SUCCESS;
}

static pj_status_t h264_postopen(ffmpeg_private *ff)
{
    h264_data *data = (h264_data*)ff->data;
    PJ_UNUSED_ARG(data);
    return PJ_SUCCESS;
}

static FUNC_PACKETIZE(h264_packetize)
{
    PJ_UNUSED_ARG(is_keyframe);
    h264_data *data = (h264_data*)ff->data;
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

static FUNC_UNPACKETIZE(h264_unpacketize)
{
    h264_data *data = (h264_data*)ff->data;
    return pjmedia_h264_unpacketize(data->pktz, payload, payload_len,
                                    bits, bits_len, bits_pos);
}

#endif /* PJMEDIA_HAS_FFMPEG_CODEC_H264 */



static const ffmpeg_codec_desc* find_codec_desc_by_info(
                        const pjmedia_vid_codec_info *info)
{
    unsigned i;

    for (i=0; i<PJ_ARRAY_SIZE(codec_desc); ++i) {
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


static int find_codec_idx_by_fmt_id(pjmedia_format_id fmt_id)
{
    unsigned i;
    for (i=0; i<PJ_ARRAY_SIZE(codec_desc); ++i) {
        if (codec_desc[i].info.fmt_id == fmt_id)
            return i;
    }

    return -1;
}

static void init_codec(AVCodec *c, pj_bool_t is_encoder,
                       pj_bool_t is_decoder)
{
    pj_status_t status;
    ffmpeg_codec_desc *desc;
    pjmedia_format_id fmt_id;
    int codec_info_idx;

    desc = &codec_desc[0];

    /* Get raw/decoded format ids in the encoder */
    // if (c->pix_fmts && is_encoder) { // AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV422P
        pjmedia_format_id raw_fmt[PJMEDIA_VID_CODEC_MAX_DEC_FMT_CNT]= {PJMEDIA_FORMAT_I420, PJMEDIA_FORMAT_NV12, PJMEDIA_FORMAT_I422};
        unsigned raw_fmt_cnt = 3;
        unsigned raw_fmt_cnt_should_be = 0;
        // const enum AVPixelFormat *p = c->pix_fmts;

        // for(;(p && *p != -1) &&
        //      (raw_fmt_cnt < PJMEDIA_VID_CODEC_MAX_DEC_FMT_CNT);
        //      ++p)
        // {
        //     pjmedia_format_id fmt_id;

        //     raw_fmt_cnt_should_be++;
        //     status = PixelFormat_to_pjmedia_format_id(*p, &fmt_id);
        //     if (status != PJ_SUCCESS) {
        //         PJ_PERROR(6, (THIS_FILE, status,
        //                       "Unrecognized ffmpeg pixel format %d", *p));
        //         continue;
        //     }

        //     if (desc->info.pt != PJMEDIA_RTP_PT_H264 ||
        //         fmt_id != PJMEDIA_FORMAT_RGB24)
        //     {
        //         raw_fmt[raw_fmt_cnt++] = fmt_id;
        //     }
        // }

        if (raw_fmt_cnt == 0) {
            PJ_LOG(5, (THIS_FILE, "No recognized raw format "
                                    "for codec [%s/%s], codec ignored",
                                    c->name, c->long_name));
            /* Skip this encoder */
            return;
        }

        if (raw_fmt_cnt < raw_fmt_cnt_should_be) {
            PJ_LOG(6, (THIS_FILE, "Codec [%s/%s] have %d raw formats, "
                                    "recognized only %d raw formats",
                                    c->name, c->long_name,
                                    raw_fmt_cnt_should_be, raw_fmt_cnt));
        }

        desc->info.dec_fmt_id_cnt = raw_fmt_cnt;
        pj_memcpy(desc->info.dec_fmt_id, raw_fmt, 
                  sizeof(raw_fmt[0])*raw_fmt_cnt);
    // }

    desc->info.dir |= PJMEDIA_DIR_ENCODING;
    // desc->enc = c; // TODO : remove this

    desc->enabled = PJ_TRUE;
    desc->info.clock_rate = 90000;
    desc->info.packings |= (PJMEDIA_VID_PACKING_WHOLE | PJMEDIA_VID_PACKING_PACKETS);
}

/*
 * Initialize and register FFMPEG codec factory to pjmedia endpoint.
 */
PJ_DEF(pj_status_t) pjmedia_codec_verkada_vid_init(pjmedia_vid_codec_mgr *mgr,
                                                  pj_pool_factory *pf)
{
    pj_pool_t *pool;
    AVCodec *c;
    pj_status_t status;
    unsigned i;

    if (ffmpeg_factory.pool != NULL) {
        /* Already initialized. */
        return PJ_SUCCESS;
    }

    if (!mgr) mgr = pjmedia_vid_codec_mgr_instance();
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
#if !LIBAVCODEC_VER_AT_LEAST(53,20)
    /* avcodec_init() dissappeared between version 53.20 and 54.15, not sure
     * exactly when 
     */
    avcodec_init();
#endif
    // for (i = 0; i < PJ_ARRAY_SIZE(codec_desc); ++i) {
    //     unsigned codec_id;

    //     pjmedia_format_id_to_CodecID(codec_desc[i].info.fmt_id, &codec_id);

    //     c = avcodec_find_encoder(codec_id);
    //     if (c)
            init_codec(c, PJ_TRUE, PJ_FALSE);
    // }


    /* Review all codecs for applying base format, registering format match for
     * SDP negotiation, etc.
     */
    for (i = 0; i < PJ_ARRAY_SIZE(codec_desc); ++i) {
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
PJ_DEF(pj_status_t) pjmedia_codec_verkada_vid_deinit(void)
{
    pj_status_t status = PJ_SUCCESS;

    if (ffmpeg_factory.pool == NULL) {
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
static pj_status_t ffmpeg_test_alloc( pjmedia_vid_codec_factory *factory, 
                                      const pjmedia_vid_codec_info *info )
{
    const ffmpeg_codec_desc *desc;

    PJ_ASSERT_RETURN(factory==&ffmpeg_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc) {
        return PJMEDIA_CODEC_EUNSUP;
    }

    return PJ_SUCCESS;
}

/*
 * Generate default attribute.
 */
static pj_status_t ffmpeg_default_attr( pjmedia_vid_codec_factory *factory, 
                                        const pjmedia_vid_codec_info *info, 
                                        pjmedia_vid_codec_param *attr )
{
    const ffmpeg_codec_desc *desc;
    unsigned i;

    PJ_ASSERT_RETURN(factory==&ffmpeg_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info && attr, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc) {
        return PJMEDIA_CODEC_EUNSUP;
    }

    pj_bzero(attr, sizeof(pjmedia_vid_codec_param));

    /* Scan the requested packings and use the lowest number */
    attr->packing = PJMEDIA_VID_PACKING_PACKETS;
    // for (i=0; i<15; ++i) {
    //     unsigned packing = (1 << i);
    //     if ((desc->info.packings & info->packings) & packing) {
    //         attr->packing = (pjmedia_vid_packing)packing;
    //         break;
    //     }
    // }
    // if (attr->packing == 0) { // PJMEDIA_VID_PACKING_PACKETS
    //     /* No supported packing in info */
    //     return PJMEDIA_CODEC_EUNSUP;
    // }

    /* Direction */
    attr->dir = desc->info.dir;

    /* Encoded format */
    pjmedia_format_init_video(&attr->enc_fmt, desc->info.fmt_id,
                              desc->size.w, desc->size.h,
                              desc->fps.num, desc->fps.denum);

    /* Decoded format */
    pjmedia_format_init_video(&attr->dec_fmt, PJMEDIA_FORMAT_I420, //desc->info.dec_fmt_id[0], // PJMEDIA_FORMAT_I420
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
static pj_status_t ffmpeg_enum_codecs( pjmedia_vid_codec_factory *factory,
                                       unsigned *count, 
                                       pjmedia_vid_codec_info codecs[])
{
    unsigned i, max_cnt;

    PJ_ASSERT_RETURN(codecs && *count > 0, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &ffmpeg_factory.base, PJ_EINVAL);

    max_cnt = PJ_MIN(*count, PJ_ARRAY_SIZE(codec_desc));
    *count = 0;

    for (i=0; i<max_cnt; ++i) {
        if (codec_desc[i].enabled) {
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
static pj_status_t ffmpeg_alloc_codec( pjmedia_vid_codec_factory *factory, 
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
    if (!desc) {
        return PJMEDIA_CODEC_EUNSUP;
    }

    /* Create pool for codec instance */
    pool = pj_pool_create(ffmpeg_factory.pf, "ffmpeg codec", 512, 512, NULL);
    if (!pool) {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec = PJ_POOL_ZALLOC_T(pool, pjmedia_vid_codec);
    if (!codec) {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec->op = &ffmpeg_op;
    codec->factory = factory;
    ff = PJ_POOL_ZALLOC_T(pool, ffmpeg_private);
    if (!ff) {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec->codec_data = ff;
    ff->pool = pool;
    ff->enc = desc->enc;
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
static pj_status_t ffmpeg_dealloc_codec( pjmedia_vid_codec_factory *factory, 
                                         pjmedia_vid_codec *codec )
{
    ffmpeg_private *ff;
    pj_pool_t *pool;

    PJ_ASSERT_RETURN(factory && codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &ffmpeg_factory.base, PJ_EINVAL);

    /* Close codec, if it's not closed. */
    ff = (ffmpeg_private*) codec->codec_data;
    pool = ff->pool;
    codec->codec_data = NULL;
    pj_pool_release(pool);

    return PJ_SUCCESS;
}

/*
 * Init codec.
 */
static pj_status_t ffmpeg_codec_init( pjmedia_vid_codec *codec, 
                                      pj_pool_t *pool )
{
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    return PJ_SUCCESS;
}

static void print_ffmpeg_err(int err)
{
#if LIBAVCODEC_VER_AT_LEAST(52,72)
    char errbuf[512];
    if (av_strerror(err, errbuf, sizeof(errbuf)) >= 0)
        PJ_LOG(5, (THIS_FILE, "ffmpeg err %d: %s", err, errbuf));
#else
    PJ_LOG(5, (THIS_FILE, "ffmpeg err %d", err));
#endif

}

static pj_status_t open_ffmpeg_codec(ffmpeg_private *ff,
                                     pj_mutex_t *ff_mutex)
{
    enum AVPixelFormat pix_fmt;
    pjmedia_video_format_detail *vfd;
    pj_bool_t enc_opened = PJ_FALSE, dec_opened = PJ_FALSE;
    pj_status_t status;

    /* Get decoded pixel format */
    // pix_fmt = AV_PIX_FMT_YUV420P;
    status = pjmedia_format_id_to_PixelFormat(ff->param->dec_fmt.id,
                                              &pix_fmt);
    if (status != PJ_SUCCESS)
        return status;
    ff->expected_dec_fmt = pix_fmt;

    /* Get video format detail for shortcut access to encoded format */
    vfd = pjmedia_format_get_video_format_detail(&ff->param->enc_fmt, 
                                                 PJ_TRUE);

    /* Allocate ffmpeg codec context */
//     if (ff->param->dir & PJMEDIA_DIR_ENCODING) {
// #if LIBAVCODEC_VER_AT_LEAST(53,20)
//         ff->enc_ctx = avcodec_alloc_context3(ff->enc);
// #else
//         ff->enc_ctx = avcodec_alloc_context();
// #endif
//         if (ff->enc_ctx == NULL)
//             goto on_error;
//     }

    /* Init generic encoder params */
//     if (ff->param->dir & PJMEDIA_DIR_ENCODING) {
//         AVCodecContext *ctx = ff->enc_ctx;

//         ctx->pix_fmt = pix_fmt;
//         ctx->width = vfd->size.w;
//         ctx->height = vfd->size.h;
//         ctx->time_base.num = vfd->fps.denum;
//         ctx->time_base.den = vfd->fps.num;
//         if (vfd->avg_bps) {
//             ctx->bit_rate = vfd->avg_bps;
//             if (vfd->max_bps > vfd->avg_bps)
//                 ctx->bit_rate_tolerance = vfd->max_bps - vfd->avg_bps;
//         }
//         ctx->strict_std_compliance = FF_COMPLIANCE_STRICT;
//         ctx->workaround_bugs = FF_BUG_AUTODETECT;
//         ctx->opaque = ff;

//         /* Set no delay, note that this may cause some codec functionals
//          * not working (e.g: rate control).
//          */
// #if LIBAVCODEC_VER_AT_LEAST(52,113) && !LIBAVCODEC_VER_AT_LEAST(53,20)
//         ctx->rc_lookahead = 0;
// #endif
//     }

    /* Override generic params or apply specific params before opening
     * the codec.
     */
     if (ff->desc->preopen) {
         status = (*ff->desc->preopen)(ff);
        if (status != PJ_SUCCESS)
             goto on_error;
    }

    /* Let the codec apply specific params after the codec opened */
    if (ff->desc->postopen) {
        status = (*ff->desc->postopen)(ff);
        if (status != PJ_SUCCESS)
            goto on_error;
    }

    return PJ_SUCCESS;

on_error:
    if (ff->enc_ctx) {
        if (enc_opened)
            avcodec_close(ff->enc_ctx);
        av_free(ff->enc_ctx);
        ff->enc_ctx = NULL;
    }
    return status;
}

/*
 * Open codec.
 */
static pj_status_t ffmpeg_codec_open( pjmedia_vid_codec *codec, 
                                      pjmedia_vid_codec_param *attr )
{
    ffmpeg_private *ff;
    pj_status_t status;
    pj_mutex_t *ff_mutex;

    PJ_ASSERT_RETURN(codec && attr, PJ_EINVAL);
    ff = (ffmpeg_private*)codec->codec_data;

    ff->param = pjmedia_vid_codec_param_clone(ff->pool, attr);

    /* Normalize encoding MTU in codec param */
    if (ff->param->enc_mtu > PJMEDIA_MAX_VID_PAYLOAD_SIZE)
        ff->param->enc_mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

    /* Open the codec */
    ff_mutex = ((struct ffmpeg_factory*)codec->factory)->mutex;
    status = open_ffmpeg_codec(ff, ff_mutex);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Init format info and apply-param of decoder */
    // ff->dec_vfi = pjmedia_get_video_format_info(NULL, ff->param->dec_fmt.id);
    // if (!ff->dec_vfi) {
    //     status = PJ_EINVAL;
    //     goto on_error;
    // }
    // pj_bzero(&ff->dec_vafp, sizeof(ff->dec_vafp));
    // ff->dec_vafp.size = ff->param->dec_fmt.det.vid.size;
    // ff->dec_vafp.buffer = NULL;
    // status = (*ff->dec_vfi->apply_fmt)(ff->dec_vfi, &ff->dec_vafp);
    // if (status != PJ_SUCCESS) {
    //     goto on_error;
    // }

    /* Init format info and apply-param of encoder */
    ff->enc_vfi = pjmedia_get_video_format_info(NULL, ff->param->dec_fmt.id);
    if (!ff->enc_vfi) {
        status = PJ_EINVAL;
        goto on_error;
    }
    pj_bzero(&ff->enc_vafp, sizeof(ff->enc_vafp));
    ff->enc_vafp.size = ff->param->enc_fmt.det.vid.size;
    ff->enc_vafp.buffer = NULL;
    status = (*ff->enc_vfi->apply_fmt)(ff->enc_vfi, &ff->enc_vafp);
    if (status != PJ_SUCCESS) {
        goto on_error;
    }

    /* Alloc buffers if needed */
    ff->whole = (ff->param->packing == PJMEDIA_VID_PACKING_WHOLE);
    if (!ff->whole) {
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
static pj_status_t ffmpeg_codec_close( pjmedia_vid_codec *codec )
{
    ffmpeg_private *ff;
    pj_mutex_t *ff_mutex;

    PJ_ASSERT_RETURN(codec, PJ_EINVAL);
    ff = (ffmpeg_private*)codec->codec_data;
    ff_mutex = ((struct ffmpeg_factory*)codec->factory)->mutex;

    pj_mutex_lock(ff_mutex);
    if (ff->enc_ctx) {
        avcodec_close(ff->enc_ctx);
        av_free(ff->enc_ctx);
    }
    ff->enc_ctx = NULL;
    pj_mutex_unlock(ff_mutex);

    return PJ_SUCCESS;
}


/*
 * Modify codec settings.
 */
static pj_status_t  ffmpeg_codec_modify( pjmedia_vid_codec *codec, 
                                         const pjmedia_vid_codec_param *attr)
{
    ffmpeg_private *ff = (ffmpeg_private*)codec->codec_data;

    PJ_UNUSED_ARG(attr);
    PJ_UNUSED_ARG(ff);

    return PJ_ENOTSUP;
}

static pj_status_t  ffmpeg_codec_get_param(pjmedia_vid_codec *codec,
                                           pjmedia_vid_codec_param *param)
{
    ffmpeg_private *ff;

    PJ_ASSERT_RETURN(codec && param, PJ_EINVAL);

    ff = (ffmpeg_private*)codec->codec_data;
    pj_memcpy(param, ff->param, sizeof(*param));

    return PJ_SUCCESS;
}


static pj_status_t  ffmpeg_packetize ( pjmedia_vid_codec *codec,
                                       pj_uint8_t *bits,
                                       pj_size_t bits_len,
                                       unsigned *bits_pos,
                                       pj_uint8_t *payload,
                                       pj_size_t *payload_len,
                                       pj_bool_t is_keyframe)
{
    ffmpeg_private *ff = (ffmpeg_private*)codec->codec_data;

    if (ff->desc->packetize) {
        return (*ff->desc->packetize)(ff, bits, bits_len, bits_pos,
                                      payload, payload_len, is_keyframe);
    }

    return PJ_ENOTSUP;
}

/*
 * Encode frames.
 */
static pj_status_t ffmpeg_codec_encode_whole(pjmedia_vid_codec *codec,
                                             const pjmedia_vid_encode_opt *opt,
                                             const pjmedia_frame *input,
                                             unsigned output_buf_len,
                                             pjmedia_frame *output)
{
    ffmpeg_private *ff = (ffmpeg_private*)codec->codec_data;
    pj_uint8_t *p = (pj_uint8_t*)input->buf;
    AVFrame avframe;
    AVPacket avpacket;
    int err, got_packet;
    //AVRational src_timebase;
    /* For some reasons (e.g: SSE/MMX usage), the avcodec_encode_video() must
     * have stack aligned to 16 bytes. Let's try to be safe by preparing the
     * 16-bytes aligned stack here, in case it's not managed by the ffmpeg.
     */
    PJ_ALIGN_DATA(pj_uint32_t i[4], 16);

    if ((long)(pj_ssize_t)i & 0xF) {
        PJ_LOG(2,(THIS_FILE, "Stack alignment fails"));
    }

    /* Check if encoder has been opened */
    // PJ_ASSERT_RETURN(ff->enc_ctx, PJ_EINVALIDOP);

#ifdef PJMEDIA_USE_OLD_FFMPEG
    avcodec_get_frame_defaults(&avframe);
#else
    pj_bzero(&avframe, sizeof(avframe));
    av_frame_unref(&avframe);
#endif

    for (i[0] = 0; i[0] < ff->enc_vfi->plane_cnt; ++i[0]) {
        avframe.data[i[0]] = p;
        avframe.linesize[i[0]] = ff->enc_vafp.strides[i[0]];
        p += ff->enc_vafp.plane_bytes[i[0]];
    }

#if LIBAVCODEC_VER_AT_LEAST(58,134)
    avframe.height = ff->enc_ctx->height;
    avframe.width = ff->enc_ctx->width;
    avframe.format = ff->enc_ctx->pix_fmt;
#endif

    /* Force keyframe */
    if (opt && opt->force_keyframe) {
#if LIBAVCODEC_VER_AT_LEAST(53,20)
        avframe.pict_type = AV_PICTURE_TYPE_I;
#else
        avframe.pict_type = FF_I_TYPE;
#endif
    }

    av_init_packet(&avpacket);
    avpacket.data = (pj_uint8_t*)output->buf;
    avpacket.size = output_buf_len;

#if LIBAVCODEC_VER_AT_LEAST(58,137)
    PJ_UNUSED_ARG(got_packet);
    err = avcodec_send_frame(ff->enc_ctx, &avframe);
    if (err >= 0) {
        AVPacket *pkt = NULL;
        pj_uint8_t  *bits_out = (pj_uint8_t*) output->buf;
        unsigned out_size = 0;
        pkt = av_packet_alloc();
        if (pkt) {
            while (err >= 0) {
                err = avcodec_receive_packet(ff->enc_ctx, pkt);
                if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
                    err = out_size;
                    break;
                }
                if (err >= 0) {
                    pj_memcpy(bits_out, pkt->data, pkt->size);
                    bits_out += pkt->size;
                    out_size += pkt->size;
                    av_packet_unref(&avpacket);
                }
            }
            av_packet_free(&pkt);
        }
    }

#elif LIBAVCODEC_VER_AT_LEAST(54,15)

    err = avcodec_encode_video2(ff->enc_ctx, &avpacket, &avframe, &got_packet);
    if (!err && got_packet)
        err = avpacket.size;
#else
    PJ_UNUSED_ARG(got_packet);
    err = avcodec_encode_video(ff->enc_ctx, avpacket.data, avpacket.size, &avframe);
#endif

    if (err < 0) {
        print_ffmpeg_err(err);
        return PJMEDIA_CODEC_EFAILED;
    } else {
        pj_bool_t has_key_frame = PJ_FALSE;
        output->size = err;
        output->bit_info = 0;

#if LIBAVCODEC_VER_AT_LEAST(54,15)
        has_key_frame = (avpacket.flags & AV_PKT_FLAG_KEY);
#else
        has_key_frame = ff->enc_ctx->coded_frame->key_frame;
#endif
        if (has_key_frame)
            output->bit_info |= PJMEDIA_VID_FRM_KEYFRAME;
    }

    return PJ_SUCCESS;
}

#include <pj/file_io.h>

static int read_whole_file(char *filename, pj_uint8_t *buffer, pj_size_t size) {
    pj_oshandle_t fd = 0;
    pj_status_t status;
    /*
     * Re-open the file and read data.
     */
    status = pj_file_open(NULL, filename, PJ_O_RDONLY, &fd);
    if (status != PJ_SUCCESS) {
        printf("...file_open() error", status);
        return -100;
    }
    pj_ssize_t totalRead;
    totalRead = 0;
    while (totalRead < size) {
        pj_ssize_t read;
        read = (size - totalRead);
        status = pj_file_read(fd, &buffer[totalRead], &read);
        if (status != PJ_SUCCESS) {
            PJ_LOG(3,("", "...error reading file after %ld bytes "
                          "(error follows)", totalRead));
            printf("...error", status);
            return -110;
        }
        if (read == 0) {
            // EOF
            break;
        }
        totalRead += read;
    }

    if (totalRead != size)
        return -120;
    pj_file_close(fd);
    return totalRead;    
}

static filenamecounter = 0;
static struct timeval prev_tv;

static fill_buffer(pj_uint8_t *buffer, pj_size_t size) {
    FILE *file;
    char filename[256];
    long fileSize;
    size_t bytesRead;
    const char baseName[] = "/Users/darshan.patel/ws/exp/q2/pjproject/videos/input.h264.";
    //const char baseName[] = "/mnt/internal/mmcblk0p21/videos/input.h264.";
    sprintf(filename, "%s%04d", baseName, filenamecounter);
    filenamecounter++;
    if (filenamecounter > 1800) {
        filenamecounter = 0;
    }
    // add sleep of 40ms
    usleep(400000);
    // print the current timestamp save it, compare it with the previous one
    // and print the difference
    struct timeval tv;
    gettimeofday(&tv, NULL);
    printf(" 4000 timestamp: %ld\n", tv.tv_sec * 1000000 + tv.tv_usec);
    // now calculate the difference with prev_tv
    if (prev_tv.tv_sec != 0) {
        long diff = (tv.tv_sec * 1000000 + tv.tv_usec) - (prev_tv.tv_sec * 1000000 + prev_tv.tv_usec);
        printf("diff: %ld\n", diff);
    }
    prev_tv = tv;
    printf("filename: %s\n", filename);
    file = fopen(filename, "rb");
    if (file == NULL) {
        printf("Failed to open the file.\n");
        // print the error message
        perror("fopen");
        return 1;
    }

    // Move the file pointer to the end of the file
    fseek(file, 0, SEEK_END);

    // Get the current position of the file pointer, which represents the file size
    fileSize = ftell(file);
    if (fileSize == -1) {
        printf("Failed to determine the file size.\n");
        fclose(file);
        return 1;
    }

    // Print the file size
    // printf("File size: %ld bytes\n", fileSize);

    if (fileSize > size) {
        printf("File size is larger than the buffer size.\n");
        fclose(file);
        return 1;
    }
    bytesRead = 100;
    //memset(frame->buf, 0, frame->size);
    bytesRead = read_whole_file(filename, buffer, fileSize);
    // bytesRead = fread(frame->buf, 1, fileSize, file);
    if (bytesRead <= 0) {
        perror("fread");
        printf("Failed to read the file.\n");
        fclose(file);
        return 1;
    }
    fclose(file);
    return bytesRead;
    // frame->size = bytesRead;
}

static pj_status_t ffmpeg_codec_encode_begin(pjmedia_vid_codec *codec,
                                             const pjmedia_vid_encode_opt *opt,
                                             const pjmedia_frame *input,
                                             unsigned out_size,
                                             pjmedia_frame *output,
                                             pj_bool_t *has_more)
{
    ffmpeg_private *ff = (ffmpeg_private*)codec->codec_data;
    pj_status_t status;

    *has_more = PJ_FALSE;

    // if (ff->whole) {
    //     status = ffmpeg_codec_encode_whole(codec, opt, input, out_size, output);
    // } else {
        // pjmedia_frame whole_frm;
        // pj_bzero(&whole_frm, sizeof(whole_frm));
        // whole_frm.buf = ff->enc_buf;
        // whole_frm.size = ff->enc_buf_size;
        // status = ffmpeg_codec_encode_whole(codec, opt, input,
        //                                    (unsigned)whole_frm.size, 
        //                                    &whole_frm);
        // if (status != PJ_SUCCESS)
        //     return status;

        // ff->enc_buf_is_keyframe = (whole_frm.bit_info & 
        //                            PJMEDIA_VID_FRM_KEYFRAME);
        // ff->enc_frame_len = (unsigned)whole_frm.size;
        ff->enc_processed = 0;

        ff->enc_frame_len = fill_buffer(ff->enc_buf, ff->enc_buf_size);
        status = ffmpeg_codec_encode_more(codec, out_size, output, has_more);
    // }

    return status;
}

static pj_status_t ffmpeg_codec_encode_more(pjmedia_vid_codec *codec,
                                            unsigned out_size,
                                            pjmedia_frame *output,
                                            pj_bool_t *has_more)
{
    ffmpeg_private *ff = (ffmpeg_private*)codec->codec_data;
    pj_uint8_t *payload = (pj_uint8_t *)output->buf;
    pj_size_t payload_len = (pj_size_t) out_size;
    pj_status_t status;

    *has_more = PJ_FALSE;

    if (ff->enc_processed >= ff->enc_frame_len) {
        /* No more frame */
        return PJ_EEOF;
    }

    status = ffmpeg_packetize(codec, (pj_uint8_t*)ff->enc_buf,
                              ff->enc_frame_len, &ff->enc_processed,
                              payload, &payload_len, ff->enc_buf_is_keyframe);
    if (status != PJ_SUCCESS)
        return status;

    output->type = PJMEDIA_FRAME_TYPE_VIDEO;
    output->size = payload_len;

    if (ff->enc_buf_is_keyframe)
        output->bit_info |= PJMEDIA_VID_FRM_KEYFRAME;

    *has_more = (ff->enc_processed < ff->enc_frame_len);

    return PJ_SUCCESS;
}


/*
 * Unreference AVFrame.
 */
static void ffmpeg_frame_unref(AVFrame *frame)
{
#ifdef PJMEDIA_USE_OLD_FFMPEG
    (void)frame;
#else
    av_frame_unref(frame);
#endif
}


#ifdef _MSC_VER
#   pragma comment( lib, "avcodec.lib")
#endif

#endif  /* PJMEDIA_HAS_FFMPEG_VID_CODEC */

