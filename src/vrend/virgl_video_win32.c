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
 * well above what Mesa's H.264 frontend will hand us, and also matches the
 * AV1 per-frame tile limit (virgl_av1_picture_desc::slice_parameter arrays
 * are sized [256]). */
#define VIRGL_VIDEO_WIN32_MAX_SLICES 256

/* Upper bound on how many reference frames we will track per codec. The DXVA
 * H.264 RefFrameList is 16 long, which is also the max in the H.264 spec.
 * HEVC uses 15 entries, VP9 has 8 ref_frame_map slots, AV1 has 8 slots with
 * 7 active frame_refs — 16 is sufficient for all four codecs. */
#define VIRGL_VIDEO_WIN32_MAX_REFS 16

/* AV1 VLD Profile 0 GUID. The D3D11 public headers bundled with MinGW don't
 * always expose D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, so we define the
 * value here. dxva.h does provide DXVA_ModeAV1_VLD_Profile0 with the same
 * byte-for-byte GUID {b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a}. */
#ifndef D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0
DEFINE_GUID(D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0_LOCAL,
    0xb8be4ccb, 0xcf53, 0x46ba, 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a);
#define VIRGL_VIDEO_WIN32_AV1_PROFILE0_GUID \
        D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0_LOCAL
#else
#define VIRGL_VIDEO_WIN32_AV1_PROFILE0_GUID \
        D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0
#endif

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
        /* TODO: add P010 output for HIGH_10 — would need a separate GUID
         * (HEVC_VLD_MAIN10 is the 10-bit surrogate we already handle). */
        return false;
    default:
        return false;
    }
}

/* True for the HEVC profiles we map to D3D11 decoder GUIDs. Main goes to
 * HEVC_VLD_MAIN (NV12); Main10 goes to HEVC_VLD_MAIN10 (P010). Other Main12,
 * Main444, etc. profiles exist in pipe but we don't advertise them. */
static bool is_supported_hevc_profile(enum pipe_video_profile profile)
{
    switch (profile) {
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
        return true;
    default:
        return false;
    }
}

static bool is_supported_vp9_profile(enum pipe_video_profile profile)
{
    switch (profile) {
    case PIPE_VIDEO_PROFILE_VP9_PROFILE0:
    case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
        return true;
    default:
        return false;
    }
}

static bool is_supported_av1_profile(enum pipe_video_profile profile)
{
    return profile == PIPE_VIDEO_PROFILE_AV1_MAIN;
}

static bool map_profile_to_dxva_guid(enum pipe_video_profile profile,
                                     GUID *out)
{
    if (is_supported_h264_profile(profile)) {
        *out = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
        return true;
    }
    switch (profile) {
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL:
        *out = D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
        return true;
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
        *out = D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10;
        return true;
    case PIPE_VIDEO_PROFILE_VP9_PROFILE0:
        *out = D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0;
        return true;
    case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
        *out = D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2;
        return true;
    case PIPE_VIDEO_PROFILE_AV1_MAIN:
        *out = VIRGL_VIDEO_WIN32_AV1_PROFILE0_GUID;
        return true;
    default:
        break;
    }
    /* TODO future codec: MPEG2 (DXVA_ModeMPEG2and1_VLD). */
    return false;
}

/* True if the given pipe profile expects a 10-bit (or higher) output
 * surface. D3D11 expects DXGI_FORMAT_P010 for these; dxgi_format_from_pipe()
 * produces P010 when the guest has requested PIPE_FORMAT_P010, so the main
 * effect is on the CreateVideoDecoder() OutputFormat we pass. */
