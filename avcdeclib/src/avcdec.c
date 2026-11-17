/* AVCDecLib API layer over FFmpeg 0.8.15 libavcodec (h264 decoder + parser). */
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/cpu.h"
#include "libavutil/mem.h"
#include "libavcodec/h264.h"
#include <limits.h>
#include "avcdec.h"
#include "avd_shim.h"

extern AVCodec ff_h264_decoder;
extern AVCodecParser ff_h264_parser;

struct AVCDecContext {
    AVCodecContext *avctx;
    AVCodecParserContext *parser;
    AVFrame *frame;
    long count;
    int eof_done;
};

static int registered;

unsigned long avcdec_version(void) { return 0x00080F02UL; }

long avcdec_init(const AVCDecCallbacks *cb, unsigned long flags, unsigned long *sp_mod16)
{
    unsigned long sp = avd_get_sp();
    if (sp_mod16) *sp_mod16 = sp & 15;
    if (!cb || !cb->alloc || !cb->release) return -1;
    avd_cb = *cb;
    av_force_cpu_flags((flags & AVCDEC_FLAG_ALTIVEC) ? AV_CPU_FLAG_ALTIVEC : 0);
    av_log_set_level(AV_LOG_ERROR);
    if (!registered) {
        avcodec_register(&ff_h264_decoder);
        av_register_codec_parser(&ff_h264_parser);
        registered = 1;
    }
    av_log(NULL, AV_LOG_ERROR, "AVCDecLib init: sp&15=%d flags=%d\n", (int)(sp & 15), (int)flags);
    return 0;
}

AVCDecContext *avcdec_open(const unsigned char *avcC, long avcC_len)
{
    AVCDecContext *c = av_mallocz(sizeof(*c));
    if (!c) return NULL;
    c->avctx = avcodec_alloc_context3(&ff_h264_decoder);
    c->frame = avcodec_alloc_frame();
    if (!c->avctx || !c->frame) goto fail;
    if (avcC && avcC_len > 0) {
        c->avctx->extradata = av_mallocz(avcC_len + FF_INPUT_BUFFER_PADDING_SIZE);
        if (!c->avctx->extradata) goto fail;
        memcpy(c->avctx->extradata, avcC, avcC_len);
        c->avctx->extradata_size = avcC_len;
    }
    if (avcodec_open2(c->avctx, &ff_h264_decoder, NULL) < 0) goto fail;
    c->parser = av_parser_init(CODEC_ID_H264);
    if (!c->parser) { avcodec_close(c->avctx); goto fail; }
    return c;
fail:
    if (c->avctx) { av_freep(&c->avctx->extradata); av_free(c->avctx); }
    av_free(c->frame);
    av_free(c);
    return NULL;
}

/* POC of the picture in c->frame: the decoder's Picture that owns the same buffer. */
static long frame_poc(AVCDecContext *c)
{
    H264Context *h = c->avctx->priv_data;
    int i;
    if (h && h->s.picture && c->frame->data[0])
        for (i = 0; i < h->s.picture_count; i++)
            if (h->s.picture[i].data[0] == c->frame->data[0]) return h->s.picture[i].poc;
    return LONG_MIN;
}

static void fill(AVCDecContext *c, AVCDecPicture *pic)
{
    int i;
    for (i = 0; i < 3; i++) { pic->plane[i] = c->frame->data[i]; pic->stride[i] = c->frame->linesize[i]; }
    pic->width = c->avctx->width;
    pic->height = c->avctx->height;
    pic->key = c->frame->key_frame;
    pic->count = ++c->count;
    pic->poc = frame_poc(c);
}

static long decode(AVCDecContext *c, const unsigned char *d, long n, AVCDecPicture *pic)
{
    AVPacket pk;
    int got = 0, r;
    av_init_packet(&pk);
    pk.data = (uint8_t *)d;
    pk.size = n;
    r = avcodec_decode_video2(c->avctx, c->frame, &got, &pk);
    if (r < 0 && !got) return 0;   /* concealed / skipped: not fatal, like the host harness */
    if (got) { fill(c, pic); return 1; }
    return 0;
}

long avcdec_feed_annexb(AVCDecContext *c, const unsigned char *data, long len, long *consumed, AVCDecPicture *pic)
{
    uint8_t *d = NULL; int ds = 0, used;
    if (consumed) *consumed = 0;
    if (!c || !pic) return -1;
    if (len <= 0) {
        if (c->eof_done) return 0;
        c->eof_done = 1;
        av_parser_parse2(c->parser, c->avctx, &d, &ds, NULL, 0, 0, 0, 0);
        return ds ? decode(c, d, ds, pic) : 0;
    }
    c->eof_done = 0;
    used = av_parser_parse2(c->parser, c->avctx, &d, &ds, data, len, 0, 0, 0);
    if (used < 0) return -2;
    if (consumed) *consumed = used;
    return ds ? decode(c, d, ds, pic) : 0;
}

long avcdec_decode_sample(AVCDecContext *c, const unsigned char *data, long len, AVCDecPicture *pic)
{
    if (!c || !pic || !data || len <= 0) return -1;
    return decode(c, data, len, pic);
}

