/**************************************************************************
 *
 * Copyright (C) 2026 WINQ-EMU contributors.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * @file
 * Windows D3D11 Video Decoder implementation of the virgl_video.h interface.
 *
 * This backend mirrors what src/vrend/virgl_video.c does with libva, but on
 * Windows hosts we route the work through the D3D11 Video Decoder API (DXVA
 * via ID3D11VideoDevice / ID3D11VideoContext) and deliver the decoded NV12
 * pixels to the caller as a CPU-accessible plane set (no DMA-BUF). The guest
 * VM still talks to the host through Mesa's gallium/drivers/virgl VA-API
 * frontend exactly as on Linux; only the host-side decode is different.
 *
 * Scope for this first pass:
 *   - H.264 decode only (all common profiles, FGT disabled, NV12 output).
 *   - No encode (virgl_video_encode_bitstream() returns -ENOSYS).
 *   - CPU readback output through a D3D11_USAGE_STAGING NV12 texture; no
 *     WGL_NV_DX_interop2 zero-copy yet. vrend_video.c uploads the NV12 bytes
 *     into the virgl resource with glTexSubImage2D.
 *
 * Extension points marked with "TODO future codec" keep room for HEVC / VP9 /
 * AV1 / MPEG2 without reshuffling the plumbing.
 *
 * References:
 *   - [MS-DXVA2]: "DirectX Video Acceleration 2.0" spec.
 *   - DXVA H.264 restricted profile (DXVA_PicParams_H264, DXVA_Qmatrix_H264,
 *     DXVA_Slice_H264_Short) in <dxva.h>.
 *   - ffmpeg/libavcodec/dxva2_h264.c for a cross-check on field packing.
 */

#include "config.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

/* COBJMACROS must be defined *before* <d3d11.h> so the IDL-generated header
 * exposes flat COM accessors like ID3D11VideoDevice_CreateVideoDecoder(). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxva.h>

#include "pipe/p_video_enums.h"
#include "util/u_formats.h"
#include "util/macros.h"

#include "virgl_hw.h"
#include "virgl_video_hw.h"
#include "virgl_util.h"
#include "virgl_video.h"

/* Some minimal toolchains don't pull this through dxva.h. It's the DXVA
 * short-format-bitstream marker used in wBadSliceChopping. */
#ifndef DXVA_SLICE_CHOPPING_NONE
#define DXVA_SLICE_CHOPPING_NONE 0
#endif

/* The DXVA spec caps slice controls per SubmitDecoderBuffers call; 256 is
 * well above what Mesa's H.264 frontend will hand us. */
#define VIRGL_VIDEO_WIN32_MAX_SLICES 256

/* Upper bound on how many reference frames we will track per codec. The DXVA
 * H.264 RefFrameList is 16 long, which is also the max in the H.264 spec. */
#define VIRGL_VIDEO_WIN32_MAX_REFS 16

/*
 * ---------------------------------------------------------------------------
 * Opaque types.
 * ---------------------------------------------------------------------------
 */

struct virgl_video_buffer {
    /* Guest-facing metadata (kept in sync with the libva variant for parity). */
    enum pipe_format format;
    uint32_t width;
    uint32_t height;
    bool interlaced;

    /* Opaque cookie passed by vrend_video.c so it can find its wrapper from a
     * virgl_video_buffer pointer. */
    void *opaque;

    /* A stable integer id the guest uses to refer to this buffer. We use the
     * address cast to uint32_t since we don't maintain a surface-id table. */
    uint32_t id;

    /* The decoder-writable NV12 texture. Created with D3D11_BIND_DECODER and
     * a single array slice so the ID3D11VideoDecoderOutputView can point at
     * it directly. */
    ID3D11Texture2D *decode_tex;
    ID3D11VideoDecoderOutputView *decode_view;

    /* Single NV12 staging texture we Map to read Y+UV data back. Y lives
     * at mapped.pData, UV at an offset the driver chooses (queried via
     * DepthPitch on drivers that honour the planar-subresource layout). */
    ID3D11Texture2D *staging_tex;

    /* Latched CPU pointers from the most recent Map(). They are only valid
     * between end_frame() and the next decode call; vrend_video.c copies out
     * the bytes in the decode_completed callback before we unmap. */
    bool staging_mapped;
    D3D11_MAPPED_SUBRESOURCE mapped_y;
    D3D11_MAPPED_SUBRESOURCE mapped_uv;

    /* Used by the decode_completed callback. We populate this once per decode
     * and pass it through virgl_video_callbacks. */
    struct virgl_video_dma_buf dmabuf;
};

struct virgl_video_codec {
    /* Parameters mirrored from create_codec_args. */
    enum pipe_video_profile profile;
    enum pipe_video_entrypoint entrypoint;
    enum pipe_video_chroma_format chroma_format;
    uint32_t level;
    uint32_t width;
    uint32_t height;
    uint32_t max_references;
    uint32_t flags;
    void *opaque;

    /* DXVA profile GUID matched during create. */
    GUID dxva_profile;

    /* D3D11 decoder created with CreateVideoDecoder(). */
    ID3D11VideoDecoder *decoder;
    D3D11_VIDEO_DECODER_CONFIG config;

    /* Per-codec decode state. Updated on begin_frame()/decode_bitstream(). */
    struct virgl_video_buffer *target;

    /* StatusReportFeedbackNumber must be a nonzero monotonically increasing
     * value per DecoderBeginFrame; 0 is reserved by the DXVA spec. */
    uint32_t status_report_feedback;

    /* Reference-frame tracking. We map each ref frame's guest-visible id (the
     * 32-bit handle Mesa passes in desc->buffer_id[]) to the backing NV12
     * texture's array slice index, so the decoder knows which slots in its
     * RefFrameList correspond to which prior decoded pictures. */
    struct {
        uint32_t buffer_id;            /* Guest ref id; 0 when slot unused */
        struct virgl_video_buffer *buf;
    } refs[VIRGL_VIDEO_WIN32_MAX_REFS];
};

/*
 * ---------------------------------------------------------------------------
 * Global state (mirrors libva backend's va_dpy + callbacks).
 * ---------------------------------------------------------------------------
 */

static struct {
    bool initialized;

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11VideoDevice *video_device;
    ID3D11VideoContext *video_context;

    D3D_FEATURE_LEVEL feature_level;
    struct virgl_video_callbacks *callbacks;
} g_vid;

