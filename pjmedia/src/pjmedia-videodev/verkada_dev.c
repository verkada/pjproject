/*
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
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
#include <pjmedia-videodev/videodev_imp.h>
#include <pjmedia/clock.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/rand.h>


#if defined(PJMEDIA_HAS_VIDEO) && PJMEDIA_HAS_VIDEO != 0 && \
    defined(PJMEDIA_VERKADA_INTERCOM) && \
    PJMEDIA_VERKADA_INTERCOM != 0
    

#define THIS_FILE               "verkada_dev.c"
#define DEFAULT_CLOCK_RATE      90000
#define DEFAULT_WIDTH           720 //640 (800x600)-> 24 fps
#define DEFAULT_HEIGHT          480 //480
#define DEFAULT_FPS             120


/* verkada_ device info */
struct verkada_dev_info
{
    pjmedia_vid_dev_info         info;
};

/* verkada_ factory */
struct verkada_factory
{
    pjmedia_vid_dev_factory      base;
    pj_pool_t                   *pool;
    pj_pool_factory             *pf;

    unsigned                     dev_count;
    struct verkada_dev_info        *dev_info;
};

struct verkada_fmt_info {
    pjmedia_format_id            fmt_id;        /* Format ID                */

    /* Info for packed formats. */
    unsigned                     c_offset[3];   /* Color component offset, 
                                                   in bytes                 */
    unsigned                     c_stride[3];   /* Color component stride,
                                                   or distance between two 
                                                   consecutive same color
                                                   components, in bytes     */
};

/* Colorbar video source supports */
static struct verkada_fmt_info verkada_fmts[] =
{
    /* Planar formats */
    { PJMEDIA_FORMAT_I420 },
};

/* Video stream. */
struct verkada_stream
{
    pjmedia_vid_dev_stream           base;          /**< Base stream        */
    pjmedia_vid_dev_param            param;         /**< Settings           */
    pj_pool_t                       *pool;          /**< Memory pool.       */

    pjmedia_vid_dev_cb               vid_cb;        /**< Stream callback.   */
    void                            *user_data;     /**< Application data.  */

    const struct verkada_fmt_info      *cbfi;
    const pjmedia_video_format_info *vfi;
    pjmedia_video_apply_fmt_param    vafp;
    pj_uint8_t                      *first_line[PJMEDIA_MAX_VIDEO_PLANES];
    pj_timestamp                     ts;
    unsigned                         ts_inc;

    /* For active capturer only */
    pjmedia_clock                   *clock;
    pj_uint8_t                      *clock_buf;
};


/* Prototypes */
static pj_status_t verkada_factory_init(pjmedia_vid_dev_factory *f);
static pj_status_t verkada_factory_destroy(pjmedia_vid_dev_factory *f);
static pj_status_t verkada_factory_refresh(pjmedia_vid_dev_factory *f); 
static unsigned    verkada_factory_get_dev_count(pjmedia_vid_dev_factory *f);
static pj_status_t verkada_factory_get_dev_info(pjmedia_vid_dev_factory *f,
                                             unsigned index,
                                             pjmedia_vid_dev_info *info);
static pj_status_t verkada_factory_default_param(pj_pool_t *pool,
                                              pjmedia_vid_dev_factory *f,
                                              unsigned index,
                                              pjmedia_vid_dev_param *param);
static pj_status_t verkada_factory_create_stream(
                                        pjmedia_vid_dev_factory *f,
                                        pjmedia_vid_dev_param *param,
                                        const pjmedia_vid_dev_cb *cb,
                                        void *user_data,
                                        pjmedia_vid_dev_stream **p_vid_strm);

static pj_status_t verkada_stream_get_param(pjmedia_vid_dev_stream *strm,
                                         pjmedia_vid_dev_param *param);
static pj_status_t verkada_stream_get_cap(pjmedia_vid_dev_stream *strm,
                                       pjmedia_vid_dev_cap cap,
                                       void *value);
