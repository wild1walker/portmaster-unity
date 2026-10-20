// Clips for the tests, made here: frames and tones encoded with the encoders of the static
// libraries the media core links (libvpx VP8/VP9, libvorbisenc, libopus) and muxed by webm_writer.c.
#define _GNU_SOURCE
#include "clips.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include <vorbis/vorbisenc.h>
#include <opus/opus.h>

/* ---- synthetic content -------------------------------------------------------------------------- */
uint8_t pix_y(int x, int y, int f) { int v = 40 + (x * 3 + y * 2 + f * 5) % 160; if (x > 10 + f && x < 30 + f && y > 8 && y < 24) v = 230; return v; }
uint8_t pix_u(int x, int y, int f) { return 64 + (x * 4 + f) % 128; }
uint8_t pix_v(int x, int y, int f) { return 200 - (y * 4 + f) % 128; }

void padd(plist *l, const void *d, size_t n, int64_t ms, int key)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->p = realloc(l->p, l->cap * sizeof *l->p); }
    packet *p = &l->p[l->n++];
    p->data = malloc(n ? n : 1); memcpy(p->data, d, n); p->size = n; p->ms = ms; p->key = key;
}

plist encode_vpx(int vp9, int w, int h, int frames)
{
    plist l = { 0 };
    vpx_codec_iface_t *ifc = vp9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();
    vpx_codec_enc_cfg_t cfg;
    vpx_codec_enc_config_default(ifc, &cfg, 0);
    cfg.g_w = w; cfg.g_h = h;
    cfg.g_timebase.num = 1; cfg.g_timebase.den = 1000;
    cfg.rc_target_bitrate = 4000;
    cfg.rc_min_quantizer = 0; cfg.rc_max_quantizer = 8;
    cfg.kf_max_dist = 10; cfg.kf_min_dist = 10;
    cfg.g_lag_in_frames = 0;
    cfg.g_threads = 1;
    vpx_codec_ctx_t enc;
    if (vpx_codec_enc_init(&enc, ifc, &cfg, 0)) { printf("encoder init failed\n"); exit(1); }
    vpx_image_t img;
    vpx_img_alloc(&img, VPX_IMG_FMT_I420, w, h, 1);
    for (int f = 0; f <= frames; f++) {
        if (f < frames) {
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) img.planes[0][y * img.stride[0] + x] = pix_y(x, y, f);
            for (int y = 0; y < (h + 1) / 2; y++) for (int x = 0; x < (w + 1) / 2; x++) {
                img.planes[1][y * img.stride[1] + x] = pix_u(x, y, f);
                img.planes[2][y * img.stride[2] + x] = pix_v(x, y, f);
            }
        }
        int64_t pts = f * 1000 / 30;
        vpx_codec_encode(&enc, f < frames ? &img : NULL, pts, 33, (f % 10 == 0) ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_GOOD_QUALITY);
        vpx_codec_iter_t it = NULL;
        const vpx_codec_cx_pkt_t *pk;
        while ((pk = vpx_codec_get_cx_data(&enc, &it)))
            if (pk->kind == VPX_CODEC_CX_FRAME_PKT)
                padd(&l, pk->data.frame.buf, pk->data.frame.sz, pk->data.frame.pts, (pk->data.frame.flags & VPX_FRAME_IS_KEY) != 0);
    }
    vpx_img_free(&img);
    vpx_codec_destroy(&enc);
    return l;
}

float tone(int ch, int64_t i) { return 0.5f * sinf(2 * (float)M_PI * (ch ? 660 : 440) * i / AR); }

plist encode_vorbis(int seconds, uint8_t **priv, size_t *privn)
{
    plist l = { 0 };
    vorbis_info vi; vorbis_info_init(&vi);
    vorbis_encode_init_vbr(&vi, 2, AR, 0.5f);
    vorbis_comment vc; vorbis_comment_init(&vc);
    vorbis_dsp_state vd; vorbis_analysis_init(&vd, &vi);
    vorbis_block vb; vorbis_block_init(&vd, &vb);
    ogg_packet h[3];
    vorbis_analysis_headerout(&vd, &vc, &h[0], &h[1], &h[2]);
    wb p = { 0 };
    const uint8_t *pk[3] = { h[0].packet, h[1].packet, h[2].packet };
    size_t pn[3] = { h[0].bytes, h[1].bytes, h[2].bytes };
    wb_xiph_private(&p, pk, pn, 3);
    *priv = p.p; *privn = p.n;
    int64_t total = (int64_t)seconds * AR, done = 0, prev_gp = 0;
    for (;;) {
        if (done < total) {
            int n = 1024;
            float **buf = vorbis_analysis_buffer(&vd, n);
            for (int i = 0; i < n; i++) { buf[0][i] = tone(0, done + i); buf[1][i] = tone(1, done + i); }
            vorbis_analysis_wrote(&vd, n);
            done += n;
        } else vorbis_analysis_wrote(&vd, 0);
        int got = 0;
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                padd(&l, op.packet, op.bytes, prev_gp * 1000 / AR, 1);
                if (op.granulepos >= 0) prev_gp = op.granulepos;
                got = 1;
                if (op.e_o_s) goto end;
            }
        }
        if (done >= total && !got) break;
    }