/*
 * ---------------------------------------------------------------------------
 * Helpers.
 * ---------------------------------------------------------------------------
 */

/* All H.264 profiles accepted by Mesa's VA-API frontend map to the same DXVA
 * GUID. High-10 / 422 / 444 would need a different GUID and a 10-bit output
 * format (P010), so we reject those for this first pass. */
static bool is_supported_h264_profile(enum pipe_video_profile profile)
{
    switch (profile) {
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
        return true;
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH10:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH422:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH444:
        /* TODO future codec: add D3D11_DECODER_PROFILE_H264_VLD_WITHFMOASO
         * / _VLD_STEREO_PROGRESSIVE variants and P010 output for HIGH_10. */
        return false;
    default:
        return false;
    }
}

static bool map_profile_to_dxva_guid(enum pipe_video_profile profile,
                                     GUID *out)
{
    if (is_supported_h264_profile(profile)) {
        *out = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
        return true;
    }
    /* TODO future codec: HEVC (D3D11_DECODER_PROFILE_HEVC_VLD_MAIN), VP9,
     * AV1, MPEG2. */
    return false;
}

static DXGI_FORMAT dxgi_format_from_pipe(enum pipe_format f)
{
    /*
     * D3D11 video decode output is almost always NV12 regardless of what
     * gallium format the guest requested for the surface. Map the common
     * 4:2:0 pipe formats (NV12, I420/IYUV, YV12, NV21) to DXGI_FORMAT_NV12
     * and let the cpu-readback path repack into the guest's plane layout.
     *
     * The libva-based path on Linux does the same thing: it accepts any
     * pipe_format and just allocates VA_RT_FORMAT_YUV420 surfaces.
     */
    switch (f) {
    case PIPE_FORMAT_NV12:
    case PIPE_FORMAT_NV21:
    case PIPE_FORMAT_IYUV:   /* aka PIPE_FORMAT_Y8_U8_V8_420_UNORM / I420 */
    case PIPE_FORMAT_YV12:
        return DXGI_FORMAT_NV12;
    case PIPE_FORMAT_P010:
        return DXGI_FORMAT_P010;
    case PIPE_FORMAT_YUYV:
        return DXGI_FORMAT_YUY2;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

/* DRM FourCC used by the existing dma-buf API. On Windows there's no actual
 * dma-buf, but vrend_video.c looks at this field to find the right pipe
 * format, so we fill it in with the Linux equivalents. */
static uint32_t drm_fourcc_nv12(void)
{
    /* DRM_FORMAT_NV12 = fourcc_code('N','V','1','2') */
    return (uint32_t)'N' | ((uint32_t)'V' << 8) |
           ((uint32_t)'1' << 16) | ((uint32_t)'2' << 24);
}

/*
 * ---------------------------------------------------------------------------
 * Init / teardown.
 * ---------------------------------------------------------------------------
 */

int virgl_video_init(int drm_fd,
                     struct virgl_video_callbacks *cbs,
                     unsigned int flags)
{
    HRESULT hr;
    UINT device_flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    (void)drm_fd;   /* Windows backend doesn't use DRM fds. */
    (void)flags;

    if (g_vid.initialized) {
        virgl_warn("virgl_video_win32: already initialized\n");
        return 0;
    }

    memset(&g_vid, 0, sizeof(g_vid));

    hr = D3D11CreateDevice(NULL,
                           D3D_DRIVER_TYPE_HARDWARE,
                           NULL,
                           device_flags,
                           levels, ARRAY_SIZE(levels),
                           D3D11_SDK_VERSION,
                           &g_vid.device,
                           &g_vid.feature_level,
                           &g_vid.context);
    if (FAILED(hr)) {
        /* Fallback for CI machines without a GPU video driver: try WARP. */
        hr = D3D11CreateDevice(NULL,
                               D3D_DRIVER_TYPE_WARP,
                               NULL,
                               device_flags,
                               levels, ARRAY_SIZE(levels),
                               D3D11_SDK_VERSION,
                               &g_vid.device,
                               &g_vid.feature_level,
                               &g_vid.context);
        if (FAILED(hr)) {
            virgl_error("virgl_video_win32: D3D11CreateDevice failed: 0x%lx\n",
                        (unsigned long)hr);
            return -1;
        }
    }

    hr = ID3D11Device_QueryInterface(g_vid.device, &IID_ID3D11VideoDevice,
                                     (void **)&g_vid.video_device);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: QI(ID3D11VideoDevice) failed: 0x%lx\n",
                    (unsigned long)hr);
        goto fail;
    }

    hr = ID3D11DeviceContext_QueryInterface(g_vid.context,
                                            &IID_ID3D11VideoContext,
                                            (void **)&g_vid.video_context);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: QI(ID3D11VideoContext) failed: 0x%lx\n",
                    (unsigned long)hr);
        goto fail;
    }

    g_vid.callbacks = cbs;
    g_vid.initialized = true;

    virgl_info("virgl_video_win32: initialized (feature level 0x%x)\n",
               (unsigned)g_vid.feature_level);
    return 0;

fail:
    virgl_video_destroy();
    return -1;
}

void virgl_video_destroy(void)
{
    if (g_vid.video_context) {
        ID3D11VideoContext_Release(g_vid.video_context);
        g_vid.video_context = NULL;
    }
    if (g_vid.video_device) {
        ID3D11VideoDevice_Release(g_vid.video_device);
        g_vid.video_device = NULL;
    }
    if (g_vid.context) {
        ID3D11DeviceContext_Release(g_vid.context);
        g_vid.context = NULL;
    }
    if (g_vid.device) {
        ID3D11Device_Release(g_vid.device);
        g_vid.device = NULL;
    }
    g_vid.callbacks = NULL;
    g_vid.initialized = false;
}

/*
 * ---------------------------------------------------------------------------
 * virgl_video_fill_caps.
 *
 * We enumerate profiles offered by the D3D11 video device, keep only the ones
 * we actually know how to translate to pipe_video_profile, and write them
 * into caps->v2.video_caps[]. Mesa uses this to decide which VA-API profiles
 * to expose to the guest.
 * ---------------------------------------------------------------------------
 */