static pj_status_t verkada_stream_set_cap(pjmedia_vid_dev_stream *strm,
                                       pjmedia_vid_dev_cap cap,
                                       const void *value);
static pj_status_t verkada_stream_get_frame(pjmedia_vid_dev_stream *strm,
                                         pjmedia_frame *frame);
static pj_status_t verkada_stream_start(pjmedia_vid_dev_stream *strm);
static pj_status_t verkada_stream_stop(pjmedia_vid_dev_stream *strm);
static pj_status_t verkada_stream_destroy(pjmedia_vid_dev_stream *strm);

/* Operations */
static pjmedia_vid_dev_factory_op factory_op =
{
    &verkada_factory_init,
    &verkada_factory_destroy,
    &verkada_factory_get_dev_count,
    &verkada_factory_get_dev_info,
    &verkada_factory_default_param,
    &verkada_factory_create_stream,
    &verkada_factory_refresh
};

static pjmedia_vid_dev_stream_op stream_op =
{
    &verkada_stream_get_param,
    &verkada_stream_get_cap,
    &verkada_stream_set_cap,
    &verkada_stream_start,
    &verkada_stream_get_frame,
    NULL,
    &verkada_stream_stop,
    &verkada_stream_destroy
};


/****************************************************************************
 * Factory operations
 */
/*
 * Init verkada_ video driver.
 */
pjmedia_vid_dev_factory* pjmedia_verkada_factory(pj_pool_factory *pf)
{
    struct verkada_factory *f;
    pj_pool_t *pool;

    pool = pj_pool_create(pf, "verkada video", 4000, 4000, NULL);
    f = PJ_POOL_ZALLOC_T(pool, struct verkada_factory);
    f->pf = pf;
    f->pool = pool;
    f->base.op = &factory_op;

    return &f->base;
}


/* API: init factory */
static pj_status_t verkada_factory_init(pjmedia_vid_dev_factory *f)
{
    struct verkada_factory *cf = (struct verkada_factory*)f;
    struct verkada_dev_info *ddi;
    unsigned i;

    cf->dev_count = 1;

    cf->dev_info = (struct verkada_dev_info*)
                   pj_pool_calloc(cf->pool, cf->dev_count,
                                  sizeof(struct verkada_dev_info));

    /* Passive capturer */
    ddi = &cf->dev_info[0];
    pj_bzero(ddi, sizeof(*ddi));
    pj_ansi_strxcpy(ddi->info.name, "Verkada Intercom",
                    sizeof(ddi->info.name));
    pj_ansi_strxcpy(ddi->info.driver, "Verkada", 
                    sizeof(ddi->info.driver));
    ddi->info.dir = PJMEDIA_DIR_CAPTURE;
    ddi->info.has_callback = PJ_FALSE;

    ddi->info.caps = PJMEDIA_VID_DEV_CAP_FORMAT;
    ddi->info.fmt_cnt = PJ_ARRAY_SIZE(verkada_fmts);
    for (i = 0; i < ddi->info.fmt_cnt; i++) {
        pjmedia_format *fmt = &ddi->info.fmt[i];
        pjmedia_format_init_video(fmt, verkada_fmts[i].fmt_id,
                                  DEFAULT_WIDTH, DEFAULT_HEIGHT,
                                  DEFAULT_FPS, 1);
    }

    PJ_LOG(4, (THIS_FILE, "Colorbar video src initialized with %d device(s):",
               cf->dev_count));
    for (i = 0; i < cf->dev_count; i++) {
        PJ_LOG(4, (THIS_FILE, "%2d: %s", i, cf->dev_info[i].info.name));
    }

    return PJ_SUCCESS;
}

/* API: destroy factory */
static pj_status_t verkada_factory_destroy(pjmedia_vid_dev_factory *f)
{
    struct verkada_factory *cf = (struct verkada_factory*)f;

    pj_pool_safe_release(&cf->pool);

    return PJ_SUCCESS;
}