long avcdec_flush(AVCDecContext *c, AVCDecPicture *pic)
{
    AVPacket pk; int got = 0;
    if (!c || !pic) return -1;
    av_init_packet(&pk);
    pk.data = NULL; pk.size = 0;
    avcodec_decode_video2(c->avctx, c->frame, &got, &pk);
    if (got) { fill(c, pic); return 1; }
    return 0;
}

void avcdec_reset(AVCDecContext *c)
{
    if (!c) return;
    avcodec_flush_buffers(c->avctx);
    av_parser_close(c->parser);
    c->parser = av_parser_init(CODEC_ID_H264);
    c->eof_done = 0;
}

void avcdec_close(AVCDecContext *c)
{
    if (!c) return;
    if (c->parser) av_parser_close(c->parser);
    avcodec_close(c->avctx);
    av_freep(&c->avctx->extradata);
    av_free(c->avctx);
    av_free(c->frame);
    av_free(c);
}

/* skip_loop_filter / skip_frame: 0 = default (nothing skipped), 1 = non-reference only,
   2 = all (skip_frame: 2 = everything except key frames). Returns 0, <0 on bad args. */
long avcdec_set_skip(AVCDecContext *c, long skip_loop_filter, long skip_frame)
{
    static const signed char lf[3] = { AVDISCARD_DEFAULT, AVDISCARD_NONREF, AVDISCARD_ALL };
    static const signed char fr[3] = { AVDISCARD_DEFAULT, AVDISCARD_NONREF, AVDISCARD_NONKEY };
    if (!c || !c->avctx) return -1;
    if (skip_loop_filter < 0 || skip_loop_filter > 2 || skip_frame < 0 || skip_frame > 2) return -2;
    c->avctx->skip_loop_filter = lf[skip_loop_filter];
    c->avctx->skip_frame = fr[skip_frame];
    return 0;
}

/* Current reorder delay in pictures (avctx->has_b_frames); valid once parameter sets are parsed. */
long avcdec_get_delay(AVCDecContext *c)
{
    if (!c || !c->avctx) return -1;
    return c->avctx->has_b_frames;
}

/* Force the reorder delay to n pictures (0..MAX_DELAYED_PIC_COUNT). FFmpeg may still raise it
   (SPS num_reorder_frames, out-of-order detection); it never lowers it. Returns 0, <0 on bad args. */
long avcdec_set_delay(AVCDecContext *c, long n)
{
    H264Context *h;
    if (!c || !c->avctx) return -1;
    if (n < 0 || n > MAX_DELAYED_PIC_COUNT) return -2;
    h = c->avctx->priv_data;
    c->avctx->has_b_frames = (int)n;
    h->s.low_delay = n ? 0 : 1;
    return 0;
}

/* Remove and return the lowest-POC held picture (FFmpeg's own choice rule, including the
   key-frame / mmco_reset barrier) and release FFmpeg's delay hold on it. 1 = *pic filled,
   0 = nothing held, -1 = bad args. *poc (optional) = its POC (also in pic->poc). */
long avcdec_take_lowest(AVCDecContext *c, AVCDecPicture *pic, long *poc)
{
    H264Context *h; Picture *out; int i, out_idx = 0;
    if (!c || !c->avctx || !pic) return -1;
    h = c->avctx->priv_data;
    out = h->delayed_pic[0];
    if (!out) return 0;
    for (i = 1; h->delayed_pic[i] && !h->delayed_pic[i]->key_frame && !h->delayed_pic[i]->mmco_reset; i++)
        if (h->delayed_pic[i]->poc < out->poc) { out = h->delayed_pic[i]; out_idx = i; }
    for (i = out_idx; h->delayed_pic[i]; i++) h->delayed_pic[i] = h->delayed_pic[i + 1];
    out->reference &= ~DELAYED_PIC_REF;
    h->next_outputed_poc = INT_MIN;   /* caller now orders; FFmpeg must not drop the next as out-of-order */
    *c->frame = *(AVFrame *)out;
    fill(c, pic);
    pic->poc = out->poc;
    if (poc) *poc = out->poc;
    return 1;
}

/* Like avcdec_take_lowest (same selection rule) but leaves the picture in FFmpeg's queue: no
   removal, DELAYED_PIC_REF and next_outputed_poc untouched, pic->count not advanced (it repeats
   the count of the last output picture). 1 = *pic filled, 0 = none held, -1 = bad args. */
long avcdec_peek_lowest(AVCDecContext *c, AVCDecPicture *pic, long *poc)
{
    H264Context *h; Picture *out; int i;
    if (!c || !c->avctx || !pic) return -1;
    h = c->avctx->priv_data;
    out = h->delayed_pic[0];
    if (!out) return 0;
    for (i = 1; h->delayed_pic[i] && !h->delayed_pic[i]->key_frame && !h->delayed_pic[i]->mmco_reset; i++)
        if (h->delayed_pic[i]->poc < out->poc) out = h->delayed_pic[i];
    *c->frame = *(AVFrame *)out;
    fill(c, pic);
    pic->count = --c->count;
    pic->poc = out->poc;
    if (poc) *poc = out->poc;
    return 1;
}

/* Number of pictures currently held in FFmpeg's reorder queue; -1 if c is 0. */
long avcdec_held(AVCDecContext *c)
{
    H264Context *h; long n = 0;
    if (!c || !c->avctx) return -1;
    h = c->avctx->priv_data;
    while (n <= MAX_DELAYED_PIC_COUNT && h->delayed_pic[n]) n++;
    return n;
}