static bool profile_guid_matches(const GUID *a, const GUID *b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static void fill_caps_for_h264(struct virgl_video_caps *v,
                               enum pipe_video_profile profile)
{
    v->profile = profile;
    v->entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
    v->max_level = 51;              /* H.264 level 5.1 is fine for 4K@30 */
    v->stacked_frames = 0;
    v->max_width = 3840;
    v->max_height = 2160;
    v->prefered_format = PIPE_FORMAT_NV12;
    /* max_macroblocks = (3840/16) * (2160/16) = 240 * 135 = 32400. */
    v->max_macroblocks = 240 * 135;
    v->npot_texture = 1;
    v->supports_progressive = 1;
    v->supports_interlaced = 0;
    v->prefers_interlaced = 0;
    v->max_temporal_layers = 0;
}

int virgl_video_fill_caps(union virgl_caps *caps)
{
    UINT i, profile_count;
    GUID guid;
    bool have_h264_nofgt = false;
    unsigned out = 0;

    if (!g_vid.initialized || !caps)
        return -1;

    profile_count = ID3D11VideoDevice_GetVideoDecoderProfileCount(
                            g_vid.video_device);
    for (i = 0; i < profile_count; i++) {
        if (FAILED(ID3D11VideoDevice_GetVideoDecoderProfile(
                        g_vid.video_device, i, &guid)))
            continue;

        if (profile_guid_matches(&guid,
                                 &D3D11_DECODER_PROFILE_H264_VLD_NOFGT)) {
            WINBOOL nv12_ok = FALSE;
            HRESULT hr = ID3D11VideoDevice_CheckVideoDecoderFormat(
                            g_vid.video_device, &guid, DXGI_FORMAT_NV12,
                            &nv12_ok);
            if (SUCCEEDED(hr) && nv12_ok)
                have_h264_nofgt = true;
        }
    }

    caps->v2.num_video_caps = 0;

    if (have_h264_nofgt) {
        /* Advertise the same H.264 profile set Mesa VA-API exposes. They all
         * decode through the same _NOFGT GUID on D3D11. */
        static const enum pipe_video_profile profiles[] = {
            PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE,
            PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE,
            PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN,
            PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED,
            PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
        };
        for (i = 0; i < ARRAY_SIZE(profiles) &&
                    out < ARRAY_SIZE(caps->v2.video_caps); i++) {
            fill_caps_for_h264(&caps->v2.video_caps[out++], profiles[i]);
        }
    }

    caps->v2.num_video_caps = out;
    return 0;
}

/*
 * ---------------------------------------------------------------------------
 * Codec lifecycle.
 * ---------------------------------------------------------------------------
 */

static int pick_decoder_config(struct virgl_video_codec *codec,
                               const D3D11_VIDEO_DECODER_DESC *desc,
                               D3D11_VIDEO_DECODER_CONFIG *out_cfg)
{
    UINT count = 0, i;
    HRESULT hr;

    hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(
                    g_vid.video_device, desc, &count);
    if (FAILED(hr) || count == 0) {
        virgl_error("virgl_video_win32: no decoder configs for profile "
                    "(hr=0x%lx, count=%u)\n", (unsigned long)hr, count);
        return -1;
    }

    /* Prefer the first config that uses short-format bitstream and doesn't
     * require encrypted input; otherwise fall back to the first one. We
     * detect "short format" via ConfigBitstreamRaw == 2 per DXVA spec. In
     * practice nearly all modern drivers report config 0 as a short-format
     * non-encrypted H.264 setup, but being explicit documents intent. */
    for (i = 0; i < count; i++) {
        D3D11_VIDEO_DECODER_CONFIG cfg;
        hr = ID3D11VideoDevice_GetVideoDecoderConfig(
                        g_vid.video_device, desc, i, &cfg);
        if (FAILED(hr))
            continue;
        (void)codec;
        /* ConfigBitstreamRaw:
         *   0 => legacy long-format (MBctrl / Residual paths).
         *   1 => long-format bitstream.
         *   2 => short-format bitstream. <-- what we want.
         * Some drivers report 0 even for working H.264; we tolerate that by
         * picking the first valid one if none claim short format. */
        if (cfg.ConfigBitstreamRaw == 2) {
            *out_cfg = cfg;
            return 0;
        }
    }

    /* Fallback: take index 0. */
    hr = ID3D11VideoDevice_GetVideoDecoderConfig(
                    g_vid.video_device, desc, 0, out_cfg);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: GetVideoDecoderConfig(0) failed: "
                    "0x%lx\n", (unsigned long)hr);
        return -1;
    }
    return 0;
}

struct virgl_video_codec *virgl_video_create_codec(
        const struct virgl_video_create_codec_args *args)
{
    struct virgl_video_codec *codec;
    D3D11_VIDEO_DECODER_DESC desc;
    HRESULT hr;

    if (!g_vid.initialized || !args)
        return NULL;

    if (args->entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM) {
        /* Encode is explicitly out of scope for this first pass. */
        virgl_warn("virgl_video_win32: entrypoint %d unsupported\n",
                   (int)args->entrypoint);
        return NULL;
    }

    codec = calloc(1, sizeof(*codec));
    if (!codec)
        return NULL;

    codec->profile = args->profile;
    codec->entrypoint = args->entrypoint;
    codec->chroma_format = args->chroma_format;
    codec->level = args->level;
    codec->width = args->width;
    codec->height = args->height;
    codec->max_references = args->max_references;
    codec->flags = args->flags;
    codec->opaque = args->opaque;
    codec->status_report_feedback = 1;

    if (!map_profile_to_dxva_guid(args->profile, &codec->dxva_profile)) {
        virgl_error("virgl_video_win32: unsupported profile %d\n",
                    (int)args->profile);
        goto fail;
    }

    memset(&desc, 0, sizeof(desc));
    desc.Guid = codec->dxva_profile;
    desc.SampleWidth = args->width;
    desc.SampleHeight = args->height;
    desc.OutputFormat = DXGI_FORMAT_NV12;

    if (pick_decoder_config(codec, &desc, &codec->config) != 0)
        goto fail;

    hr = ID3D11VideoDevice_CreateVideoDecoder(g_vid.video_device,
                                              &desc, &codec->config,
                                              &codec->decoder);
    if (FAILED(hr) || !codec->decoder) {
        virgl_error("virgl_video_win32: CreateVideoDecoder failed: 0x%lx\n",
                    (unsigned long)hr);
        goto fail;
    }

    return codec;

fail:
    virgl_video_destroy_codec(codec);
    return NULL;
}