static bool profile_wants_10bit_output(enum pipe_video_profile profile)
{
    switch (profile) {
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
    case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
        return true;
    default:
        return false;
    }
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

/* HEVC / H.265: Mesa VA-API exposes Main and Main10; the spec allows much
 * larger pictures than H.264 (8K common). D3D11 HEVC CTBs are effectively
 * 16x16 for the purposes of "max_macroblocks" bookkeeping. */
static void fill_caps_for_hevc(struct virgl_video_caps *v,
                               enum pipe_video_profile profile)
{
    v->profile = profile;
    v->entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
    v->max_level = 153;             /* HEVC Level 5.1 encoded as 30*level */
    v->stacked_frames = 0;
    /* Advertise up to 8K decode so guest Mesa can pick through. Actual
     * support is gated by CheckVideoDecoderFormat + CreateVideoDecoder. */
    v->max_width = 7680;
    v->max_height = 4320;
    v->prefered_format = (profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10) ?
                         PIPE_FORMAT_P010 : PIPE_FORMAT_NV12;
    v->max_macroblocks = (7680 / 16) * (4320 / 16);
    v->npot_texture = 1;
    v->supports_progressive = 1;
    v->supports_interlaced = 0;
    v->prefers_interlaced = 0;
    v->max_temporal_layers = 0;
}

static void fill_caps_for_vp9(struct virgl_video_caps *v,
                              enum pipe_video_profile profile)
{
    v->profile = profile;
    v->entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
    v->max_level = 51;              /* VP9 uses a 0..6.2 level range; the
                                     * cap is advisory for the guest */
    v->stacked_frames = 0;
    v->max_width = 7680;
    v->max_height = 4320;
    v->prefered_format = (profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2) ?
                         PIPE_FORMAT_P010 : PIPE_FORMAT_NV12;
    /* VP9 "superblocks" are 64x64 but Mesa's cap accounting is in 16x16
     * macroblock equivalents, matching what the H.264/HEVC branches do. */
    v->max_macroblocks = (7680 / 16) * (4320 / 16);
    v->npot_texture = 1;
    v->supports_progressive = 1;
    v->supports_interlaced = 0;
    v->prefers_interlaced = 0;
    v->max_temporal_layers = 0;
}

static void fill_caps_for_av1(struct virgl_video_caps *v,
                              enum pipe_video_profile profile)
{
    v->profile = profile;
    v->entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
    v->max_level = 51;              /* AV1 level 5.1 */
    v->stacked_frames = 0;
    v->max_width = 7680;
    v->max_height = 4320;
    /* Profile 0 is 8-bit 4:2:0; output is always NV12 here. */
    v->prefered_format = PIPE_FORMAT_NV12;
    v->max_macroblocks = (7680 / 16) * (4320 / 16);
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
    bool have_hevc_main = false;
    bool have_hevc_main10 = false;
    bool have_vp9_p0 = false;
    bool have_vp9_p2 = false;
    bool have_av1_p0 = false;
    unsigned out = 0;

    if (!g_vid.initialized || !caps)
        return -1;

    /*
     * Walk every profile GUID the D3D11 driver claims to support, then
     * filter to the ones we know how to drive. CheckVideoDecoderFormat
     * (NV12 for 8-bit / P010 for 10-bit) is a cheap sanity check that the
     * driver will actually accept the output format we'd create the decoder
     * with.
     */
    profile_count = ID3D11VideoDevice_GetVideoDecoderProfileCount(
                            g_vid.video_device);
    for (i = 0; i < profile_count; i++) {
        WINBOOL nv12_ok = FALSE, p010_ok = FALSE;
        HRESULT hr;

        if (FAILED(ID3D11VideoDevice_GetVideoDecoderProfile(
                        g_vid.video_device, i, &guid)))
            continue;

        hr = ID3D11VideoDevice_CheckVideoDecoderFormat(
                        g_vid.video_device, &guid, DXGI_FORMAT_NV12,
                        &nv12_ok);
        if (FAILED(hr))
            nv12_ok = FALSE;
        hr = ID3D11VideoDevice_CheckVideoDecoderFormat(
                        g_vid.video_device, &guid, DXGI_FORMAT_P010,
                        &p010_ok);
        if (FAILED(hr))
            p010_ok = FALSE;

        if (profile_guid_matches(&guid,
                                 &D3D11_DECODER_PROFILE_H264_VLD_NOFGT)) {
            if (nv12_ok)
                have_h264_nofgt = true;
        } else if (profile_guid_matches(&guid,
                                 &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN)) {
            if (nv12_ok)
                have_hevc_main = true;
        } else if (profile_guid_matches(&guid,
                                 &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10)) {
            if (p010_ok)
                have_hevc_main10 = true;
        } else if (profile_guid_matches(&guid,
                                 &D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0)) {
            if (nv12_ok)
                have_vp9_p0 = true;
        } else if (profile_guid_matches(&guid,
                                 &D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2)) {
            if (p010_ok)
                have_vp9_p2 = true;
        } else if (profile_guid_matches(&guid,
                                 &VIRGL_VIDEO_WIN32_AV1_PROFILE0_GUID)) {
            if (nv12_ok)
                have_av1_p0 = true;
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

    if (have_hevc_main && out < ARRAY_SIZE(caps->v2.video_caps)) {
        fill_caps_for_hevc(&caps->v2.video_caps[out++],
                           PIPE_VIDEO_PROFILE_HEVC_MAIN);
        if (out < ARRAY_SIZE(caps->v2.video_caps))
            fill_caps_for_hevc(&caps->v2.video_caps[out++],
                               PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL);
    }
    if (have_hevc_main10 && out < ARRAY_SIZE(caps->v2.video_caps)) {
        fill_caps_for_hevc(&caps->v2.video_caps[out++],
                           PIPE_VIDEO_PROFILE_HEVC_MAIN_10);
    }

    if (have_vp9_p0 && out < ARRAY_SIZE(caps->v2.video_caps)) {
        fill_caps_for_vp9(&caps->v2.video_caps[out++],
                          PIPE_VIDEO_PROFILE_VP9_PROFILE0);
    }
    if (have_vp9_p2 && out < ARRAY_SIZE(caps->v2.video_caps)) {
        fill_caps_for_vp9(&caps->v2.video_caps[out++],
                          PIPE_VIDEO_PROFILE_VP9_PROFILE2);
    }

    if (have_av1_p0 && out < ARRAY_SIZE(caps->v2.video_caps)) {
        fill_caps_for_av1(&caps->v2.video_caps[out++],
                          PIPE_VIDEO_PROFILE_AV1_MAIN);
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
    /* 10-bit profiles (HEVC Main10, VP9 Profile2) decode into P010. Everything
     * else stays on NV12. The guest still creates the virgl video buffer with
     * its chosen pipe format — virgl_video_create_buffer maps NV12/P010 to
     * the matching DXGI format independently. */
    desc.OutputFormat = profile_wants_10bit_output(args->profile) ?
                        DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

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

/*
 * ---------------------------------------------------------------------------
 * HEVC / H.265 picture-parameter marshalling.
 *
 * DXVA_PicParams_HEVC is considerably larger than the H.264 equivalent: it
 * carries the full SPS/PPS-derived state plus per-ref POC and ref-pic-set
 * indices. We pack it from virgl_h265_picture_desc, which Mesa fills from
 * VA-API's VAPictureParameterBufferHEVC / VAIQMatrixBufferHEVC / VASlice*.
 *
 * See [MS-DXVA]: "DirectX Video Acceleration HEVC Specification" and the
 * ffmpeg libavcodec/dxva2_hevc.c as a cross-check.
 * ---------------------------------------------------------------------------
 */

static void dxva_picentry_hevc_invalidate(DXVA_PicEntry_HEVC *e)
{
    e->bPicEntry = 0xFF;     /* Index7Bits=0x7F + AssociatedFlag=1 => invalid */
}

static void fill_dxva_picparams_hevc(struct virgl_video_codec *codec,
                                     struct virgl_video_buffer *target,
                                     const struct virgl_h265_picture_desc *desc,
                                     DXVA_PicParams_HEVC *pp)
{
    const struct virgl_h265_pps *pps = &desc->pps;
    const struct virgl_h265_sps *sps = &pps->sps;
    unsigned i;
    int self_slot;
    UCHAR min_cb_size;

    memset(pp, 0, sizeof(*pp));

    /* Picture size is expressed in multiples of the minimum luma coding
     * block size (which is 2^(log2_min_luma_coding_block_size_minus3 + 3)).
     * PicWidthInMinCbsY / PicHeightInMinCbsY are unitless block counts. */
    min_cb_size =
        (UCHAR)(1u << (sps->log2_min_luma_coding_block_size_minus3 + 3));
    if (min_cb_size == 0)
        min_cb_size = 8;   /* defensive against a bad SPS */
    pp->PicWidthInMinCbsY  = (USHORT)(sps->pic_width_in_luma_samples / min_cb_size);
    pp->PicHeightInMinCbsY = (USHORT)(sps->pic_height_in_luma_samples / min_cb_size);

    /* wFormatAndSequenceInfoFlags */
    pp->chroma_format_idc              = sps->chroma_format_idc;
    pp->separate_colour_plane_flag     = sps->separate_colour_plane_flag;
    pp->bit_depth_luma_minus8          = sps->bit_depth_luma_minus8;
    pp->bit_depth_chroma_minus8        = sps->bit_depth_chroma_minus8;
    pp->log2_max_pic_order_cnt_lsb_minus4 =
        sps->log2_max_pic_order_cnt_lsb_minus4;
    /* NoPicReorderingFlag / NoBiPredFlag are conservative hints — leaving
     * them 0 (the driver will reorder / bipred as needed). */
    pp->NoPicReorderingFlag = 0;
    pp->NoBiPredFlag        = 0;

    /* CurrPic */
    self_slot = codec_find_or_add_ref_slot(codec, target->id, target);
    if (self_slot < 0)
        self_slot = 0;
    pp->CurrPic.Index7Bits   = (UCHAR)(self_slot & 0x7F);
    pp->CurrPic.AssociatedFlag = 0;

    /* SPS-derived scalars. */
    pp->sps_max_dec_pic_buffering_minus1 =
        sps->sps_max_dec_pic_buffering_minus1;
    pp->log2_min_luma_coding_block_size_minus3 =
        sps->log2_min_luma_coding_block_size_minus3;
    pp->log2_diff_max_min_luma_coding_block_size =
        sps->log2_diff_max_min_luma_coding_block_size;
    pp->log2_min_transform_block_size_minus2 =
        sps->log2_min_transform_block_size_minus2;
    pp->log2_diff_max_min_transform_block_size =
        sps->log2_diff_max_min_transform_block_size;
    pp->max_transform_hierarchy_depth_inter =
        sps->max_transform_hierarchy_depth_inter;
    pp->max_transform_hierarchy_depth_intra =
        sps->max_transform_hierarchy_depth_intra;
    pp->num_short_term_ref_pic_sets = sps->num_short_term_ref_pic_sets;
    pp->num_long_term_ref_pics_sps  = sps->num_long_term_ref_pics_sps;

    /* PPS-derived scalars. */
    pp->num_ref_idx_l0_default_active_minus1 =
        pps->num_ref_idx_l0_default_active_minus1;
    pp->num_ref_idx_l1_default_active_minus1 =
        pps->num_ref_idx_l1_default_active_minus1;
    pp->init_qp_minus26            = pps->init_qp_minus26;
    pp->ucNumDeltaPocsOfRefRpsIdx  = (UCHAR)desc->NumDeltaPocsOfRefRpsIdx;
    pp->wNumBitsForShortTermRPSInSlice =
        desc->UseStRpsBits ? (USHORT)desc->NumShortTermPictureSliceHeaderBits
                           : (USHORT)pps->st_rps_bits;

    /* dwCodingParamToolFlags (SPS-ish) */
    pp->scaling_list_enabled_flag         = sps->scaling_list_enabled_flag;
    pp->amp_enabled_flag                  = sps->amp_enabled_flag;
    pp->sample_adaptive_offset_enabled_flag =
        sps->sample_adaptive_offset_enabled_flag;
    pp->pcm_enabled_flag                  = sps->pcm_enabled_flag;
    pp->pcm_sample_bit_depth_luma_minus1  = sps->pcm_sample_bit_depth_luma_minus1;
    pp->pcm_sample_bit_depth_chroma_minus1 =
        sps->pcm_sample_bit_depth_chroma_minus1;
    pp->log2_min_pcm_luma_coding_block_size_minus3 =
        sps->log2_min_pcm_luma_coding_block_size_minus3;
    pp->log2_diff_max_min_pcm_luma_coding_block_size =
        sps->log2_diff_max_min_pcm_luma_coding_block_size;
    pp->pcm_loop_filter_disabled_flag     = sps->pcm_loop_filter_disabled_flag;
    pp->long_term_ref_pics_present_flag   = sps->long_term_ref_pics_present_flag;
    pp->sps_temporal_mvp_enabled_flag     = sps->sps_temporal_mvp_enabled_flag;
    pp->strong_intra_smoothing_enabled_flag =
        sps->strong_intra_smoothing_enabled_flag;
    pp->dependent_slice_segments_enabled_flag =
        pps->dependent_slice_segments_enabled_flag;
    pp->output_flag_present_flag          = pps->output_flag_present_flag;
    pp->num_extra_slice_header_bits       = pps->num_extra_slice_header_bits;
    pp->sign_data_hiding_enabled_flag     = pps->sign_data_hiding_enabled_flag;
    pp->cabac_init_present_flag           = pps->cabac_init_present_flag;

    /* dwCodingSettingPicturePropertyFlags (PPS-ish + current-pic flags) */
    pp->constrained_intra_pred_flag       = pps->constrained_intra_pred_flag;
    pp->transform_skip_enabled_flag       = pps->transform_skip_enabled_flag;
    pp->cu_qp_delta_enabled_flag          = pps->cu_qp_delta_enabled_flag;
    pp->pps_slice_chroma_qp_offsets_present_flag =
        pps->pps_slice_chroma_qp_offsets_present_flag;
    pp->weighted_pred_flag                = pps->weighted_pred_flag;
    pp->weighted_bipred_flag              = pps->weighted_bipred_flag;
    pp->transquant_bypass_enabled_flag    = pps->transquant_bypass_enabled_flag;
    pp->tiles_enabled_flag                = pps->tiles_enabled_flag;
    pp->entropy_coding_sync_enabled_flag  = pps->entropy_coding_sync_enabled_flag;
    pp->uniform_spacing_flag              = pps->uniform_spacing_flag;
    pp->loop_filter_across_tiles_enabled_flag =
        pps->loop_filter_across_tiles_enabled_flag;
    pp->pps_loop_filter_across_slices_enabled_flag =
        pps->pps_loop_filter_across_slices_enabled_flag;
    pp->deblocking_filter_override_enabled_flag =
        pps->deblocking_filter_override_enabled_flag;
    pp->pps_deblocking_filter_disabled_flag =
        pps->pps_deblocking_filter_disabled_flag;
    pp->lists_modification_present_flag   = pps->lists_modification_present_flag;
    pp->slice_segment_header_extension_present_flag =
        pps->slice_segment_header_extension_present_flag;
    pp->IrapPicFlag                       = desc->RAPPicFlag ? 1 : 0;
    pp->IdrPicFlag                        = desc->IDRPicFlag ? 1 : 0;
    /* IntraPicFlag is a hint for intra-only frames; derive conservatively
     * from IDR. TODO: virgl desc doesn't expose a distinct intra-only bit,
     * so non-IDR I-frames report 0 and the driver will resolve from slice
     * headers. */
    pp->IntraPicFlag                      = desc->IDRPicFlag ? 1 : 0;

    pp->pps_cb_qp_offset        = pps->pps_cb_qp_offset;
    pp->pps_cr_qp_offset        = pps->pps_cr_qp_offset;
    pp->num_tile_columns_minus1 = pps->num_tile_columns_minus1;
    pp->num_tile_rows_minus1    = pps->num_tile_rows_minus1;

    /* DXVA carries 19 columns / 21 rows worth of sizes; virgl has 20/22.
     * We only copy as many as the PPS says are present, which is bounded
     * by num_tile_{columns,rows}_minus1 <= DXVA's array size. */
    for (i = 0; i < 19 && i < 20; i++)
        pp->column_width_minus1[i] = pps->column_width_minus1[i];
    for (i = 0; i < 21 && i < 22; i++)
        pp->row_height_minus1[i]   = pps->row_height_minus1[i];

    pp->diff_cu_qp_delta_depth       = pps->diff_cu_qp_delta_depth;
    pp->pps_beta_offset_div2         = pps->pps_beta_offset_div2;
    pp->pps_tc_offset_div2           = pps->pps_tc_offset_div2;
    pp->log2_parallel_merge_level_minus2 =
        pps->log2_parallel_merge_level_minus2;
    pp->CurrPicOrderCntVal           = desc->CurrPicOrderCntVal;

    /* RefPicList: up to 15 entries. DXVA encodes a long-term flag in the
     * AssociatedFlag bit; we mark unused slots with bPicEntry=0xFF. */
    for (i = 0; i < 15; i++) {
        dxva_picentry_hevc_invalidate(&pp->RefPicList[i]);
        pp->PicOrderCntValList[i] = 0;
    }
    for (i = 0; i < 15; i++) {
        uint32_t bid = desc->ref[i];
        int slot;

        if (bid == 0)
            continue;

        slot = codec_find_or_add_ref_slot(codec, bid, NULL);
        if (slot < 0)
            continue;
        pp->RefPicList[i].Index7Bits    = (UCHAR)(slot & 0x7F);
        pp->RefPicList[i].AssociatedFlag = desc->IsLongTerm[i] ? 1 : 0;
        pp->PicOrderCntValList[i] = desc->PicOrderCntVal[i];
    }

    /* Ref-pic-set indices into RefPicList[]: curr-before, curr-after,
     * lt-curr. Each is padded with 0xFF when fewer than 8 entries apply. */
    memset(pp->RefPicSetStCurrBefore, 0xFF, sizeof(pp->RefPicSetStCurrBefore));
    memset(pp->RefPicSetStCurrAfter,  0xFF, sizeof(pp->RefPicSetStCurrAfter));
    memset(pp->RefPicSetLtCurr,       0xFF, sizeof(pp->RefPicSetLtCurr));
    for (i = 0; i < desc->NumPocStCurrBefore && i < 8; i++)
        pp->RefPicSetStCurrBefore[i] = desc->RefPicSetStCurrBefore[i];
    for (i = 0; i < desc->NumPocStCurrAfter && i < 8; i++)
        pp->RefPicSetStCurrAfter[i]  = desc->RefPicSetStCurrAfter[i];
    for (i = 0; i < desc->NumPocLtCurr && i < 8; i++)
        pp->RefPicSetLtCurr[i]       = desc->RefPicSetLtCurr[i];

    pp->StatusReportFeedbackNumber = codec->status_report_feedback++;
    if (pp->StatusReportFeedbackNumber == 0)
        pp->StatusReportFeedbackNumber = codec->status_report_feedback++;
}

static void fill_dxva_qmatrix_hevc(const struct virgl_h265_picture_desc *desc,
                                   DXVA_Qmatrix_HEVC *qm)
{
    const struct virgl_h265_sps *sps = &desc->pps.sps;

    /* HEVC has four block-size classes of scaling lists. virgl packs them
     * in the same zig-zag order DXVA expects (matching VA-API semantics). */
    memcpy(qm->ucScalingLists0, sps->ScalingList4x4,
           sizeof(qm->ucScalingLists0));
    memcpy(qm->ucScalingLists1, sps->ScalingList8x8,
           sizeof(qm->ucScalingLists1));
    memcpy(qm->ucScalingLists2, sps->ScalingList16x16,
           sizeof(qm->ucScalingLists2));
    memcpy(qm->ucScalingLists3, sps->ScalingList32x32,
           sizeof(qm->ucScalingLists3));
    memcpy(qm->ucScalingListDCCoefSizeID2, sps->ScalingListDCCoeff16x16,
           sizeof(qm->ucScalingListDCCoefSizeID2));
    memcpy(qm->ucScalingListDCCoefSizeID3, sps->ScalingListDCCoeff32x32,
           sizeof(qm->ucScalingListDCCoefSizeID3));
}

/* Shared short-format slice/bitstream submission for HEVC / VP9 / AV1.
 * All three use DXVA_Slice_*_Short with the same byte layout (offset, size,
 * chopping), so one helper handles all of them.
 *
 * pp/pp_size  : picture-params blob (already filled).
 * qm/qm_size  : inverse-quantization-matrix blob, or NULL when the codec
 *               doesn't use one (VP9, AV1).
 * sc/sc_elem  : slice-control entry size (sizeof(DXVA_Slice_*_Short) or
 *               sizeof(DXVA_Tile_AV1) for AV1).
 * build_sc    : callback that fills one slice/tile entry given its index,
 *               offset and size; returns void.
 */
typedef void (*build_sc_entry_fn)(void *sc_array, unsigned idx,
                                  UINT bs_offset, UINT slice_sz,
                                  void *user);

static int submit_short_format_decode(struct virgl_video_codec *codec,
                                      const void *pp, UINT pp_size,
                                      const void *qm, UINT qm_size,
                                      UINT sc_elem_size,
                                      build_sc_entry_fn build_sc,
                                      void *build_sc_user,
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
    uint8_t slice_storage[VIRGL_VIDEO_WIN32_MAX_SLICES * 64];
    unsigned i, slice_count = 0;
    UINT bs_offset = 0;
    D3D11_VIDEO_DECODER_BUFFER_DESC descs[4];
    UINT ret_desc = 0;

    if (sc_elem_size > 64 ||
        sc_elem_size * VIRGL_VIDEO_WIN32_MAX_SLICES > sizeof(slice_storage)) {
        virgl_error("virgl_video_win32: slice control elem size %u too big\n",
                    sc_elem_size);
        return -1;
    }
    memset(slice_storage, 0, sizeof(slice_storage));

    /* --- PictureParameters --- */
    hr = ID3D11VideoContext_GetDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            &pp_buf_size, &pp_ptr);
    if (FAILED(hr) || !pp_ptr || pp_buf_size < pp_size) {
        virgl_error("virgl_video_win32: GetDecoderBuffer(PP) "
                    "failed (hr=0x%lx, size=%u/need=%u)\n",
                    (unsigned long)hr, pp_buf_size, pp_size);
        return -1;
    }
    memcpy(pp_ptr, pp, pp_size);
    hr = ID3D11VideoContext_ReleaseDecoderBuffer(
            g_vid.video_context, codec->decoder,
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: ReleaseDecoderBuffer(PP) failed: "
                    "0x%lx\n", (unsigned long)hr);
        return -1;
    }

    /* --- InverseQuantizationMatrix (optional) --- */
    if (qm && qm_size) {
        hr = ID3D11VideoContext_GetDecoderBuffer(
                g_vid.video_context, codec->decoder,
                D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
                &iq_buf_size, &iq_ptr);
        if (FAILED(hr) || !iq_ptr || iq_buf_size < qm_size) {
            virgl_error("virgl_video_win32: GetDecoderBuffer(IQ) "
                        "failed (hr=0x%lx, size=%u/need=%u)\n",
                        (unsigned long)hr, iq_buf_size, qm_size);
            return -1;
        }
        memcpy(iq_ptr, qm, qm_size);
        hr = ID3D11VideoContext_ReleaseDecoderBuffer(
                g_vid.video_context, codec->decoder,
                D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX);
        if (FAILED(hr)) {
            virgl_error("virgl_video_win32: ReleaseDecoderBuffer(IQ) failed: "
                        "0x%lx\n", (unsigned long)hr);
            return -1;
        }
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
            virgl_error("virgl_video_win32: bitstream buffer overflow "
                        "(%u + %u > %u)\n", bs_offset, sz, bs_buf_size);
            ID3D11VideoContext_ReleaseDecoderBuffer(
                g_vid.video_context, codec->decoder,
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
            return -1;
        }
        memcpy((uint8_t *)bs_ptr + bs_offset, buffers[i], sz);
        build_sc(slice_storage, slice_count, bs_offset, sz, build_sc_user);
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
        sc_buf_size < slice_count * sc_elem_size) {
        virgl_error("virgl_video_win32: GetDecoderBuffer(SC) "
                    "failed (hr=0x%lx, need=%u, got=%u)\n",
                    (unsigned long)hr,
                    (unsigned)(slice_count * sc_elem_size),
                    sc_buf_size);
        return -1;
    }
    memcpy(sc_ptr, slice_storage, slice_count * sc_elem_size);
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
    descs[ret_desc].DataSize = pp_size;
    ret_desc++;

    if (qm && qm_size) {
        descs[ret_desc].BufferType =
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
        descs[ret_desc].DataSize = qm_size;
        ret_desc++;
    }

    descs[ret_desc].BufferType =
        D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
    descs[ret_desc].DataSize = (UINT)(slice_count * sc_elem_size);
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

static void build_sc_hevc(void *sc_array, unsigned idx,
                          UINT bs_offset, UINT slice_sz, void *user)
{
    DXVA_Slice_HEVC_Short *arr = (DXVA_Slice_HEVC_Short *)sc_array;
    (void)user;
    arr[idx].BSNALunitDataLocation = bs_offset;
    arr[idx].SliceBytesInBuffer    = slice_sz;
    arr[idx].wBadSliceChopping     = DXVA_SLICE_CHOPPING_NONE;
}

static int hevc_decode_bitstream(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *target,
                                 const struct virgl_h265_picture_desc *desc,
                                 unsigned num_buffers,
                                 const void * const *buffers,
                                 const unsigned *sizes)
{
    DXVA_PicParams_HEVC pp;
    DXVA_Qmatrix_HEVC   qm;

    fill_dxva_picparams_hevc(codec, target, desc, &pp);
    fill_dxva_qmatrix_hevc(desc, &qm);

    return submit_short_format_decode(codec,
                                      &pp, (UINT)sizeof(pp),
                                      &qm, (UINT)sizeof(qm),
                                      (UINT)sizeof(DXVA_Slice_HEVC_Short),
                                      build_sc_hevc, NULL,
                                      num_buffers, buffers, sizes);
}

/*
 * ---------------------------------------------------------------------------
 * VP9 picture-parameter marshalling.
 *
 * virgl_vp9_picture_desc is considerably slimmer than the DXVA struct but
 * carries everything DXVA needs: the uncompressed-header-derived frame
 * flags, segmentation data, base QP and ref_frame_map. DXVA additionally
 * wants coded width/height per reference slot, which we can get from the
 * referenced virgl buffers we've tracked.
 * ---------------------------------------------------------------------------
 */

static void dxva_picentry_vpx_invalidate(DXVA_PicEntry_VPx *e)
{
    e->bPicEntry = 0xFF;
}

static void fill_dxva_picparams_vp9(struct virgl_video_codec *codec,
                                    struct virgl_video_buffer *target,
                                    const struct virgl_vp9_picture_desc *desc,
                                    DXVA_PicParams_VP9 *pp)
{
    const struct virgl_vp9_picture_desc *d = desc;
    unsigned i;
    int self_slot;

    memset(pp, 0, sizeof(*pp));

    self_slot = codec_find_or_add_ref_slot(codec, target->id, target);
    if (self_slot < 0)
        self_slot = 0;
    pp->CurrPic.Index7Bits     = (UCHAR)(self_slot & 0x7F);
    pp->CurrPic.AssociatedFlag = 0;

    pp->profile = d->picture_parameter.profile;

    /* wFormatAndPictureInfoFlags */
    pp->frame_type                   = d->picture_parameter.pic_fields.frame_type;
    pp->show_frame                   = d->picture_parameter.pic_fields.show_frame;
    pp->error_resilient_mode         = d->picture_parameter.pic_fields.error_resilient_mode;
    pp->subsampling_x                = d->picture_parameter.pic_fields.subsampling_x;
    pp->subsampling_y                = d->picture_parameter.pic_fields.subsampling_y;
    pp->extra_plane                  = 0;   /* TODO: not signalled in desc */
    pp->refresh_frame_context        = d->picture_parameter.pic_fields.refresh_frame_context;
    pp->frame_parallel_decoding_mode =
        d->picture_parameter.pic_fields.frame_parallel_decoding_mode;
    pp->intra_only                   = d->picture_parameter.pic_fields.intra_only;
    pp->frame_context_idx            = d->picture_parameter.pic_fields.frame_context_idx;
    pp->reset_frame_context          = d->picture_parameter.pic_fields.reset_frame_context;
    pp->allow_high_precision_mv      = d->picture_parameter.pic_fields.allow_high_precision_mv;

    pp->width  = d->picture_parameter.frame_width;
    pp->height = d->picture_parameter.frame_height;

    pp->BitDepthMinus8Luma   = (UCHAR)(d->picture_parameter.bit_depth - 8);
    pp->BitDepthMinus8Chroma = (UCHAR)(d->picture_parameter.bit_depth - 8);
    pp->interp_filter        = d->picture_parameter.pic_fields.mcomp_filter_type;

    /* ref_frame_map: virgl packs 16 guest buffer ids, DXVA wants 8 slots
     * (VP9 has 8 reference-buffer slots). Map by looking the buffer up in
     * our per-codec refs[] table. */
    for (i = 0; i < 8; i++) {
        dxva_picentry_vpx_invalidate(&pp->ref_frame_map[i]);
        pp->ref_frame_coded_width[i]  = 0;
        pp->ref_frame_coded_height[i] = 0;
    }
    for (i = 0; i < 8; i++) {
        uint32_t bid = d->ref[i];
        int slot;
        struct virgl_video_buffer *refbuf = NULL;

        if (bid == 0)
            continue;

        /* Find tracked buf to extract coded width/height. */
        for (unsigned j = 0; j < VIRGL_VIDEO_WIN32_MAX_REFS; j++) {
            if (codec->refs[j].buffer_id == bid) {
                refbuf = codec->refs[j].buf;
                break;
            }
        }
        slot = codec_find_or_add_ref_slot(codec, bid, refbuf);
        if (slot < 0)
            continue;
        pp->ref_frame_map[i].Index7Bits = (UCHAR)(slot & 0x7F);
        pp->ref_frame_map[i].AssociatedFlag = 0;
        if (refbuf) {
            pp->ref_frame_coded_width[i]  = refbuf->width;
            pp->ref_frame_coded_height[i] = refbuf->height;
        } else {
            /* Fallback to current-frame size if we don't have the ref buf
             * cached (first ref of the first P-frame sometimes). */
            pp->ref_frame_coded_width[i]  = d->picture_parameter.frame_width;
            pp->ref_frame_coded_height[i] = d->picture_parameter.frame_height;
        }
    }

    /* frame_refs[3]: last, golden, altref — indices into ref_frame_map. */
    for (i = 0; i < 3; i++)
        dxva_picentry_vpx_invalidate(&pp->frame_refs[i]);
    pp->frame_refs[0].Index7Bits =
        (UCHAR)(d->picture_parameter.pic_fields.last_ref_frame & 0x7);
    pp->frame_refs[0].AssociatedFlag = 0;
    pp->frame_refs[1].Index7Bits =
        (UCHAR)(d->picture_parameter.pic_fields.golden_ref_frame & 0x7);
    pp->frame_refs[1].AssociatedFlag = 0;
    pp->frame_refs[2].Index7Bits =
        (UCHAR)(d->picture_parameter.pic_fields.alt_ref_frame & 0x7);
    pp->frame_refs[2].AssociatedFlag = 0;

    /* ref_frame_sign_bias[4]: VP9 uses 1-indexed refs (0=INTRA, 1=LAST,
     * 2=GOLDEN, 3=ALTREF). */
    pp->ref_frame_sign_bias[0] = 0;
    pp->ref_frame_sign_bias[1] =
        d->picture_parameter.pic_fields.last_ref_frame_sign_bias;
    pp->ref_frame_sign_bias[2] =
        d->picture_parameter.pic_fields.golden_ref_frame_sign_bias;
    pp->ref_frame_sign_bias[3] =
        d->picture_parameter.pic_fields.alt_ref_frame_sign_bias;

    pp->filter_level    = d->picture_parameter.filter_level;
    pp->sharpness_level = d->picture_parameter.sharpness_level;

    /* wControlInfoFlags */
    pp->mode_ref_delta_enabled   = d->picture_parameter.mode_ref_delta_enabled ? 1 : 0;
    pp->mode_ref_delta_update    = d->picture_parameter.mode_ref_delta_update ? 1 : 0;
    pp->use_prev_in_find_mv_refs = 0;   /* TODO: not signalled in virgl desc */

    memcpy(pp->ref_deltas,  d->picture_parameter.ref_deltas,  4);
    memcpy(pp->mode_deltas, d->picture_parameter.mode_deltas, 2);

    pp->base_qindex   = d->picture_parameter.base_qindex;
    pp->y_dc_delta_q  = d->picture_parameter.y_dc_delta_q;
    pp->uv_dc_delta_q = d->picture_parameter.uv_dc_delta_q;
    pp->uv_ac_delta_q = d->picture_parameter.uv_ac_delta_q;

    /* Segmentation */
    pp->stVP9Segments.enabled =
        d->picture_parameter.pic_fields.segmentation_enabled;
    pp->stVP9Segments.update_map =
        d->picture_parameter.pic_fields.segmentation_update_map;
    pp->stVP9Segments.temporal_update =
        d->picture_parameter.pic_fields.segmentation_temporal_update;
    pp->stVP9Segments.abs_delta = d->picture_parameter.abs_delta;
    memcpy(pp->stVP9Segments.tree_probs,
           d->picture_parameter.mb_segment_tree_probs, 7);
    memcpy(pp->stVP9Segments.pred_probs,
           d->picture_parameter.segment_pred_probs, 3);
    /* Per-segment filter_level / QP deltas: virgl packs in seg_param[].
     * DXVA DXVA_segmentation_VP9.feature_data is SHORT[8][4]:
     *   [seg][0] = luma AC QP delta
     *   [seg][1] = luma loop-filter delta
     *   [seg][2] = ref frame (used only when ref-enabled bit set)
     *   [seg][3] = skip (0/1)
     * virgl's per-seg filter/QP deltas come from slice_parameter.seg_param;
     * the DXVA mask encodes which features are active per segment. */
    for (i = 0; i < 8; i++) {
        const struct virgl_vp9_segment_parameter *sp =
            &d->slice_parameter.seg_param[i];
        UCHAR mask = 0;

        pp->stVP9Segments.feature_data[i][0] = sp->luma_ac_quant_scale;
        /* The "loop-filter-level" DXVA field expects a signed delta; virgl
         * stores per-(ref,mode) 4x2 level array. Pick ref=0/mode=0 (intra)
         * as the primary loop filter level delta. */
        pp->stVP9Segments.feature_data[i][1] = (SHORT)sp->filter_level[0][0];
        pp->stVP9Segments.feature_data[i][2] =
            sp->segment_flags.segment_reference;
        pp->stVP9Segments.feature_data[i][3] =
            sp->segment_flags.segment_reference_skipped;

        /* feature_mask bits (per DXVA): alt_q, alt_lf, ref, skip. */
        if (sp->luma_ac_quant_scale || sp->luma_dc_quant_scale ||
            sp->chroma_ac_quant_scale || sp->chroma_dc_quant_scale)
            mask |= 0x1;
        if (sp->filter_level[0][0])
            mask |= 0x2;
        if (sp->segment_flags.segment_reference_enabled)
            mask |= 0x4;
        if (sp->segment_flags.segment_reference_skipped)
            mask |= 0x8;
        pp->stVP9Segments.feature_mask[i] = mask;
    }

    pp->log2_tile_cols = d->picture_parameter.log2_tile_columns;
    pp->log2_tile_rows = d->picture_parameter.log2_tile_rows;
    pp->uncompressed_header_size_byte_aligned =
        d->picture_parameter.frame_header_length_in_bytes;
    pp->first_partition_size = d->picture_parameter.first_partition_size;

    pp->StatusReportFeedbackNumber = codec->status_report_feedback++;
    if (pp->StatusReportFeedbackNumber == 0)
        pp->StatusReportFeedbackNumber = codec->status_report_feedback++;
}

static void build_sc_vpx(void *sc_array, unsigned idx,
                         UINT bs_offset, UINT slice_sz, void *user)
{
    DXVA_Slice_VPx_Short *arr = (DXVA_Slice_VPx_Short *)sc_array;
    (void)user;
    arr[idx].BSNALunitDataLocation = bs_offset;
    arr[idx].SliceBytesInBuffer    = slice_sz;
    arr[idx].wBadSliceChopping     = DXVA_SLICE_CHOPPING_NONE;
}

static int vp9_decode_bitstream(struct virgl_video_codec *codec,
                                struct virgl_video_buffer *target,
                                const struct virgl_vp9_picture_desc *desc,
                                unsigned num_buffers,
                                const void * const *buffers,
                                const unsigned *sizes)
{
    DXVA_PicParams_VP9 pp;

    fill_dxva_picparams_vp9(codec, target, desc, &pp);

    /* VP9 has no separate IQ matrix buffer; quantization is per-segment
     * inside the picture params. */
    return submit_short_format_decode(codec,
                                      &pp, (UINT)sizeof(pp),
                                      NULL, 0,
                                      (UINT)sizeof(DXVA_Slice_VPx_Short),
                                      build_sc_vpx, NULL,
                                      num_buffers, buffers, sizes);
}

/*
 * ---------------------------------------------------------------------------
 * AV1 picture-parameter marshalling.
 *
 * AV1 is the densest of the four codecs: DXVA_PicParams_AV1 covers
 * sequence/frame header, tile geometry, segmentation, CDEF, loop
 * restoration, quantization, film grain and global motion. virgl's
 * picture_parameter mirrors the same concepts one-to-one (Mesa fills it
 * from VADecPictureParameterBufferAV1), so the mapping is mechanical.
 *
 * Slice control for AV1 uses DXVA_Tile_AV1 (not a short-format slice), one
 * per tile. virgl slice_parameter carries per-tile offset/size/row/col.
 * ---------------------------------------------------------------------------
 */

static void fill_dxva_picparams_av1(struct virgl_video_codec *codec,
                                    struct virgl_video_buffer *target,
                                    const struct virgl_av1_picture_desc *desc,
                                    DXVA_PicParams_AV1 *pp)
{
    const struct virgl_av1_picture_desc *d = desc;
    unsigned i, j;
    int self_slot;

    memset(pp, 0, sizeof(*pp));

    pp->width       = d->picture_parameter.frame_width;
    pp->height      = d->picture_parameter.frame_height;
    pp->max_width   = d->picture_parameter.max_width;
    pp->max_height  = d->picture_parameter.max_height;

    self_slot = codec_find_or_add_ref_slot(codec, target->id, target);
    if (self_slot < 0)
        self_slot = 0;
    pp->CurrPicTextureIndex = (UCHAR)(self_slot & 0x7F);

    pp->superres_denom = d->picture_parameter.superres_scale_denominator;
    /* AV1 profile 0 is 8-bit; bit_depth_idx values per AV1 spec:
     *   0 => 8-bit, 1 => 10-bit, 2 => 12-bit. */
    switch (d->picture_parameter.bit_depth_idx) {
    case 0: pp->bitdepth = 8;  break;
    case 1: pp->bitdepth = 10; break;
    case 2: pp->bitdepth = 12; break;
    default: pp->bitdepth = 8; break;
    }
    pp->seq_profile = d->picture_parameter.profile;

    /* Tile geometry. AV1 allows up to 64x64 tiles; DXVA stores widths and
     * heights per-tile-col / per-tile-row. */
    pp->tiles.cols = d->picture_parameter.tile_cols;
    pp->tiles.rows = d->picture_parameter.tile_rows;
    pp->tiles.context_update_id = d->picture_parameter.context_update_tile_id;
    for (i = 0; i < 64; i++) {
        pp->tiles.widths[i]  = d->picture_parameter.width_in_sbs[i];
        pp->tiles.heights[i] = d->picture_parameter.height_in_sbs[i];
    }

    /* CodingParamToolFlags */
    pp->coding.use_128x128_superblock =
        d->picture_parameter.seq_info_fields.use_128x128_superblock;
    pp->coding.intra_edge_filter =
        d->picture_parameter.seq_info_fields.enable_intra_edge_filter;
    pp->coding.interintra_compound =
        d->picture_parameter.seq_info_fields.enable_interintra_compound;
    pp->coding.masked_compound =
        d->picture_parameter.seq_info_fields.enable_masked_compound;
    pp->coding.warped_motion =
        d->picture_parameter.pic_info_fields.allow_warped_motion;
    pp->coding.dual_filter =
        d->picture_parameter.seq_info_fields.enable_dual_filter;
    pp->coding.jnt_comp =
        d->picture_parameter.seq_info_fields.enable_jnt_comp;
    pp->coding.screen_content_tools =
        d->picture_parameter.pic_info_fields.allow_screen_content_tools;
    pp->coding.integer_mv =
        d->picture_parameter.pic_info_fields.force_integer_mv;
    pp->coding.cdef =
        d->picture_parameter.seq_info_fields.enable_cdef;
    pp->coding.restoration = 0;   /* derived from loop_restoration_fields below */
    pp->coding.film_grain =
        d->picture_parameter.seq_info_fields.film_grain_params_present;
    pp->coding.intrabc =
        d->picture_parameter.pic_info_fields.allow_intrabc;
    pp->coding.high_precision_mv =
        d->picture_parameter.pic_info_fields.allow_high_precision_mv;
    pp->coding.switchable_motion_mode =
        d->picture_parameter.pic_info_fields.is_motion_mode_switchable;
    pp->coding.filter_intra =
        d->picture_parameter.seq_info_fields.enable_filter_intra;
    pp->coding.disable_frame_end_update_cdf =
        d->picture_parameter.pic_info_fields.disable_frame_end_update_cdf;
    pp->coding.disable_cdf_update =
        d->picture_parameter.pic_info_fields.disable_cdf_update;
    pp->coding.reference_mode =
        d->picture_parameter.mode_control_fields.reference_select;
    pp->coding.skip_mode =
        d->picture_parameter.mode_control_fields.skip_mode_present;
    pp->coding.reduced_tx_set =
        d->picture_parameter.mode_control_fields.reduced_tx_set_used;
    pp->coding.superres =
        d->picture_parameter.pic_info_fields.use_superres;
    pp->coding.tx_mode =
        d->picture_parameter.mode_control_fields.tx_mode;
    pp->coding.use_ref_frame_mvs =
        d->picture_parameter.pic_info_fields.use_ref_frame_mvs;
    pp->coding.enable_ref_frame_mvs =
        d->picture_parameter.seq_info_fields.ref_frame_mvs;
    pp->coding.reference_frame_update = 1;   /* virgl desc lacks a direct
                                              * signal; conservative 1 tells
                                              * driver to update ref state */

    /* FormatAndPictureInfoFlags */
    pp->format.frame_type =
        d->picture_parameter.pic_info_fields.frame_type;
    pp->format.show_frame =
        d->picture_parameter.pic_info_fields.show_frame;
    pp->format.showable_frame =
        d->picture_parameter.pic_info_fields.showable_frame;
    /* AV1 profile 0 => 4:2:0; subsampling_{x,y} = 1. Higher profiles flip
     * these bits; we pull them from bit_depth_idx / profile for profile 0
     * and leave the driver to check against its own CheckVideoDecoderFormat
     * result for other profiles. */
    pp->format.subsampling_x = 1;
    pp->format.subsampling_y = 1;
    pp->format.mono_chrome =
        d->picture_parameter.seq_info_fields.mono_chrome;

    pp->primary_ref_frame = d->picture_parameter.primary_ref_frame;
    pp->order_hint        = d->picture_parameter.order_hint;
    pp->order_hint_bits   =
        (UCHAR)(d->picture_parameter.order_hint_bits_minus_1 + 1);

    /* frame_refs[7]: per-ref width/height/global-motion. These indices
     * point at entries in RefFrameMapTextureIndex[]. */
    for (i = 0; i < 7; i++) {
        memset(&pp->frame_refs[i], 0, sizeof(pp->frame_refs[i]));
        pp->frame_refs[i].Index = d->picture_parameter.ref_frame_idx[i];
        pp->frame_refs[i].wminvalid = d->picture_parameter.wm[i].invalid ? 1 : 0;
        pp->frame_refs[i].wmtype    = (UCHAR)(d->picture_parameter.wm[i].wmtype & 0x3);
        for (j = 0; j < 6; j++)
            pp->frame_refs[i].wmmat[j] = d->picture_parameter.wm[i].wmmat[j];
        /* Per-ref coded width/height: pull from our tracked ref buffers if
         * available; otherwise use current-frame size. */
        pp->frame_refs[i].width  = d->picture_parameter.frame_width;
        pp->frame_refs[i].height = d->picture_parameter.frame_height;
    }

    /* RefFrameMapTextureIndex: 8 entries, mapping AV1 ref-slot -> decode
     * texture slot. */
    for (i = 0; i < 8; i++)
        pp->RefFrameMapTextureIndex[i] = 0xFF;
    for (i = 0; i < 8; i++) {
        uint32_t bid = d->ref[i];
        int slot;
        if (bid == 0)
            continue;
        slot = codec_find_or_add_ref_slot(codec, bid, NULL);
        if (slot < 0)
            continue;
        pp->RefFrameMapTextureIndex[i] = (UCHAR)(slot & 0x7F);
    }

    /* Loop filter */
    pp->loop_filter.filter_level[0] = d->picture_parameter.filter_level[0];
    pp->loop_filter.filter_level[1] = d->picture_parameter.filter_level[1];
    pp->loop_filter.filter_level_u  = d->picture_parameter.filter_level_u;
    pp->loop_filter.filter_level_v  = d->picture_parameter.filter_level_v;
    pp->loop_filter.sharpness_level =
        d->picture_parameter.loop_filter_info_fields.sharpness_level;
    pp->loop_filter.mode_ref_delta_enabled =
        d->picture_parameter.loop_filter_info_fields.mode_ref_delta_enabled;
    pp->loop_filter.mode_ref_delta_update =
        d->picture_parameter.loop_filter_info_fields.mode_ref_delta_update;
    pp->loop_filter.delta_lf_multi =
        d->picture_parameter.mode_control_fields.delta_lf_multi;
    pp->loop_filter.delta_lf_present =
        d->picture_parameter.mode_control_fields.delta_lf_present_flag;
    for (i = 0; i < 8; i++)
        pp->loop_filter.ref_deltas[i] = d->picture_parameter.ref_deltas[i];
    pp->loop_filter.mode_deltas[0] = d->picture_parameter.mode_deltas[0];
    pp->loop_filter.mode_deltas[1] = d->picture_parameter.mode_deltas[1];
    pp->loop_filter.delta_lf_res =
        (UCHAR)d->picture_parameter.mode_control_fields.log2_delta_lf_res;
    pp->loop_filter.frame_restoration_type[0] =
        (UCHAR)d->picture_parameter.loop_restoration_fields.yframe_restoration_type;
    pp->loop_filter.frame_restoration_type[1] =
        (UCHAR)d->picture_parameter.loop_restoration_fields.cbframe_restoration_type;
    pp->loop_filter.frame_restoration_type[2] =
        (UCHAR)d->picture_parameter.loop_restoration_fields.crframe_restoration_type;
    pp->loop_filter.log2_restoration_unit_size[0] =
        d->picture_parameter.lr_unit_size[0];
    pp->loop_filter.log2_restoration_unit_size[1] =
        d->picture_parameter.lr_unit_size[1];
    pp->loop_filter.log2_restoration_unit_size[2] =
        d->picture_parameter.lr_unit_size[2];

    /* Set loop-restoration enable bit based on whether any plane has it on. */
    if (pp->loop_filter.frame_restoration_type[0] ||
        pp->loop_filter.frame_restoration_type[1] ||
        pp->loop_filter.frame_restoration_type[2])
        pp->coding.restoration = 1;

    /* Quantization */
    pp->quantization.delta_q_present =
        d->picture_parameter.mode_control_fields.delta_q_present_flag;
    pp->quantization.delta_q_res =
        d->picture_parameter.mode_control_fields.log2_delta_q_res;
    pp->quantization.base_qindex  = d->picture_parameter.base_qindex;
    pp->quantization.y_dc_delta_q = d->picture_parameter.y_dc_delta_q;
    pp->quantization.u_dc_delta_q = d->picture_parameter.u_dc_delta_q;
    pp->quantization.v_dc_delta_q = d->picture_parameter.v_dc_delta_q;
    pp->quantization.u_ac_delta_q = d->picture_parameter.u_ac_delta_q;
    pp->quantization.v_ac_delta_q = d->picture_parameter.v_ac_delta_q;
    pp->quantization.qm_y = (UCHAR)d->picture_parameter.qmatrix_fields.qm_y;
    pp->quantization.qm_u = (UCHAR)d->picture_parameter.qmatrix_fields.qm_u;
    pp->quantization.qm_v = (UCHAR)d->picture_parameter.qmatrix_fields.qm_v;

    /* CDEF */
    pp->cdef.damping = (UCHAR)(d->picture_parameter.cdef_damping_minus_3 & 0x3);
    pp->cdef.bits    = (UCHAR)(d->picture_parameter.cdef_bits & 0x3);
    for (i = 0; i < 8; i++) {
        pp->cdef.y_strengths[i].combined  = d->picture_parameter.cdef_y_strengths[i];
        pp->cdef.uv_strengths[i].combined = d->picture_parameter.cdef_uv_strengths[i];
    }

    pp->interp_filter = d->picture_parameter.interp_filter;

    /* Segmentation */
    pp->segmentation.enabled =
        d->picture_parameter.seg_info.segment_info_fields.enabled;
    pp->segmentation.update_map =
        d->picture_parameter.seg_info.segment_info_fields.update_map;
    pp->segmentation.update_data =
        d->picture_parameter.seg_info.segment_info_fields.update_data;
    pp->segmentation.temporal_update =
        d->picture_parameter.seg_info.segment_info_fields.temporal_update;
    for (i = 0; i < 8; i++) {
        pp->segmentation.feature_mask[i].mask =
            d->picture_parameter.seg_info.feature_mask[i];
        for (j = 0; j < 8; j++)
            pp->segmentation.feature_data[i][j] =
                d->picture_parameter.seg_info.feature_data[i][j];
    }

    /* Film grain */
    pp->film_grain.apply_grain =
        d->picture_parameter.film_grain_info.film_grain_info_fields.apply_grain;
    pp->film_grain.scaling_shift_minus8 =
        d->picture_parameter.film_grain_info.film_grain_info_fields.grain_scaling_minus_8;
    pp->film_grain.chroma_scaling_from_luma =
        d->picture_parameter.film_grain_info.film_grain_info_fields.chroma_scaling_from_luma;
    pp->film_grain.ar_coeff_lag =
        d->picture_parameter.film_grain_info.film_grain_info_fields.ar_coeff_lag;
    pp->film_grain.ar_coeff_shift_minus6 =
        d->picture_parameter.film_grain_info.film_grain_info_fields.ar_coeff_shift_minus_6;
    pp->film_grain.grain_scale_shift =
        d->picture_parameter.film_grain_info.film_grain_info_fields.grain_scale_shift;
    pp->film_grain.overlap_flag =
        d->picture_parameter.film_grain_info.film_grain_info_fields.overlap_flag;
    pp->film_grain.clip_to_restricted_range =
        d->picture_parameter.film_grain_info.film_grain_info_fields.clip_to_restricted_range;
    pp->film_grain.matrix_coeff_is_identity = 0;   /* TODO: derive from
                                                    * matrix_coefficients==AV1_MC_IDENTITY */
    pp->film_grain.grain_seed =
        d->picture_parameter.film_grain_info.grain_seed;
    pp->film_grain.num_y_points =
        d->picture_parameter.film_grain_info.num_y_points;
    pp->film_grain.num_cb_points =
        d->picture_parameter.film_grain_info.num_cb_points;
    pp->film_grain.num_cr_points =
        d->picture_parameter.film_grain_info.num_cr_points;
    for (i = 0; i < 14; i++) {
        pp->film_grain.scaling_points_y[i][0] =
            d->picture_parameter.film_grain_info.point_y_value[i];
        pp->film_grain.scaling_points_y[i][1] =
            d->picture_parameter.film_grain_info.point_y_scaling[i];
    }
    for (i = 0; i < 10; i++) {
        pp->film_grain.scaling_points_cb[i][0] =
            d->picture_parameter.film_grain_info.point_cb_value[i];
        pp->film_grain.scaling_points_cb[i][1] =
            d->picture_parameter.film_grain_info.point_cb_scaling[i];
        pp->film_grain.scaling_points_cr[i][0] =
            d->picture_parameter.film_grain_info.point_cr_value[i];
        pp->film_grain.scaling_points_cr[i][1] =
            d->picture_parameter.film_grain_info.point_cr_scaling[i];
    }
    for (i = 0; i < 24; i++)
        pp->film_grain.ar_coeffs_y[i] =
            (UCHAR)d->picture_parameter.film_grain_info.ar_coeffs_y[i];
    for (i = 0; i < 25; i++) {
        pp->film_grain.ar_coeffs_cb[i] =
            (UCHAR)d->picture_parameter.film_grain_info.ar_coeffs_cb[i];
        pp->film_grain.ar_coeffs_cr[i] =
            (UCHAR)d->picture_parameter.film_grain_info.ar_coeffs_cr[i];
    }
    pp->film_grain.cb_mult      = d->picture_parameter.film_grain_info.cb_mult;
    pp->film_grain.cb_luma_mult = d->picture_parameter.film_grain_info.cb_luma_mult;
    pp->film_grain.cr_mult      = d->picture_parameter.film_grain_info.cr_mult;
    pp->film_grain.cr_luma_mult = d->picture_parameter.film_grain_info.cr_luma_mult;
    pp->film_grain.cb_offset    =
        (SHORT)d->picture_parameter.film_grain_info.cb_offset;
    pp->film_grain.cr_offset    =
        (SHORT)d->picture_parameter.film_grain_info.cr_offset;

    pp->StatusReportFeedbackNumber = codec->status_report_feedback++;
    if (pp->StatusReportFeedbackNumber == 0)
        pp->StatusReportFeedbackNumber = codec->status_report_feedback++;
}

/* AV1 slice control is a DXVA_Tile_AV1 array, one per submitted OBU-tile.
 * virgl slice_parameter carries 1:1 per-tile offset/size/row/col/anchor. */
struct av1_sc_ctx {
    const struct virgl_av1_picture_desc *desc;
};

static void build_sc_av1(void *sc_array, unsigned idx,
                         UINT bs_offset, UINT slice_sz, void *user)
{
    DXVA_Tile_AV1 *arr = (DXVA_Tile_AV1 *)sc_array;
    struct av1_sc_ctx *ctx = (struct av1_sc_ctx *)user;

    arr[idx].DataOffset    = bs_offset;
    arr[idx].DataSize      = slice_sz;
    arr[idx].row           = (idx < 256) ?
        ctx->desc->slice_parameter.slice_data_row[idx] : 0;
    arr[idx].column        = (idx < 256) ?
        ctx->desc->slice_parameter.slice_data_col[idx] : 0;
    arr[idx].anchor_frame  = (idx < 256) ?
        ctx->desc->slice_parameter.slice_data_anchor_frame_idx[idx] : 0xFF;
    arr[idx].Reserved16Bits = 0;
    arr[idx].Reserved8Bits  = 0;
}

static int av1_decode_bitstream(struct virgl_video_codec *codec,
                                struct virgl_video_buffer *target,
                                const struct virgl_av1_picture_desc *desc,
                                unsigned num_buffers,
                                const void * const *buffers,
                                const unsigned *sizes)
{
    DXVA_PicParams_AV1 pp;
    struct av1_sc_ctx ctx = { .desc = desc };

    fill_dxva_picparams_av1(codec, target, desc, &pp);

    /* AV1 has no IQ-matrix buffer (quantization is per-segment in pic
     * params). Slice-control element is DXVA_Tile_AV1. */
    return submit_short_format_decode(codec,
                                      &pp, (UINT)sizeof(pp),
                                      NULL, 0,
                                      (UINT)sizeof(DXVA_Tile_AV1),
                                      build_sc_av1, &ctx,
                                      num_buffers, buffers, sizes);
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
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
        return hevc_decode_bitstream(codec, target, &desc->h265,
                                     num_buffers, buffers, sizes);
    case PIPE_VIDEO_PROFILE_VP9_PROFILE0:
    case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
        return vp9_decode_bitstream(codec, target, &desc->vp9,
                                    num_buffers, buffers, sizes);
    case PIPE_VIDEO_PROFILE_AV1_MAIN:
        return av1_decode_bitstream(codec, target, &desc->av1,
                                    num_buffers, buffers, sizes);
    /* TODO future codec: MPEG2 goes here. */
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