/* API: refresh the list of devices */
static pj_status_t verkada_factory_refresh(pjmedia_vid_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return PJ_SUCCESS;
}

/* API: get number of devices */
static unsigned verkada_factory_get_dev_count(pjmedia_vid_dev_factory *f)
{
    struct verkada_factory *cf = (struct verkada_factory*)f;
    return cf->dev_count;
}

/* API: get device info */
static pj_status_t verkada_factory_get_dev_info(pjmedia_vid_dev_factory *f,
                                             unsigned index,
                                             pjmedia_vid_dev_info *info)
{
    struct verkada_factory *cf = (struct verkada_factory*)f;

    PJ_ASSERT_RETURN(index < cf->dev_count, PJMEDIA_EVID_INVDEV);

    pj_memcpy(info, &cf->dev_info[index].info, sizeof(*info));

    return PJ_SUCCESS;
}

/* API: create default device parameter */
static pj_status_t verkada_factory_default_param(pj_pool_t *pool,
                                              pjmedia_vid_dev_factory *f,
                                              unsigned index,
                                              pjmedia_vid_dev_param *param)
{
    struct verkada_factory *cf = (struct verkada_factory*)f;
    struct verkada_dev_info *di = &cf->dev_info[index];

    PJ_ASSERT_RETURN(index < cf->dev_count, PJMEDIA_EVID_INVDEV);

    PJ_UNUSED_ARG(pool);

    pj_bzero(param, sizeof(*param));
    param->dir = PJMEDIA_DIR_CAPTURE;
    param->cap_id = index;
    param->rend_id = PJMEDIA_VID_INVALID_DEV;
    param->flags = PJMEDIA_VID_DEV_CAP_FORMAT;
    param->clock_rate = DEFAULT_CLOCK_RATE;
    pj_memcpy(&param->fmt, &di->info.fmt[0], sizeof(param->fmt));

    return PJ_SUCCESS;
}

static const struct verkada_fmt_info* get_verkada_fmt_info(pjmedia_format_id id)
{
    unsigned i;

    for (i = 0; i < PJ_ARRAY_SIZE(verkada_fmts); i++) {
        if (verkada_fmts[i].fmt_id == id)
            return &verkada_fmts[i];
    }

    return NULL;
}

static void fill_first_line(pj_uint8_t *first_lines[], 
                            const struct verkada_fmt_info *cbfi,
                            const pjmedia_video_format_info *vfi,
                            const pjmedia_video_apply_fmt_param *vafp)
{
    typedef pj_uint8_t color_comp_t[3];
    color_comp_t rgb_colors[] = 
    { 
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0}
    };
    color_comp_t yuv_colors[] = 
    { 
        //{235,128,128}, {162,44,142}, {131,156,44}, {112,72,58},
        //{84,184,198}, {65,100,212}, {35,212,114}, {16,128,128}
        {235,128,128}, {210,16,146}, {170,166,16}, {145,54,34},
        {106,202,222}, {81,90,240}, {41,240,110}, {16,128,128}
    };

    unsigned i, j, k;

    if (vfi->plane_cnt == 1) {
        /* Packed */

        for (i = 0; i < 8; ++i) {
            /* iterate bars */
            for (j = 0; j < 3; ++j) {
                /* iterate color components */
                pj_uint8_t *p = NULL, c;
                unsigned bar_width, inc_p;

                if (vfi->color_model == PJMEDIA_COLOR_MODEL_RGB)
                    c = rgb_colors[i][j];
                else
                    c = yuv_colors[i][j];

                bar_width = vafp->size.w/8;
                bar_width /= (cbfi->c_stride[j] * 8 / vfi->bpp);
                inc_p = cbfi->c_stride[j];
                p = first_lines[0] + bar_width*i*inc_p + cbfi->c_offset[j];

                /* draw this color */
                for (k = 0; k < bar_width; ++k) {
                    *p = c;
                    p += inc_p;
                }
            }
        }

    } else if (vfi->plane_cnt == 3) {

        for (i = 0; i < 8; ++i) {
            /* iterate bars */
            for (j = 0; j < 3; ++j) {
                /* iterate planes/color components */
                pj_uint8_t *p = NULL, c;
                unsigned bar_width;

                if (vfi->color_model == PJMEDIA_COLOR_MODEL_RGB)
                    c = rgb_colors[i][j];
                else {
                    if (vfi->id == PJMEDIA_FORMAT_YV12 && j > 0)
                        c = yuv_colors[i][3-j];
                    else
                        c = yuv_colors[i][j];
                }

                bar_width = vafp->strides[j]/8;
                p = first_lines[j] + bar_width*i;

                /* draw this plane/color */
                for (k = 0; k < bar_width; ++k)
                    *p++ = c;
            }
        }
    }
}