void virgl_video_destroy_codec(struct virgl_video_codec *codec)
{
    if (!codec)
        return;
    if (codec->decoder) {
        ID3D11VideoDecoder_Release(codec->decoder);
        codec->decoder = NULL;
    }
    free(codec);
}

enum pipe_video_profile virgl_video_codec_profile(
        const struct virgl_video_codec *codec)
{
    return codec ? codec->profile : PIPE_VIDEO_PROFILE_UNKNOWN;
}

void *virgl_video_codec_opaque_data(struct virgl_video_codec *codec)
{
    return codec ? codec->opaque : NULL;
}

/*
 * ---------------------------------------------------------------------------
 * Video buffers. Each buffer owns:
 *   - decode_tex : BIND_DECODER NV12 texture, the actual DXVA output surface.
 *   - decode_view: VideoDecoderOutputView pointed at decode_tex array slice 0.
 *   - staging_tex: USAGE_STAGING, CPU-readable NV12 texture; copy target for
 *                  the post-decode readback.
 * ---------------------------------------------------------------------------
 */

static void unmap_staging_if_needed(struct virgl_video_buffer *buf)
{
    if (buf->staging_mapped && g_vid.context && buf->staging_tex) {
        ID3D11DeviceContext_Unmap(g_vid.context,
                                  (ID3D11Resource *)buf->staging_tex, 0);
        buf->staging_mapped = false;
        memset(&buf->mapped_y,  0, sizeof(buf->mapped_y));
        memset(&buf->mapped_uv, 0, sizeof(buf->mapped_uv));
    }
}

struct virgl_video_buffer *virgl_video_create_buffer(
        const struct virgl_video_create_buffer_args *args)
{
    struct virgl_video_buffer *buf;
    D3D11_TEXTURE2D_DESC tex;
    HRESULT hr;
    DXGI_FORMAT fmt;

    if (!g_vid.initialized || !args)
        return NULL;

    fmt = dxgi_format_from_pipe(args->format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        virgl_error("virgl_video_win32: unsupported pipe format %d\n",
                    (int)args->format);
        return NULL;
    }

    /* D3D11 requires NV12 dimensions to be even. Round up defensively; the
     * guest should have aligned already but bad input shouldn't crash us. */
    if ((args->width & 1) || (args->height & 1)) {
        virgl_warn("virgl_video_win32: odd buffer dims %ux%u; rounding up\n",
                   args->width, args->height);
    }

    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    buf->format = args->format;
    buf->width = (args->width + 1) & ~1u;
    buf->height = (args->height + 1) & ~1u;
    buf->interlaced = args->interlaced;
    buf->opaque = args->opaque;
    buf->id = (uint32_t)(uintptr_t)buf;   /* stable while buf lives */

    /* Decoder-writable NV12 texture. BIND_DECODER is the critical flag; a
     * single array slice keeps slot 0 addressing simple. */
    memset(&tex, 0, sizeof(tex));
    tex.Width = buf->width;
    tex.Height = buf->height;
    tex.MipLevels = 1;
    tex.ArraySize = 1;
    tex.Format = fmt;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_DEFAULT;
    tex.BindFlags = D3D11_BIND_DECODER;
    tex.CPUAccessFlags = 0;
    tex.MiscFlags = 0;

    hr = ID3D11Device_CreateTexture2D(g_vid.device, &tex, NULL,
                                      &buf->decode_tex);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: CreateTexture2D(decode) "
                    "%ux%u failed: 0x%lx\n",
                    buf->width, buf->height, (unsigned long)hr);
        goto fail;
    }

    /* Single NV12 staging texture matching the decoder output layout. */
    memset(&tex, 0, sizeof(tex));
    tex.Width = buf->width;
    tex.Height = buf->height;
    tex.MipLevels = 1;
    tex.ArraySize = 1;
    tex.Format = fmt;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_STAGING;
    tex.BindFlags = 0;
    tex.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    tex.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_vid.device, &tex, NULL,
                                      &buf->staging_tex);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: CreateTexture2D(staging NV12) "
                    "%ux%u failed: 0x%lx\n",
                    buf->width, buf->height, (unsigned long)hr);
        goto fail;
    }

    /* The VideoDecoderOutputView is created lazily in begin_frame(); we need
     * the decoder object for that, which the codec may not exist yet at this
     * point (buffers and codecs are created independently by vrend_video). */
    buf->decode_view = NULL;

    return buf;

fail:
    virgl_video_destroy_buffer(buf);
    return NULL;
}

void virgl_video_destroy_buffer(struct virgl_video_buffer *buffer)
{
    if (!buffer)
        return;

    unmap_staging_if_needed(buffer);

    if (buffer->decode_view) {
        ID3D11VideoDecoderOutputView_Release(buffer->decode_view);
        buffer->decode_view = NULL;
    }
    if (buffer->decode_tex) {
        ID3D11Texture2D_Release(buffer->decode_tex);
        buffer->decode_tex = NULL;
    }
    if (buffer->staging_tex) {
        ID3D11Texture2D_Release(buffer->staging_tex);
        buffer->staging_tex = NULL;
    }
    free(buffer);
}

uint32_t virgl_video_buffer_id(const struct virgl_video_buffer *buffer)
{
    return buffer ? buffer->id : 0;
}

void *virgl_video_buffer_opaque_data(struct virgl_video_buffer *buffer)
{
    return buffer ? buffer->opaque : NULL;
}

/*
 * ---------------------------------------------------------------------------
 * Reference-frame bookkeeping.
 *
 * The DXVA H.264 RefFrameList[16] stores DXVA_PicEntry_H264 entries with an
 * Index7Bits slot number and an AssociatedFlag bit. Driver-wise, the slot
 * number is just an index the host decoder agreed on with itself: we use it
 * as an opaque tag that must be distinct per in-flight reference frame. We
 * keep our own codec->refs[] table and look up entries by buffer_id (the
 * guest-side handle Mesa passes in virgl_h264_picture_desc::buffer_id[]).
 * ---------------------------------------------------------------------------
 */

static int codec_find_or_add_ref_slot(struct virgl_video_codec *codec,
                                      uint32_t buffer_id,
                                      struct virgl_video_buffer *buf)
{
    unsigned i, free_slot = UINT_MAX;

    if (buffer_id == 0)
        return -1;