end:
    vorbis_block_clear(&vb); vorbis_dsp_clear(&vd); vorbis_comment_clear(&vc); vorbis_info_clear(&vi);
    return l;
}

plist encode_opus(int seconds, int *preskip)
{
    plist l = { 0 };
    int err;
    OpusEncoder *oe = opus_encoder_create(AR, 2, OPUS_APPLICATION_AUDIO, &err);
    opus_encoder_ctl(oe, OPUS_GET_LOOKAHEAD(preskip));
    int16_t pcm[960 * 2];
    uint8_t out[4000];
    for (int64_t s = 0; s < (int64_t)seconds * AR; s += 960) {
        for (int i = 0; i < 960; i++) { pcm[i * 2] = (int16_t)(tone(0, s + i) * 32767); pcm[i * 2 + 1] = (int16_t)(tone(1, s + i) * 32767); }
        int n = opus_encode(oe, pcm, 960, out, sizeof out);
        padd(&l, out, n, s * 1000 / AR, 1);
    }
    opus_encoder_destroy(oe);
    return l;
}

/* ---- muxing ----------------------------------------------------------------------------------------- */

/* clusters of 500 ms; audio packets of the second cluster laced (Xiph, then EBML) when lace */
wb mux_webm(mtrack *t, int nt, double duration_ms, int lace)
{
    wb out = { 0 };
    wb_header(&out);
    wb seg = { 0 }, info = { 0 }, tracks = { 0 };
    wb_uint(&info, 0x2AD7B1, 1000000);
    wb_float(&info, 0x4489, duration_ms);
    wb_str(&info, 0x4D80, "test"); wb_str(&info, 0x5741, "test");
    wb_master(&seg, 0x1549A966, &info);
    for (int i = 0; i < nt; i++) {
        wb te = { 0 }, sub = { 0 };
        wb_uint(&te, 0xD7, i + 1);
        wb_uint(&te, 0x73C5, i + 100);
        wb_uint(&te, 0x83, t[i].audio ? 2 : 1);
        wb_str(&te, 0x86, t[i].codec);
        if (t[i].priv) wb_elem(&te, 0x63A2, t[i].priv, t[i].privn);
        if (t[i].delay_ns) wb_uint(&te, 0x56AA, t[i].delay_ns);
        if (t[i].audio) {
            wb_float(&sub, 0xB5, AR); wb_uint(&sub, 0x9F, 2);
            wb_master(&te, 0xE1, &sub);
        } else {
            wb_uint(&sub, 0xB0, t[i].w); wb_uint(&sub, 0xBA, t[i].h);
            if (t[i].fps > 0) wb_float4(&sub, 0x2383E3, t[i].fps);
            wb_master(&te, 0xE0, &sub);
        }
        wb_master(&tracks, 0xAE, &te);
    }
    wb_master(&seg, 0x1654AE6B, &tracks);
    int pos[8] = { 0 };
    for (int64_t c0 = 0; ; c0 += 500) {
        int any = 0;
        for (int i = 0; i < nt; i++) if (pos[i] < t[i].pk->n) any = 1;
        if (!any) break;
        wb cl = { 0 };
        wb_uint(&cl, 0xE7, c0);
        for (;;) {           /* interleave by time within the cluster */
            int best = -1;
            for (int i = 0; i < nt; i++)
                if (pos[i] < t[i].pk->n && t[i].pk->p[pos[i]].ms < c0 + 500 &&
                    (best < 0 || t[i].pk->p[pos[i]].ms < t[best].pk->p[pos[best]].ms)) best = i;
            if (best < 0) break;
            packet *p = &t[best].pk->p[pos[best]];
            if (lace && t[best].audio && c0 == 500 && pos[best] + 3 < t[best].pk->n) {
                const uint8_t *fr[4]; size_t sz[4];
                int n = 0;
                while (n < 4 && pos[best] < t[best].pk->n && t[best].pk->p[pos[best]].ms < c0 + 500) {
                    fr[n] = t[best].pk->p[pos[best]].data; sz[n] = t[best].pk->p[pos[best]].size; n++; pos[best]++;
                }
                static int which;
                wb_block(&cl, best + 1, (int)(p->ms - c0), 1, fr, sz, n, (which++ & 1) ? 3 : 1);
                continue;
            }
            const uint8_t *fr = p->data;
            wb_block(&cl, best + 1, (int)(p->ms - c0), p->key, &fr, &p->size, 1, 0);
            pos[best]++;
        }
        wb_master(&seg, 0x1F43B675, &cl);
    }
    wb_master(&out, 0x18538067, &seg);
    return out;
}

/* the clip inside a bigger file at an offset, as a clip inside an APK */
int write_embedded(const char *path, const wb *clip, int64_t *off)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    uint8_t junk[1234];
    memset(junk, 0x5A, sizeof junk);
    *off = sizeof junk;
    if (write(fd, junk, sizeof junk) != sizeof junk || write(fd, clip->p, clip->n) != (ssize_t)clip->n ||
        write(fd, junk, 100) != 100) { close(fd); return -1; }
    return fd;
}

