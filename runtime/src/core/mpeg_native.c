/*
 * Native MPEG-2 video decoding for the libmpeg replacement (hle_mpeg.c),
 * on FFmpeg's libavcodec (shipped with the PCSX2 dependencies).
 *
 * Input: the MPEG-2 video elementary stream the game would send to the IPU.
 * Output: pictures in display order, in the IPU RAW8 macroblock layout that
 * sceMpegGetPictureRAW8 produces: 16x16 macroblocks in column-major order
 * (top to bottom, then the next column), each Y[16][16], Cb[8][8], Cr[8][8]
 * (PCSX2 IPU/IPU_MultiISA.h, macroblock_8).
 */
#include "mpeg_native.h"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <stdlib.h>
#include <string.h>

struct mpeg_native {
    const AVCodec *codec;
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    int have_frame;   /* frame holds a picture not handed out yet */
    int last_type;    /* picture_coding_type of the last picture taken */
};

mpeg_native *mpn_create(void)
{
    mpeg_native *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    m->ctx = m->codec ? avcodec_alloc_context3(m->codec) : NULL;
    m->parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    m->pkt = av_packet_alloc();
    m->frame = av_frame_alloc();
    if (!m->ctx || !m->parser || !m->pkt || !m->frame || avcodec_open2(m->ctx, m->codec, NULL) < 0) {
        mpn_destroy(m);
        return NULL;
    }
    return m;
}

void mpn_destroy(mpeg_native *m)
{
    if (!m)
        return;
    if (m->parser)
        av_parser_close(m->parser);
    avcodec_free_context(&m->ctx);
    av_packet_free(&m->pkt);
    av_frame_free(&m->frame);
    free(m);
}

static void try_receive(mpeg_native *m)
{
    if (!m->have_frame && avcodec_receive_frame(m->ctx, m->frame) == 0)
        m->have_frame = 1;
}

/* Feeds elementary stream bytes; returns the number consumed (all of them
 * unless a decoded picture is waiting to be taken). */
size_t mpn_feed(mpeg_native *m, const uint8_t *data, size_t size)
{
    size_t used = 0;
    while (used < size) {
        try_receive(m);
        if (m->have_frame)
            break;
        uint8_t *out = NULL;
        int out_size = 0;
        int n = av_parser_parse2(m->parser, m->ctx, &out, &out_size, data + used, (int)(size - used),
                                 AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (n < 0)
            break;
        used += (size_t)n;
        if (out_size) {
            m->pkt->data = out;
            m->pkt->size = out_size;
            avcodec_send_packet(m->ctx, m->pkt);
        }
    }
    try_receive(m);
    return used;
}

/* End of stream: flush the parser and the decoder's delayed pictures. */
void mpn_flush(mpeg_native *m)
{
    uint8_t *out = NULL;
    int out_size = 0;
    av_parser_parse2(m->parser, m->ctx, &out, &out_size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (out_size) {
        m->pkt->data = out;
        m->pkt->size = out_size;
        avcodec_send_packet(m->ctx, m->pkt);
    }
    avcodec_send_packet(m->ctx, NULL);
    try_receive(m);
}

int mpn_has_picture(mpeg_native *m)
{
    try_receive(m);
    return m->have_frame;
}

/* Writes the waiting picture as RAW8 macroblocks (at most max_mbs). */
int mpn_take_raw8(mpeg_native *m, uint8_t *out, int max_mbs, int *width, int *height)
{
    try_receive(m);
    if (!m->have_frame)
        return 0;
    AVFrame *f = m->frame;
    int mbw = (f->width + 15) / 16, mbh = (f->height + 15) / 16, n = 0;
    /* Column-major: libmpeg writes macroblocks top to bottom, then the next
     * column (measured against the real IPU output, see NATIVE_PLAN.md). */
    for (int mx = 0; mx < mbw; mx++)
        for (int my = 0; my < mbh && n < max_mbs; my++, n++) {
            uint8_t *mb = out + (size_t)n * 384;
            for (int y = 0; y < 16; y++) {
                int sy = my * 16 + y < f->height ? my * 16 + y : f->height - 1;
                memcpy(mb + y * 16, f->data[0] + (size_t)sy * f->linesize[0] + mx * 16, 16);
            }
            for (int y = 0; y < 8; y++) {
                int sy = my * 8 + y < (f->height + 1) / 2 ? my * 8 + y : (f->height + 1) / 2 - 1;
                memcpy(mb + 256 + y * 8, f->data[1] + (size_t)sy * f->linesize[1] + mx * 8, 8);
                memcpy(mb + 320 + y * 8, f->data[2] + (size_t)sy * f->linesize[2] + mx * 8, 8);
            }
        }
    if (width)
        *width = f->width;
    if (height)
        *height = f->height;
    m->last_type = f->pict_type == AV_PICTURE_TYPE_I ? 1 : f->pict_type == AV_PICTURE_TYPE_P ? 2 : 3;
    m->have_frame = 0;
    av_frame_unref(f);
    return n;
}

int mpn_last_picture_type(mpeg_native *m) { return m->last_type; }

void mpn_reset(mpeg_native *m)
{
    avcodec_flush_buffers(m->ctx);
    av_parser_close(m->parser);
    m->parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    m->have_frame = 0;
}