    for (i = 0; i < VIRGL_VIDEO_WIN32_MAX_REFS; i++) {
        if (codec->refs[i].buffer_id == buffer_id) {
            codec->refs[i].buf = buf;
            return (int)i;
        }
        if (codec->refs[i].buffer_id == 0 && free_slot == UINT_MAX)
            free_slot = i;
    }
    if (free_slot == UINT_MAX) {
        /* Evict slot 0; in practice we rarely exceed 16 tracked refs. */
        free_slot = 0;
    }
    codec->refs[free_slot].buffer_id = buffer_id;
    codec->refs[free_slot].buf = buf;
    return (int)free_slot;
}

/*
 * ---------------------------------------------------------------------------
 * begin_frame.
 * ---------------------------------------------------------------------------
 */

static HRESULT ensure_decode_view(struct virgl_video_codec *codec,
                                  struct virgl_video_buffer *buf)
{
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view;

    if (buf->decode_view)
        return S_OK;

    memset(&view, 0, sizeof(view));
    view.DecodeProfile = codec->dxva_profile;
    view.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    view.Texture2D.ArraySlice = 0;

    return ID3D11VideoDevice_CreateVideoDecoderOutputView(
                g_vid.video_device,
                (ID3D11Resource *)buf->decode_tex,
                &view,
                &buf->decode_view);
}

int virgl_video_begin_frame(struct virgl_video_codec *codec,
                            struct virgl_video_buffer *target)
{
    HRESULT hr;

    if (!g_vid.initialized || !codec || !target || !codec->decoder)
        return -1;

    /* If the previous frame's map is still live, release it now. The guest's
     * Mesa driver has already copied out the bytes via the decode_completed
     * callback by the time it sends another begin_frame. */
    unmap_staging_if_needed(target);

    hr = ensure_decode_view(codec, target);
    if (FAILED(hr) || !target->decode_view) {
        virgl_error("virgl_video_win32: CreateVideoDecoderOutputView failed: "
                    "0x%lx\n", (unsigned long)hr);
        return -1;
    }

    hr = ID3D11VideoContext_DecoderBeginFrame(g_vid.video_context,
                                              codec->decoder,
                                              target->decode_view,
                                              0, NULL);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: DecoderBeginFrame failed: 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }

    codec->target = target;
    return 0;
}

/*
 * ---------------------------------------------------------------------------
 * H.264 picture-parameter marshalling.
 *
 * These mirror the VA-API side one-for-one; see the libva backend's
 * h264_fill_picture_param() for the authoritative field meanings. The tricky
 * bits are wBitFields packing and RefFrameList slot/flag encoding, which
 * differ between VA and DXVA conventions.
 * ---------------------------------------------------------------------------
 */

static void dxva_picentry_invalidate(DXVA_PicEntry_H264 *e)
{
    e->Index7Bits = 0x7F;
    e->AssociatedFlag = 0;
}