/* API: create stream */
static pj_status_t verkada_factory_create_stream(
                                        pjmedia_vid_dev_factory *f,
                                        pjmedia_vid_dev_param *param,
                                        const pjmedia_vid_dev_cb *cb,
                                        void *user_data,
                                        pjmedia_vid_dev_stream **p_vid_strm)
{
    struct verkada_factory *cf = (struct verkada_factory*)f;
    pj_pool_t *pool;
    struct verkada_stream *strm;
    const pjmedia_video_format_detail *vfd;
    const pjmedia_video_format_info *vfi;
    pjmedia_video_apply_fmt_param vafp;
    const struct verkada_fmt_info *cbfi;
    unsigned i;

    PJ_ASSERT_RETURN(f && param && p_vid_strm, PJ_EINVAL);
    PJ_ASSERT_RETURN(param->fmt.type == PJMEDIA_TYPE_VIDEO &&
                     param->fmt.detail_type == PJMEDIA_FORMAT_DETAIL_VIDEO &&
                     param->dir == PJMEDIA_DIR_CAPTURE,
                     PJ_EINVAL);

    pj_bzero(&vafp, sizeof(vafp));

    vfd = pjmedia_format_get_video_format_detail(&param->fmt, PJ_TRUE);
    vfi = pjmedia_get_video_format_info(NULL, param->fmt.id);
    cbfi = get_verkada_fmt_info(param->fmt.id);
    if (!vfi || !cbfi)
        return PJMEDIA_EVID_BADFORMAT;

    vafp.size = param->fmt.det.vid.size;
    if (vfi->apply_fmt(vfi, &vafp) != PJ_SUCCESS)
        return PJMEDIA_EVID_BADFORMAT;

    /* Create and Initialize stream descriptor */
    pool = pj_pool_create(cf->pf, "verkada-dev", 512, 512, NULL);
    PJ_ASSERT_RETURN(pool != NULL, PJ_ENOMEM);

    strm = PJ_POOL_ZALLOC_T(pool, struct verkada_stream);
    pj_memcpy(&strm->param, param, sizeof(*param));
    strm->pool = pool;
    pj_memcpy(&strm->vid_cb, cb, sizeof(*cb));
    strm->user_data = user_data;
    strm->vfi = vfi;
    strm->cbfi = cbfi;
    pj_memcpy(&strm->vafp, &vafp, sizeof(vafp));
    strm->ts_inc = PJMEDIA_SPF2(param->clock_rate, &vfd->fps, 1);

    for (i = 0; i < vfi->plane_cnt; ++i) {
        strm->first_line[i] = pj_pool_alloc(pool, vafp.strides[i]);
        pj_memset(strm->first_line[i], 255, vafp.strides[i]);
    }

    fill_first_line(strm->first_line, strm->cbfi, vfi, &strm->vafp);

    /* Done */
    strm->base.op = &stream_op;
    *p_vid_strm = &strm->base;

    return PJ_SUCCESS;
}