static void fill_dxva_picparams_h264(struct virgl_video_codec *codec,
                                     struct virgl_video_buffer *target,
                                     const struct virgl_h264_picture_desc *desc,
                                     DXVA_PicParams_H264 *pp)
{
    unsigned i;
    int self_slot;

    memset(pp, 0, sizeof(*pp));

    /* Frame dimensions are in macroblocks minus one; we report the codec's
     * coded size rather than the buffer's because codec->width/height was
     * what the client told us at CreateVideoDecoder time. */
    pp->wFrameWidthInMbsMinus1 = (USHORT)((codec->width + 15) / 16 - 1);
    pp->wFrameHeightInMbsMinus1 = (USHORT)((codec->height + 15) / 16 - 1);

    /* CurrPic: we pack "slot 0 in the output view array" with an associated
     * flag indicating bottom-field when field-coded. */
    self_slot = codec_find_or_add_ref_slot(codec, target->id, target);
    if (self_slot < 0)
        self_slot = 0;
    pp->CurrPic.Index7Bits = (UCHAR)(self_slot & 0x7F);
    pp->CurrPic.AssociatedFlag = desc->field_pic_flag && desc->bottom_field_flag;

    pp->CurrFieldOrderCnt[0] = (INT)desc->field_order_cnt[0];
    pp->CurrFieldOrderCnt[1] = (INT)desc->field_order_cnt[1];
    pp->frame_num = (USHORT)desc->frame_num;
    pp->num_ref_frames = desc->num_ref_frames;

    /* --- bitfields (wBitFields packed USHORT) --- */
    pp->field_pic_flag = desc->field_pic_flag ? 1 : 0;
    pp->MbaffFrameFlag = (desc->pps.sps.mb_adaptive_frame_field_flag &&
                          !desc->field_pic_flag) ? 1 : 0;
    pp->residual_colour_transform_flag =
                          desc->pps.sps.separate_colour_plane_flag ? 1 : 0;
    pp->sp_for_switch_flag = 0;   /* not signalled in picture desc */
    pp->chroma_format_idc = desc->pps.sps.chroma_format_idc;
    pp->RefPicFlag = desc->is_reference ? 1 : 0;
    pp->constrained_intra_pred_flag = desc->pps.constrained_intra_pred_flag;
    pp->weighted_pred_flag = desc->pps.weighted_pred_flag;
    pp->weighted_bipred_idc = desc->pps.weighted_bipred_idc;
    pp->MbsConsecutiveFlag = 1;   /* slices always come in raster order from
                                   * Mesa's VA frontend */
    pp->frame_mbs_only_flag = desc->pps.sps.frame_mbs_only_flag;
    pp->transform_8x8_mode_flag = desc->pps.transform_8x8_mode_flag;
    pp->MinLumaBipredSize8x8Flag = desc->pps.sps.MinLumaBiPredSize8x8;
    pp->IntraPicFlag = 0;         /* unknown ahead of slice headers */

    pp->bit_depth_luma_minus8 = desc->pps.sps.bit_depth_luma_minus8;
    pp->bit_depth_chroma_minus8 = desc->pps.sps.bit_depth_chroma_minus8;
    pp->Reserved16Bits = 3;       /* spec says: set to 3 for DXVA H.264 */

    pp->StatusReportFeedbackNumber = codec->status_report_feedback++;
    if (pp->StatusReportFeedbackNumber == 0)
        pp->StatusReportFeedbackNumber = codec->status_report_feedback++;

    /* Reference list. desc->buffer_id[] holds guest-visible ids for up to 16
     * prior pictures; we translate each to our local slot and mark whether
     * it's a long-term ref. AssociatedFlag = long-term bit per DXVA spec. */
    for (i = 0; i < 16; i++) {
        dxva_picentry_invalidate(&pp->RefFrameList[i]);
        pp->FieldOrderCntList[i][0] = 0;
        pp->FieldOrderCntList[i][1] = 0;
        pp->FrameNumList[i] = 0;
    }
    pp->UsedForReferenceFlags = 0;
    pp->NonExistingFrameFlags = 0;

    for (i = 0; i < desc->num_ref_frames && i < 16; i++) {
        uint32_t bid = desc->buffer_id[i];
        int slot;

        if (bid == 0) {
            pp->NonExistingFrameFlags |= (USHORT)(1u << (i * 1));
            continue;
        }

        slot = codec_find_or_add_ref_slot(codec, bid, NULL);
        if (slot < 0) {
            pp->NonExistingFrameFlags |= (USHORT)(1u << (i * 1));
            continue;
        }
        pp->RefFrameList[i].Index7Bits = (UCHAR)(slot & 0x7F);
        pp->RefFrameList[i].AssociatedFlag = desc->is_long_term[i] ? 1 : 0;
        pp->FieldOrderCntList[i][0] = (INT)desc->field_order_cnt_list[i][0];
        pp->FieldOrderCntList[i][1] = (INT)desc->field_order_cnt_list[i][1];
        pp->FrameNumList[i] = (USHORT)desc->frame_num_list[i];

        /* UsedForReferenceFlags is 2 bits per entry: top + bottom. */
        if (desc->top_is_reference[i])
            pp->UsedForReferenceFlags |= (UINT)(1u << (i * 2 + 0));
        if (desc->bottom_is_reference[i])
            pp->UsedForReferenceFlags |= (UINT)(1u << (i * 2 + 1));
    }

    /* Quantization / deblocking parameters. */
    pp->pic_init_qp_minus26 = desc->pps.pic_init_qp_minus26;
    pp->pic_init_qs_minus26 = desc->pps.pic_init_qs_minus26;
    pp->chroma_qp_index_offset = desc->pps.chroma_qp_index_offset;
    pp->second_chroma_qp_index_offset = desc->pps.second_chroma_qp_index_offset;
    pp->ContinuationFlag = 1;     /* this desc covers all fields below */

    pp->num_ref_idx_l0_active_minus1 = desc->num_ref_idx_l0_active_minus1;
    pp->num_ref_idx_l1_active_minus1 = desc->num_ref_idx_l1_active_minus1;
    pp->Reserved8BitsA = 0;
    pp->Reserved8BitsB = 0;

    pp->log2_max_frame_num_minus4 = desc->pps.sps.log2_max_frame_num_minus4;
    pp->pic_order_cnt_type = desc->pps.sps.pic_order_cnt_type;
    pp->log2_max_pic_order_cnt_lsb_minus4 =
                        desc->pps.sps.log2_max_pic_order_cnt_lsb_minus4;
    pp->delta_pic_order_always_zero_flag =
                        desc->pps.sps.delta_pic_order_always_zero_flag;
    pp->direct_8x8_inference_flag = desc->pps.sps.direct_8x8_inference_flag;
    pp->entropy_coding_mode_flag = desc->pps.entropy_coding_mode_flag;
    pp->pic_order_present_flag =
                desc->pps.bottom_field_pic_order_in_frame_present_flag;
    pp->num_slice_groups_minus1 = desc->pps.num_slice_groups_minus1;
    pp->slice_group_map_type = desc->pps.slice_group_map_type;
    pp->deblocking_filter_control_present_flag =
                desc->pps.deblocking_filter_control_present_flag;
    pp->redundant_pic_cnt_present_flag =
                desc->pps.redundant_pic_cnt_present_flag;
    pp->slice_group_change_rate_minus1 =
                (USHORT)desc->pps.slice_group_change_rate_minus1;
    /* SliceGroupMap[810] is used only when num_slice_groups_minus1 > 0 with
     * slice_group_map_type == 6 — out of scope for H.264 VLD_NOFGT practice.
     */
}

static void fill_dxva_qmatrix_h264(const struct virgl_h264_picture_desc *desc,
                                   DXVA_Qmatrix_H264 *qm)
{
    /*
     * DXVA expects zig-zag scaling lists. Mesa's virgl_h264_pps already
     * provides them in the zig-zag order (matching VA-API VAIQMatrixBufferH264
     * bScalingLists4x4/8x8 semantics), so a straight memcpy is correct.
     */
    memcpy(qm->bScalingLists4x4, desc->pps.ScalingList4x4,
           sizeof(qm->bScalingLists4x4));
    /* DXVA only carries two 8x8 lists (Intra/Inter Y); Mesa has 6 because it
     * also tracks Cb/Cr which aren't used in H.264 8x8 mode. */
    memcpy(qm->bScalingLists8x8[0], desc->pps.ScalingList8x8[0], 64);
    memcpy(qm->bScalingLists8x8[1], desc->pps.ScalingList8x8[1], 64);
}

/*
 * ---------------------------------------------------------------------------
 * decode_bitstream.
 * ---------------------------------------------------------------------------
 */

/* Copy compressed slice data into the decoder's Bitstream buffer. The DXVA
 * spec requires the decoder buffer to start with a 00 00 01 start code, then
 * the NAL unit bytes. Mesa feeds us raw NAL units (with a 4-byte 00 00 00 01
 * prefix already present), so we can just memcpy the payload verbatim. */
static int submit_h264_decode(struct virgl_video_codec *codec,
                              const DXVA_PicParams_H264 *pp,
                              const DXVA_Qmatrix_H264 *qm,
                              unsigned num_buffers,
                              const void * const *buffers,
                              const unsigned *sizes)
{
    HRESULT hr;
    UINT bs_buf_size = 0;
    void *bs_ptr = NULL;
    UINT pp_buf_size = 0;
    void *pp_ptr = NULL;
    UINT iq_buf_size = 0;
    void *iq_ptr = NULL;
    UINT sc_buf_size = 0;
    void *sc_ptr = NULL;
    DXVA_Slice_H264_Short slices[VIRGL_VIDEO_WIN32_MAX_SLICES];
    unsigned i, slice_count = 0;
    UINT bs_offset = 0;
    D3D11_VIDEO_DECODER_BUFFER_DESC descs[4];
    UINT ret_desc = 0;

    /* --- PictureParameters --- */
    hr = ID3D11VideoContext_GetDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            &pp_buf_size, &pp_ptr);
    if (FAILED(hr) || !pp_ptr || pp_buf_size < sizeof(*pp)) {
        virgl_error("virgl_video_win32: GetDecoderBuffer(PP) "
                    "failed (hr=0x%lx, size=%u)\n",
                    (unsigned long)hr, pp_buf_size);
        return -1;
    }
    memcpy(pp_ptr, pp, sizeof(*pp));
    hr = ID3D11VideoContext_ReleaseDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: ReleaseDecoderBuffer(PP) failed: "
                    "0x%lx\n", (unsigned long)hr);
        return -1;
    }

    /* --- InverseQuantizationMatrix --- */
    hr = ID3D11VideoContext_GetDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
            &iq_buf_size, &iq_ptr);
    if (FAILED(hr) || !iq_ptr || iq_buf_size < sizeof(*qm)) {
        virgl_error("virgl_video_win32: GetDecoderBuffer(IQ) "
                    "failed (hr=0x%lx, size=%u)\n",
                    (unsigned long)hr, iq_buf_size);
        return -1;
    }
    memcpy(iq_ptr, qm, sizeof(*qm));
    hr = ID3D11VideoContext_ReleaseDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: ReleaseDecoderBuffer(IQ) failed: "
                    "0x%lx\n", (unsigned long)hr);
        return -1;
    }

    /* --- Bitstream --- */
    hr = ID3D11VideoContext_GetDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
            &bs_buf_size, &bs_ptr);
    if (FAILED(hr) || !bs_ptr) {
        virgl_error("virgl_video_win32: GetDecoderBuffer(BS) "
                    "failed (hr=0x%lx, size=%u)\n",
                    (unsigned long)hr, bs_buf_size);
        return -1;
    }

    for (i = 0; i < num_buffers && slice_count < VIRGL_VIDEO_WIN32_MAX_SLICES; i++) {
        unsigned sz = sizes[i];
        if (!buffers[i] || !sz)
            continue;
        if (bs_offset + sz > bs_buf_size) {
            /* The DXVA driver owns this buffer and sized it for the frame;
             * spill is fatal. In production we would chunk via
             * wBadSliceChopping, but Mesa rarely hits this in H.264. */
            virgl_error("virgl_video_win32: bitstream buffer overflow "
                        "(%u + %u > %u)\n", bs_offset, sz, bs_buf_size);
            /* Release then bail. */
            ID3D11VideoContext_ReleaseDecoderBuffer(
                g_vid.video_context, codec->decoder,
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
            return -1;
        }
        memcpy((uint8_t *)bs_ptr + bs_offset, buffers[i], sz);

        slices[slice_count].BSNALunitDataLocation = bs_offset;
        slices[slice_count].SliceBytesInBuffer = sz;
        slices[slice_count].wBadSliceChopping = DXVA_SLICE_CHOPPING_NONE;

        bs_offset += sz;
        slice_count++;
    }

    if (slice_count == 0) {
        virgl_warn("virgl_video_win32: no slice data submitted\n");
        ID3D11VideoContext_ReleaseDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
        return -1;
    }

    /* DXVA spec requires the bitstream buffer size to be 128-byte aligned on
     * some drivers. The rest of the buffer after bs_offset is left as-is;
     * DataSize in the buffer desc bounds the valid region. */
    hr = ID3D11VideoContext_ReleaseDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: ReleaseDecoderBuffer(BS) failed: "
                    "0x%lx\n", (unsigned long)hr);
        return -1;
    }

    /* --- SliceControl --- */
    hr = ID3D11VideoContext_GetDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            &sc_buf_size, &sc_ptr);
    if (FAILED(hr) || !sc_ptr ||
        sc_buf_size < slice_count * sizeof(DXVA_Slice_H264_Short)) {
        virgl_error("virgl_video_win32: GetDecoderBuffer(SC) "
                    "failed (hr=0x%lx, need=%u, got=%u)\n",
                    (unsigned long)hr,
                    (unsigned)(slice_count * sizeof(DXVA_Slice_H264_Short)),
                    sc_buf_size);
        return -1;
    }
    memcpy(sc_ptr, slices, slice_count * sizeof(DXVA_Slice_H264_Short));
    hr = ID3D11VideoContext_ReleaseDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: ReleaseDecoderBuffer(SC) failed: "
                    "0x%lx\n", (unsigned long)hr);
        return -1;
    }

    /* --- SubmitDecoderBuffers --- */
    memset(descs, 0, sizeof(descs));

    descs[ret_desc].BufferType =
        D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
    descs[ret_desc].DataSize = (UINT)sizeof(DXVA_PicParams_H264);
    ret_desc++;

    descs[ret_desc].BufferType =
        D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
    descs[ret_desc].DataSize = (UINT)sizeof(DXVA_Qmatrix_H264);
    ret_desc++;

    descs[ret_desc].BufferType =
        D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
    descs[ret_desc].DataSize =
        (UINT)(slice_count * sizeof(DXVA_Slice_H264_Short));
    ret_desc++;

    descs[ret_desc].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
    descs[ret_desc].DataSize = bs_offset;
    ret_desc++;

    hr = ID3D11VideoContext_SubmitDecoderBuffers(
            g_vid.video_context, codec->decoder, ret_desc, descs);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: SubmitDecoderBuffers failed: 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }

    return 0;
}

static int h264_decode_bitstream(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *target,
                                 const struct virgl_h264_picture_desc *desc,
                                 unsigned num_buffers,
                                 const void * const *buffers,
                                 const unsigned *sizes)
{
    DXVA_PicParams_H264 pp;
    DXVA_Qmatrix_H264 qm;

    fill_dxva_picparams_h264(codec, target, desc, &pp);
    fill_dxva_qmatrix_h264(desc, &qm);

    return submit_h264_decode(codec, &pp, &qm, num_buffers, buffers, sizes);
}

int virgl_video_decode_bitstream(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *target,
                                 const union virgl_picture_desc *desc,
                                 unsigned num_buffers,
                                 const void * const *buffers,
                                 const unsigned *sizes)
{
    if (!g_vid.initialized || !codec || !target || !desc ||
        !num_buffers || !buffers || !sizes)
        return -1;

    if ((enum pipe_video_profile)desc->base.profile != codec->profile) {
        virgl_error("virgl_video_win32: profile mismatch pic=%d codec=%d\n",
                    (int)desc->base.profile, (int)codec->profile);
        return -1;
    }

    switch (codec->profile) {
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
        return h264_decode_bitstream(codec, target, &desc->h264,
                                     num_buffers, buffers, sizes);
    /* TODO future codec: HEVC, VP9, AV1, MPEG2 go here. */
    default:
        virgl_error("virgl_video_win32: profile %d not implemented\n",
                    (int)codec->profile);
        return -1;
    }
}