/* API: Get stream info. */
static pj_status_t verkada_stream_get_param(pjmedia_vid_dev_stream *s,
                                         pjmedia_vid_dev_param *pi)
{
    struct verkada_stream *strm = (struct verkada_stream*)s;

    PJ_ASSERT_RETURN(strm && pi, PJ_EINVAL);

    pj_memcpy(pi, &strm->param, sizeof(*pi));

    return PJ_SUCCESS;
}

/* API: get capability */
static pj_status_t verkada_stream_get_cap(pjmedia_vid_dev_stream *s,
                                       pjmedia_vid_dev_cap cap,
                                       void *pval)
{
    struct verkada_stream *strm = (struct verkada_stream*)s;

    PJ_UNUSED_ARG(strm);

    PJ_ASSERT_RETURN(s && pval, PJ_EINVAL);

// TODO: this feels bit hacky,which capability it supports then ?
//     if (cap==PJMEDIA_VID_DEV_CAP_INPUT_SCALE)
//     {
//         return PJMEDIA_EVID_INVCAP;
// //      return PJ_SUCCESS;
//     } else {
//         return PJMEDIA_EVID_INVCAP;
//     }
}

/* API: set capability */
static pj_status_t verkada_stream_set_cap(pjmedia_vid_dev_stream *s,
                                       pjmedia_vid_dev_cap cap,
                                       const void *pval)
{
    struct verkada_stream *strm = (struct verkada_stream*)s;

    PJ_UNUSED_ARG(strm);

    PJ_ASSERT_RETURN(s && pval, PJ_EINVAL);

// TODO: similar here which capability it supports then ?
    // if (cap==PJMEDIA_VID_DEV_CAP_INPUT_SCALE)
    // {
    //     return PJ_SUCCESS;
    // }

    return PJMEDIA_EVID_INVCAP;
}

/* API: Get frame from stream */
static pj_status_t verkada_stream_get_frame(pjmedia_vid_dev_stream *strm,
                                         pjmedia_frame *frame)
{
    struct verkada_stream *stream = (struct verkada_stream*)strm;
    FILE *file;
    char filename[256];
    long fileSize;
    size_t bytesRead;

    frame->type = PJMEDIA_FRAME_TYPE_VIDEO;
    frame->bit_info = 0;
    frame->timestamp = stream->ts;
    stream->ts.u64 += stream->ts_inc;

    return PJ_SUCCESS;
}

/* API: Start stream. */
static pj_status_t verkada_stream_start(pjmedia_vid_dev_stream *strm)
{
    struct verkada_stream *stream = (struct verkada_stream*)strm;

    PJ_LOG(4, (THIS_FILE, "Starting verkada video stream"));

    if (stream->clock)
        return pjmedia_clock_start(stream->clock);

    return PJ_SUCCESS;
}

/* API: Stop stream. */
static pj_status_t verkada_stream_stop(pjmedia_vid_dev_stream *strm)
{
    struct verkada_stream *stream = (struct verkada_stream*)strm;

    PJ_LOG(4, (THIS_FILE, "Stopping verkada video stream"));

    if (stream->clock)
        return pjmedia_clock_stop(stream->clock);

    return PJ_SUCCESS;
}


/* API: Destroy stream. */
static pj_status_t verkada_stream_destroy(pjmedia_vid_dev_stream *strm)
{
    struct verkada_stream *stream = (struct verkada_stream*)strm;

    PJ_ASSERT_RETURN(stream != NULL, PJ_EINVAL);

    verkada_stream_stop(strm);

    if (stream->clock)
        pjmedia_clock_destroy(stream->clock);
    stream->clock = NULL;

    pj_pool_release(stream->pool);

    return PJ_SUCCESS;
}

#endif  /* PJMEDIA_VIDEO_DEV_HAS_verkada_SRC */