int virgl_video_encode_bitstream(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *source,
                                 const union virgl_picture_desc *desc)
{
    /* Encode is out of scope for the Windows backend. Return ENOSYS so Mesa
     * marks the path as unsupported and falls back to CPU encoders. */
    (void)codec;
    (void)source;
    (void)desc;
    return -ENOSYS;
}

/*
 * ---------------------------------------------------------------------------
 * end_frame.
 *
 * Let the decoder finish, then copy the decode texture into our staging
 * texture and Map() it so the decode_completed callback can run
 * virgl_video_buffer_cpu_readback() and pull NV12 bytes out.
 * ---------------------------------------------------------------------------
 */

static void fill_dma_buf_metadata(struct virgl_video_buffer *buf)
{
    struct virgl_video_dma_buf *d = &buf->dmabuf;

    memset(d, 0, sizeof(*d));
    d->buf = buf;
    d->drm_format = drm_fourcc_nv12();
    d->width = buf->width;
    d->height = buf->height;
    d->flags = VIRGL_VIDEO_DMABUF_READ_ONLY;
    /* NV12 has two planes: Y (full res) and UV interleaved (half res). */
    d->num_planes = 2;
    for (unsigned i = 0; i < 2; i++) {
        d->planes[i].drm_format = d->drm_format;
        d->planes[i].fd = -1;            /* No fd on Windows. */
        d->planes[i].size = 0;
        d->planes[i].modifier = 0;
        d->planes[i].offset = 0;
        d->planes[i].pitch = 0;          /* Populated after Map(). */
    }
}

int virgl_video_end_frame(struct virgl_video_codec *codec,
                          struct virgl_video_buffer *target)
{
    HRESULT hr;

    if (!g_vid.initialized || !codec || !target || !codec->decoder)
        return -1;

    hr = ID3D11VideoContext_DecoderEndFrame(g_vid.video_context,
                                            codec->decoder);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: DecoderEndFrame failed: 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }

    unmap_staging_if_needed(target);

    ID3D11DeviceContext_CopyResource(g_vid.context,
                                     (ID3D11Resource *)target->staging_tex,
                                     (ID3D11Resource *)target->decode_tex);

    hr = ID3D11DeviceContext_Map(g_vid.context,
                                 (ID3D11Resource *)target->staging_tex,
                                 0, D3D11_MAP_READ, 0, &target->mapped_y);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: staging Map failed: 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }
    /*
     * Single-subresource NV12 staging: Y plane lives at pData, UV plane
     * at pData + height*RowPitch (the documented DXGI NV12 layout).
     * `mapped_uv` shares the underlying mapping; we only use its pData
     * and RowPitch fields as a convenience carrier for the UV pointer.
     */
    target->mapped_uv = target->mapped_y;
    target->mapped_uv.pData =
        (uint8_t *)target->mapped_y.pData +
        (size_t)target->mapped_y.RowPitch * target->height;
    target->staging_mapped = true;

    /* One-shot diagnostic: show what's actually landing in the staging
     * textures. If Y and UV are both zeroed, CopySubresourceRegion is
     * silently failing (likely a format-compatibility rejection) and the
     * visible output will be YUV (Y=0, UV=0) — a solid green screen. */
    {
        static bool logged = false;
        if (!logged) {
            const uint8_t *yp = target->mapped_y.pData;
            const uint8_t *uvp = target->mapped_uv.pData;
            virgl_warn("vid-diag end_frame: Y pitch=%u first=%02x %02x %02x %02x %02x %02x %02x %02x; "
                       "UV pitch=%u first=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                       target->mapped_y.RowPitch,
                       yp[0], yp[1], yp[2], yp[3], yp[4], yp[5], yp[6], yp[7],
                       target->mapped_uv.RowPitch,
                       uvp[0], uvp[1], uvp[2], uvp[3], uvp[4], uvp[5], uvp[6], uvp[7]);
            logged = true;
        }
    }

    /* Build a dma-buf-shaped descriptor with fd=-1. vrend_video.c uses
     * virgl_video_buffer_cpu_readback() instead of the fd when it sees the
     * sentinel. */
    fill_dma_buf_metadata(target);
    target->dmabuf.planes[0].pitch = target->mapped_y.RowPitch;
    target->dmabuf.planes[1].pitch = target->mapped_uv.RowPitch;

    if (g_vid.callbacks && g_vid.callbacks->decode_completed)
        g_vid.callbacks->decode_completed(codec, &target->dmabuf);

    /* vrend_video.c's callback has now copied the bytes out; we can safely
     * unmap here so subsequent decodes can reuse the staging texture. */
    unmap_staging_if_needed(target);

    codec->target = NULL;
    return 0;
}

/*
 * ---------------------------------------------------------------------------
 * virgl_video_buffer_cpu_readback.
 *
 * Exposed through virgl_video.h so vrend_video.c can retrieve NV12 plane
 * pointers after a decode_completed callback fires. The pointers stay valid
 * until we hit end_frame/begin_frame again, which matches the span of the
 * callback itself.
 * ---------------------------------------------------------------------------
 */

unsigned virgl_video_buffer_cpu_readback(struct virgl_video_buffer *buffer,
                                         void *planes_out[4],
                                         uint32_t pitches_out[4])
{
    if (!buffer || !planes_out || !pitches_out)
        return 0;
    if (!buffer->staging_mapped ||
        !buffer->mapped_y.pData || !buffer->mapped_uv.pData)
        return 0;

    /* Y and UV come from independent Map() calls — their pitches and base
     * pointers are not required to be related. */
    planes_out[0]  = buffer->mapped_y.pData;
    pitches_out[0] = buffer->mapped_y.RowPitch;
    planes_out[1]  = buffer->mapped_uv.pData;
    pitches_out[1] = buffer->mapped_uv.RowPitch;
    planes_out[2] = NULL; pitches_out[2] = 0;
    planes_out[3] = NULL; pitches_out[3] = 0;

    return 2;
}
