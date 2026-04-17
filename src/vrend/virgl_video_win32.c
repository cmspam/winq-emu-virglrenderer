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
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

/* Some MinGW toolchains ship an older Media Foundation header set that
 * doesn't declare a handful of GUIDs / constants we need. Re-declare them
 * locally (guarded) so we don't depend on the system version.
 *
 * MFVideoFormat_HEVC lives in mfapi.h on recent SDKs but is missing on some
 * UCRT64 headers. MF_E_TRANSFORM_STREAM_CHANGE is similarly ancient. */
#ifndef MFVideoFormat_HEVC
DEFINE_GUID(MFVideoFormat_HEVC_local,
    0x43564548, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
#define VIRGL_MFVIDEOFORMAT_HEVC MFVideoFormat_HEVC_local
#else
#define VIRGL_MFVIDEOFORMAT_HEVC MFVideoFormat_HEVC
#endif

#ifndef MF_E_TRANSFORM_NEED_MORE_INPUT
#define MF_E_TRANSFORM_NEED_MORE_INPUT ((HRESULT)0xC00D6D72L)
#endif
#ifndef MF_E_TRANSFORM_STREAM_CHANGE
#define MF_E_TRANSFORM_STREAM_CHANGE   ((HRESULT)0xC00D6D61L)
#endif

/* MF_LOW_LATENCY attribute — {9C27891A-ED7A-40e1-88E8-B22727A024EE}. Hardware
 * MFTs interpret this as a hint to minimise internal buffering which in
 * practice also smooths over a number of "types not advertised until
 * configured" quirks on Intel/AMD H.264/HEVC encoders. Declared locally
 * because older MinGW Media Foundation headers don't export it. */
#ifndef MF_LOW_LATENCY
DEFINE_GUID(MF_LOW_LATENCY_local,
    0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);
#define VIRGL_MF_LOW_LATENCY MF_LOW_LATENCY_local
#else
#define VIRGL_MF_LOW_LATENCY MF_LOW_LATENCY
#endif

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

/* Size of the shared Decoded Picture Buffer (DPB) texture array owned by each
 * codec. DXVA uses RefFrameList[i].Index7Bits to identify which array slice of
 * the shared NV12/P010 texture the GPU decoder should sample as a reference
 * for inter-prediction. The DPB size must accommodate the worst-case number
 * of in-flight decoded pictures:
 *   H.264 Level 5.1: max_dec_frame_buffering = 16 + 1 current = 17
 *   HEVC Main 6.0:   sps_max_dec_pic_buffering_minus1 + 1 (<= 16) + 1 = 17
 *   VP9:             8 reference slots + 1 current = 9
 *   AV1:             8 reference slots + 1 current = 9
 * We pick 17 as a single value that fits all four codecs.
 *
 * The slot index fits in 7 bits (DXVA_PicEntry.Index7Bits), so the ceiling is
 * 127; 17 is comfortably below that. A 3840x2160 NV12 array at slice 17 is
 * ~106 MiB of GPU memory, which is acceptable. */
#define VIRGL_VIDEO_WIN32_DPB_SIZE 17

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

    /* Index of the array slice in the decoder's shared DPB texture
     * (codec->decode_tex_array) currently backing this buffer, or -1 when
     * unassigned. Set by virgl_video_begin_frame(); the inter-prediction
     * RefFrameList/RefPicList/ref_frame_map entries for downstream frames
     * reference this slot. current_codec_holder points at the codec whose
     * DPB holds the slot, so destroy_buffer can punch out the back-reference
     * when the buffer dies while still holding a slot. A buffer can only
     * live in one codec's DPB at a time in our design — DXVA pic entries
     * are scoped to a single decoder. */
    int current_slot_in_codec;
    struct virgl_video_codec *current_codec_holder;

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

    /*
     * Encode source-frame staging. On Windows we can't import the guest-side
     * GL texture into D3D11 directly (no WGL_NV_DX_interop2 yet), so
     * vrend_video.c reads the guest NV12/I420 planes back into CPU memory
     * with glGetTexImage and hands them in via
     * virgl_video_buffer_cpu_writeback(). We latch the bytes here until the
     * next encode_bitstream call consumes them in encoder_build_sample().
     *
     * Layout is canonical NV12: `width * height` bytes of Y, followed by
     * `width * height / 2` bytes of interleaved UV. Freed in
     * virgl_video_destroy_buffer().
     */
    uint8_t *encode_src_nv12;
    size_t   encode_src_size;
    size_t   encode_src_capacity;
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
     * 32-bit handle Mesa passes in desc->buffer_id[]) to its backing
     * virgl_video_buffer*. The slice the buffer currently occupies in the DPB
     * array is stored on the buffer itself (current_slot_in_codec). */
    struct {
        uint32_t buffer_id;            /* Guest ref id; 0 when slot unused */
        struct virgl_video_buffer *buf;
    } refs[VIRGL_VIDEO_WIN32_MAX_REFS];

    /* Shared Decoded Picture Buffer. DXVA inter-prediction requires every
     * reference frame (and the current frame) to live in slices of one shared
     * ID3D11Texture2D array; RefFrameList[i].Index7Bits identifies which slice
     * holds which reference. Per-buffer standalone textures cannot be fed to
     * the hardware decoder for multi-ref prediction.
     *
     * decode_tex_array is sized (codec->width, codec->height, ArraySize = DPB_SIZE).
     * slot_views[i] is an ID3D11VideoDecoderOutputView for array slice i.
     * slot_buffer[i] maps the slice back to the virgl_video_buffer currently
     *   occupying it, or NULL if the slot is free.
     * slot_last_used[i] is an LRU tick for eviction when all slots are in use. */
    ID3D11Texture2D *decode_tex_array;
    ID3D11VideoDecoderOutputView *slot_views[VIRGL_VIDEO_WIN32_DPB_SIZE];
    struct virgl_video_buffer *slot_buffer[VIRGL_VIDEO_WIN32_DPB_SIZE];
    uint64_t slot_last_used[VIRGL_VIDEO_WIN32_DPB_SIZE];
    uint64_t lru_tick;
    DXGI_FORMAT dpb_format;

    /* --------------------------------------------------------------
     * Encode-only state (populated lazily for ENCODE entrypoint codecs).
     * -------------------------------------------------------------- */
    IMFTransform *encoder_mft;         /* hardware MFT for H.264 / HEVC encode */
    bool          encoder_stream_started;
    uint32_t      encoder_bitrate;     /* most recent requested bitrate, bps */
    uint64_t      encoder_frame_count; /* number of frames fed to ProcessInput */
    GUID          encoder_output_guid; /* MFVideoFormat_H264 or _HEVC */
    unsigned      encoder_width;
    unsigned      encoder_height;
    unsigned      encoder_fps_num;
    unsigned      encoder_fps_den;
    /* Bitstream buffers queued from the most recent ProcessOutput. Cached so
     * the encode_completed callback can deliver them to the guest in one
     * shot matching the libva behaviour (which returns one coded buffer per
     * encode call). */
    uint8_t      *encoder_coded_buf;
    unsigned      encoder_coded_size;
    unsigned      encoder_coded_capacity;
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

    /* Media Foundation refcount. We call MFStartup() the first time an
     * encoder MFT is created and MFShutdown() when the last encode-capable
     * codec is destroyed. MFStartup internally refcounts but the
     * accompanying CoInitializeEx does not, so we track our own count too. */
    unsigned      mf_refcount;
    bool          mf_com_initialized;
} g_vid;

/* Forward decls for the encode helpers defined much further down. We keep
 * the encode implementation in one contiguous block at the bottom of the
 * file so the decoder path can ignore it, but fill_caps / create_codec /
 * destroy_codec need to reach in. */
static bool  profile_encode_supported(enum pipe_video_profile profile);
static int   encoder_create_mft(struct virgl_video_codec *codec,
                                const struct virgl_video_create_codec_args *args);
static void  encoder_destroy_mft(struct virgl_video_codec *codec);
static bool  have_hw_encoder_for(const GUID *output_subtype);
static HRESULT virgl_video_mf_acquire(void);
static void    virgl_video_mf_release(void);

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
 * Media Foundation init/shutdown — reference-counted. We only pay the cost
 * of CoInitializeEx / MFStartup on the first encoder created, and tear them
 * down when the last encoder is destroyed. Decoders don't touch this; they
 * use D3D11 video directly.
 * ---------------------------------------------------------------------------
 */

static HRESULT virgl_video_mf_acquire(void)
{
    HRESULT hr;

    if (g_vid.mf_refcount > 0) {
        g_vid.mf_refcount++;
        return S_OK;
    }

    /* MFStartup requires an initialized COM apartment. We use multithreaded
     * because virglrenderer callers may run on any thread. */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* S_FALSE means someone else has already initialized — not a failure. */
    if (hr == S_OK) {
        g_vid.mf_com_initialized = true;
    } else if (hr == S_FALSE) {
        /* Still add to our balancing count so teardown is symmetric. */
        g_vid.mf_com_initialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        /* Another component picked a different threading model. We can
         * still use MF, but must not call CoUninitialize from this thread. */
        g_vid.mf_com_initialized = false;
    } else if (FAILED(hr)) {
        virgl_error("virgl_video_win32: CoInitializeEx failed 0x%lx\n",
                    (unsigned long)hr);
        return hr;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: MFStartup failed 0x%lx\n",
                    (unsigned long)hr);
        if (g_vid.mf_com_initialized) {
            CoUninitialize();
            g_vid.mf_com_initialized = false;
        }
        return hr;
    }

    g_vid.mf_refcount = 1;
    return S_OK;
}

static void virgl_video_mf_release(void)
{
    if (g_vid.mf_refcount == 0)
        return;
    g_vid.mf_refcount--;
    if (g_vid.mf_refcount == 0) {
        MFShutdown();
        if (g_vid.mf_com_initialized) {
            CoUninitialize();
            g_vid.mf_com_initialized = false;
        }
    }
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
    v->max_level = 0;             /* let Mesa compute from attributes */
    v->stacked_frames = 0;
    /* Advertise up to 8K decode so guest Mesa can pick through. Actual
     * support is gated by CheckVideoDecoderFormat + CreateVideoDecoder. */
    /* Advertised cap (uint16 max_macroblocks overflows past ~4K @ 16x16;
     * Mesa uses max_macroblocks to gate profile advertisement in the guest
     * so keep it accurate. Actual decoder support is re-checked against
     * D3D11 at CreateVideoDecoder time — so larger clips still work if the
     * host driver accepts them; they just don't count toward the "supports
     * profile X" heuristic in the guest. */
    v->max_width = 3840;
    v->max_height = 2160;
    v->prefered_format = (profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10) ?
                         PIPE_FORMAT_P010 : PIPE_FORMAT_NV12;
    v->max_macroblocks = (3840 / 16) * (2160 / 16);
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
    v->max_level = 0;
    v->stacked_frames = 0;
    /* Advertised cap (uint16 max_macroblocks overflows past ~4K @ 16x16;
     * Mesa uses max_macroblocks to gate profile advertisement in the guest
     * so keep it accurate. Actual decoder support is re-checked against
     * D3D11 at CreateVideoDecoder time — so larger clips still work if the
     * host driver accepts them; they just don't count toward the "supports
     * profile X" heuristic in the guest. */
    v->max_width = 3840;
    v->max_height = 2160;
    v->prefered_format = (profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2) ?
                         PIPE_FORMAT_P010 : PIPE_FORMAT_NV12;
    /* VP9 "superblocks" are 64x64 but Mesa's cap accounting is in 16x16
     * macroblock equivalents, matching what the H.264/HEVC branches do. */
    v->max_macroblocks = (3840 / 16) * (2160 / 16);
    v->npot_texture = 1;
    v->supports_progressive = 1;
    v->supports_interlaced = 0;
    v->prefers_interlaced = 0;
    v->max_temporal_layers = 0;
}

/* Encode caps — fill an entry advertising the PIPE_VIDEO_ENTRYPOINT_ENCODE
 * path backed by a Media Foundation hardware MFT. */
static void fill_caps_for_encode(struct virgl_video_caps *v,
                                 enum pipe_video_profile profile)
{
    v->profile = profile;
    v->entrypoint = PIPE_VIDEO_ENTRYPOINT_ENCODE;
    v->max_level = 51;
    v->stacked_frames = 0;
    v->max_width = 3840;
    v->max_height = 2160;
    v->prefered_format = PIPE_FORMAT_NV12;
    v->max_macroblocks = (3840 / 16) * (2160 / 16);
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
    v->max_level = 0;
    v->stacked_frames = 0;
    /* Advertised cap (uint16 max_macroblocks overflows past ~4K @ 16x16;
     * Mesa uses max_macroblocks to gate profile advertisement in the guest
     * so keep it accurate. Actual decoder support is re-checked against
     * D3D11 at CreateVideoDecoder time — so larger clips still work if the
     * host driver accepts them; they just don't count toward the "supports
     * profile X" heuristic in the guest. */
    v->max_width = 3840;
    v->max_height = 2160;
    /* Profile 0 is 8-bit 4:2:0; output is always NV12 here. */
    v->prefered_format = PIPE_FORMAT_NV12;
    v->max_macroblocks = (3840 / 16) * (2160 / 16);
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

    /* Encode: advertise H.264 + HEVC if the host has matching hardware
     * MFTs. We briefly bring up Media Foundation for the enumeration and
     * shut it down right after — no need to keep MF alive when no codec is
     * open. */
    {
        bool have_h264_enc = false;
        bool have_hevc_enc = false;

        if (SUCCEEDED(virgl_video_mf_acquire())) {
            GUID h264 = MFVideoFormat_H264;
            GUID hevc = VIRGL_MFVIDEOFORMAT_HEVC;
            have_h264_enc = have_hw_encoder_for(&h264);
            have_hevc_enc = have_hw_encoder_for(&hevc);
            virgl_video_mf_release();
        }

        if (have_h264_enc) {
            static const enum pipe_video_profile profiles[] = {
                PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE,
                PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE,
                PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN,
                PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
            };
            for (i = 0; i < ARRAY_SIZE(profiles) &&
                        out < ARRAY_SIZE(caps->v2.video_caps); i++) {
                fill_caps_for_encode(&caps->v2.video_caps[out++],
                                     profiles[i]);
            }
        }
        if (have_hevc_enc && out < ARRAY_SIZE(caps->v2.video_caps)) {
            fill_caps_for_encode(&caps->v2.video_caps[out++],
                                 PIPE_VIDEO_PROFILE_HEVC_MAIN);
            if (out < ARRAY_SIZE(caps->v2.video_caps))
                fill_caps_for_encode(&caps->v2.video_caps[out++],
                                     PIPE_VIDEO_PROFILE_HEVC_MAIN_10);
        }

        virgl_warn("virgl_video_win32: fill_caps advertised %u profiles "
                   "(h264=%d hevc_main=%d hevc_m10=%d vp9_p0=%d vp9_p2=%d "
                   "av1=%d / enc h264=%d hevc=%d)\n",
                   out,
                   have_h264_nofgt, have_hevc_main, have_hevc_main10,
                   have_vp9_p0, have_vp9_p2, have_av1_p0,
                   have_h264_enc, have_hevc_enc);
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

    if (args->entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM &&
        args->entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE) {
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

    if (args->entrypoint == PIPE_VIDEO_ENTRYPOINT_ENCODE) {
        if (!profile_encode_supported(args->profile)) {
            virgl_error("virgl_video_win32: encode profile %d unsupported\n",
                        (int)args->profile);
            goto fail;
        }
        if (encoder_create_mft(codec, args) != 0)
            goto fail;
        return codec;
    }

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
    codec->dpb_format = desc.OutputFormat;

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

    /* Allocate the shared DPB texture array (one slice per in-flight frame)
     * and its per-slice decoder output views. DXVA inter-prediction reads
     * references out of array slices of a single NV12/P010 texture; each
     * view binds one slice for DecoderBeginFrame. */
    {
        D3D11_TEXTURE2D_DESC arr;
        unsigned s;
        uint32_t cw = (codec->width  + 1u) & ~1u;
        uint32_t ch = (codec->height + 1u) & ~1u;

        memset(&arr, 0, sizeof(arr));
        arr.Width = cw;
        arr.Height = ch;
        arr.MipLevels = 1;
        arr.ArraySize = VIRGL_VIDEO_WIN32_DPB_SIZE;
        arr.Format = codec->dpb_format;
        arr.SampleDesc.Count = 1;
        arr.Usage = D3D11_USAGE_DEFAULT;
        arr.BindFlags = D3D11_BIND_DECODER;
        arr.CPUAccessFlags = 0;
        arr.MiscFlags = 0;

        hr = ID3D11Device_CreateTexture2D(g_vid.device, &arr, NULL,
                                          &codec->decode_tex_array);
        if (FAILED(hr) || !codec->decode_tex_array) {
            virgl_error("virgl_video_win32: CreateTexture2D(DPB array) "
                        "%ux%ux%u failed: 0x%lx\n",
                        cw, ch, VIRGL_VIDEO_WIN32_DPB_SIZE,
                        (unsigned long)hr);
            goto fail;
        }

        for (s = 0; s < VIRGL_VIDEO_WIN32_DPB_SIZE; s++) {
            D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view;
            memset(&view, 0, sizeof(view));
            view.DecodeProfile = codec->dxva_profile;
            view.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
            view.Texture2D.ArraySlice = s;
            hr = ID3D11VideoDevice_CreateVideoDecoderOutputView(
                    g_vid.video_device,
                    (ID3D11Resource *)codec->decode_tex_array,
                    &view,
                    &codec->slot_views[s]);
            if (FAILED(hr) || !codec->slot_views[s]) {
                virgl_error("virgl_video_win32: CreateVideoDecoderOutputView "
                            "slot %u failed: 0x%lx\n", s, (unsigned long)hr);
                goto fail;
            }
            codec->slot_buffer[s] = NULL;
            codec->slot_last_used[s] = 0;
        }
        codec->lru_tick = 0;
    }

    return codec;

fail:
    virgl_video_destroy_codec(codec);
    return NULL;
}

void virgl_video_destroy_codec(struct virgl_video_codec *codec)
{
    unsigned s;

    if (!codec)
        return;

    /* Detach each slot from its occupant buffer so subsequent codecs can
     * reassign the buffer without a stale slot index hanging around. */
    for (s = 0; s < VIRGL_VIDEO_WIN32_DPB_SIZE; s++) {
        if (codec->slot_buffer[s]) {
            codec->slot_buffer[s]->current_slot_in_codec = -1;
            codec->slot_buffer[s]->current_codec_holder = NULL;
            codec->slot_buffer[s] = NULL;
        }
        if (codec->slot_views[s]) {
            ID3D11VideoDecoderOutputView_Release(codec->slot_views[s]);
            codec->slot_views[s] = NULL;
        }
    }
    if (codec->decode_tex_array) {
        ID3D11Texture2D_Release(codec->decode_tex_array);
        codec->decode_tex_array = NULL;
    }
    if (codec->decoder) {
        ID3D11VideoDecoder_Release(codec->decoder);
        codec->decoder = NULL;
    }
    encoder_destroy_mft(codec);
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
 *   - staging_tex: USAGE_STAGING, CPU-readable NV12/P010 texture; copy target
 *                  for the post-decode readback. Per-buffer (CPU readback is
 *                  not performance critical, and different buffers may be
 *                  read back concurrently).
 *
 * The actual decoder-writable texture lives on the codec as a shared
 * ID3D11Texture2D with ArraySize = VIRGL_VIDEO_WIN32_DPB_SIZE. begin_frame()
 * assigns one slice of that array to the buffer (tracked via
 * buffer->current_slot_in_codec); end_frame() copies from the slice into the
 * staging texture.
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
    buf->current_slot_in_codec = -1;

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

    /* If this buffer currently occupies a slot in a codec's DPB, punch out
     * the codec's back-reference so the slot is freed (and doesn't point at
     * released memory on the next LRU scan). The codec's refs[] table also
     * caches buffer pointers; clear matching entries there too. */
    if (buffer->current_codec_holder) {
        struct virgl_video_codec *c = buffer->current_codec_holder;
        unsigned i;
        if (buffer->current_slot_in_codec >= 0 &&
            buffer->current_slot_in_codec < VIRGL_VIDEO_WIN32_DPB_SIZE &&
            c->slot_buffer[buffer->current_slot_in_codec] == buffer) {
            c->slot_buffer[buffer->current_slot_in_codec] = NULL;
        }
        for (i = 0; i < VIRGL_VIDEO_WIN32_MAX_REFS; i++) {
            if (c->refs[i].buf == buffer) {
                c->refs[i].buf = NULL;
                /* buffer_id stays — the id-to-nonexistent-buf mapping will
                 * be skipped by the "buf must be live" guard in picparam
                 * fills. */
            }
        }
        buffer->current_codec_holder = NULL;
        buffer->current_slot_in_codec = -1;
    }

    if (buffer->staging_tex) {
        ID3D11Texture2D_Release(buffer->staging_tex);
        buffer->staging_tex = NULL;
    }
    if (buffer->encode_src_nv12) {
        free(buffer->encode_src_nv12);
        buffer->encode_src_nv12 = NULL;
        buffer->encode_src_size = 0;
        buffer->encode_src_capacity = 0;
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
 * DXVA inter-prediction requires each reference in RefFrameList[] /
 * RefPicList[] / ref_frame_map[] / RefFrameMapTextureIndex[] to carry the
 * DPB array-slice index of the corresponding prior decoded frame. The slice
 * index lives on the virgl_video_buffer itself (current_slot_in_codec,
 * assigned by begin_frame), but Mesa identifies references by their opaque
 * guest-visible buffer id, not by pointer. codec->refs[] is the
 * id-to-virgl_video_buffer* cache used when filling picparams: each time a
 * buffer is the target of begin_frame, or shows up as a reference in a
 * picture desc, we remember its pointer here so the next frame's picparam
 * marshalling can translate its id back to a buffer and thus to a DPB slot.
 * ---------------------------------------------------------------------------
 */

static void codec_remember_ref(struct virgl_video_codec *codec,
                               uint32_t buffer_id,
                               struct virgl_video_buffer *buf)
{
    unsigned i, free_slot = UINT_MAX;

    if (buffer_id == 0)
        return;

    for (i = 0; i < VIRGL_VIDEO_WIN32_MAX_REFS; i++) {
        if (codec->refs[i].buffer_id == buffer_id) {
            if (buf)
                codec->refs[i].buf = buf;
            return;
        }
        if (codec->refs[i].buffer_id == 0 && free_slot == UINT_MAX)
            free_slot = i;
    }
    if (free_slot == UINT_MAX)
        free_slot = 0;   /* round-robin evict slot 0 (rarely hit) */
    codec->refs[free_slot].buffer_id = buffer_id;
    codec->refs[free_slot].buf = buf;
}

/* Back-compat shim: accept the old function name so the per-codec picparam
 * marshalling can keep its existing "(void)codec_find_or_add_ref_slot(...)"
 * calls unchanged. Return value is ignored by all callers now — the slot
 * number they actually use comes from target->current_slot_in_codec. */
static int codec_find_or_add_ref_slot(struct virgl_video_codec *codec,
                                      uint32_t buffer_id,
                                      struct virgl_video_buffer *buf)
{
    codec_remember_ref(codec, buffer_id, buf);
    return 0;
}

/*
 * ---------------------------------------------------------------------------
 * begin_frame.
 * ---------------------------------------------------------------------------
 */

/* Assign an array slice in codec->decode_tex_array to `target`:
 *   1. If target already owns a slot on this codec, keep it (monotonic slot
 *      assignment is required for references from prior frames to stay
 *      valid).
 *   2. Otherwise pick a free slot (slot_buffer[i] == NULL).
 *   3. If none are free, evict the least-recently-used slot. The evicted
 *      buffer's current_slot_in_codec is cleared so any future picparam
 *      marshalling will emit the 0xFF "no reference" sentinel for it.
 *
 * target must match codec->dpb_format and codec dimensions — we checked that
 * at the top of begin_frame.
 */
static int codec_assign_dpb_slot(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *target)
{
    unsigned i;
    int chosen = -1;
    uint64_t oldest = UINT64_MAX;

    /* Case 1: already assigned. */
    if (target->current_codec_holder == codec &&
        target->current_slot_in_codec >= 0 &&
        target->current_slot_in_codec < VIRGL_VIDEO_WIN32_DPB_SIZE &&
        codec->slot_buffer[target->current_slot_in_codec] == target) {
        codec->slot_last_used[target->current_slot_in_codec] = ++codec->lru_tick;
        return target->current_slot_in_codec;
    }

    /* Case 2: target is attached to a *different* codec. Detach first. */
    if (target->current_codec_holder && target->current_codec_holder != codec) {
        struct virgl_video_codec *old = target->current_codec_holder;
        if (target->current_slot_in_codec >= 0 &&
            target->current_slot_in_codec < VIRGL_VIDEO_WIN32_DPB_SIZE &&
            old->slot_buffer[target->current_slot_in_codec] == target)
            old->slot_buffer[target->current_slot_in_codec] = NULL;
        target->current_codec_holder = NULL;
        target->current_slot_in_codec = -1;
    }

    /* Case 3: find a free slot. */
    for (i = 0; i < VIRGL_VIDEO_WIN32_DPB_SIZE; i++) {
        if (codec->slot_buffer[i] == NULL) {
            chosen = (int)i;
            break;
        }
    }

    /* Case 4: LRU-evict. In practice this only happens in adversarial streams
     * that reference more than DPB_SIZE frames. All real H.264/HEVC/VP9/AV1
     * streams have a max_dec_frame_buffering <= 16 so we should never hit
     * this path in well-formed content. */
    if (chosen < 0) {
        for (i = 0; i < VIRGL_VIDEO_WIN32_DPB_SIZE; i++) {
            if (codec->slot_last_used[i] < oldest) {
                oldest = codec->slot_last_used[i];
                chosen = (int)i;
            }
        }
        if (chosen >= 0 && codec->slot_buffer[chosen]) {
            struct virgl_video_buffer *victim = codec->slot_buffer[chosen];
            virgl_warn("virgl_video_win32: DPB full, evicting slot %d "
                       "(buf id=0x%x)\n", chosen, victim->id);
            victim->current_slot_in_codec = -1;
            victim->current_codec_holder  = NULL;
            codec->slot_buffer[chosen] = NULL;
        }
    }

    if (chosen < 0) {
        /* Should be unreachable — DPB_SIZE > 0, so the LRU walk always picks
         * something. Belt-and-suspenders fallback. */
        chosen = 0;
    }

    codec->slot_buffer[chosen] = target;
    codec->slot_last_used[chosen] = ++codec->lru_tick;
    target->current_slot_in_codec = chosen;
    target->current_codec_holder  = codec;
    return chosen;
}

int virgl_video_begin_frame(struct virgl_video_codec *codec,
                            struct virgl_video_buffer *target)
{
    HRESULT hr;
    int slot;

    if (!g_vid.initialized || !codec || !target || !codec->decoder)
        return -1;

    if (!codec->decode_tex_array) {
        virgl_error("virgl_video_win32: codec DPB array missing\n");
        return -1;
    }

    /* If the previous frame's map is still live, release it now. The guest's
     * Mesa driver has already copied out the bytes via the decode_completed
     * callback by the time it sends another begin_frame. */
    unmap_staging_if_needed(target);

    slot = codec_assign_dpb_slot(codec, target);
    if (slot < 0 || slot >= VIRGL_VIDEO_WIN32_DPB_SIZE ||
        !codec->slot_views[slot]) {
        virgl_error("virgl_video_win32: DPB slot assignment failed\n");
        return -1;
    }

    hr = ID3D11VideoContext_DecoderBeginFrame(g_vid.video_context,
                                              codec->decoder,
                                              codec->slot_views[slot],
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
    /* Per DXVA spec: bPicEntry = 0xFF (Index7Bits=0x7F, AssociatedFlag=1) is
     * the "no valid reference here" sentinel. Drivers reject other
     * combinations — e.g. Index7Bits=0x7F with AssociatedFlag=0 is interpreted
     * as a short-term reference at slot 127, which then reads garbage. */
    e->Index7Bits = 0x7F;
    e->AssociatedFlag = 1;
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

    /* Also cache the current target in the per-codec refs[] table so that
     * subsequent frames can resolve buffer_id -> virgl_video_buffer* ->
     * current_slot_in_codec when marshalling their ref lists. */
    (void)codec_find_or_add_ref_slot(codec, target->id, target);

    /* CurrPic.Index7Bits must be the DPB slot in codec->decode_tex_array that
     * the decoder is writing into this frame (set by begin_frame via
     * codec_assign_dpb_slot). AssociatedFlag is bottom-field-in-field-pair.
     */
    self_slot = target->current_slot_in_codec;
    if (self_slot < 0 || self_slot >= VIRGL_VIDEO_WIN32_DPB_SIZE)
        self_slot = 0;   /* should be unreachable: begin_frame assigns slot */
    pp->CurrPic.Index7Bits = (UCHAR)(self_slot & 0x7F);
    pp->CurrPic.AssociatedFlag = desc->field_pic_flag && desc->bottom_field_flag;

    pp->CurrFieldOrderCnt[0] = (INT)desc->field_order_cnt[0];
    pp->CurrFieldOrderCnt[1] = (INT)desc->field_order_cnt[1];
    pp->frame_num = (USHORT)desc->frame_num;
    /* DXVA num_ref_frames is the SPS-signalled maximum (H.264 sps
     * max_num_ref_frames), NOT the count of currently-active references in
     * RefFrameList[]. FFmpeg's dxva2_h264.c uses h->ps.sps->ref_frame_count.
     * Sending the active count breaks driver-side DPB sizing for streams
     * whose active ref count grows across frames (e.g. B-frames with
     * ref=4). */
    pp->num_ref_frames = desc->pps.sps.max_num_ref_frames
                             ? desc->pps.sps.max_num_ref_frames
                             : desc->num_ref_frames;

    /* --- bitfields (wBitFields packed USHORT) --- */
    pp->field_pic_flag = desc->field_pic_flag ? 1 : 0;
    /* H.264 7.4.3: MbaffFrameFlag = mb_adaptive_frame_field_flag &&
     * !field_pic_flag. Additionally, mb_adaptive_frame_field_flag is only
     * signalled when frame_mbs_only_flag=0, so guard on that explicitly —
     * a misbehaving SPS can leave stale bits set. */
    pp->MbaffFrameFlag = (!desc->pps.sps.frame_mbs_only_flag &&
                          desc->pps.sps.mb_adaptive_frame_field_flag &&
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
        struct virgl_video_buffer *refbuf = NULL;
        int slot = -1;
        unsigned j;

        if (bid == 0) {
            /* Empty ref slot. Per DXVA H.264 spec and FFmpeg's dxva2_h264.c,
             * the 0xFF sentinel (set above) is sufficient to mark an unused
             * entry. NonExistingFrameFlags is reserved for gaps-in-frame_num
             * generated non-existing refs, NOT for empty list positions —
             * setting it here misleads the driver's B-frame ref management. */
            continue;
        }

        /* Look up the referenced buffer by guest id. If we have a live
         * mapping, its DPB slot is what the GPU decoder needs to find the
         * reference pixels. */
        for (j = 0; j < VIRGL_VIDEO_WIN32_MAX_REFS; j++) {
            if (codec->refs[j].buffer_id == bid) {
                refbuf = codec->refs[j].buf;
                break;
            }
        }
        if (refbuf && refbuf->current_codec_holder == codec)
            slot = refbuf->current_slot_in_codec;

        if (slot < 0 || slot >= VIRGL_VIDEO_WIN32_DPB_SIZE) {
            /* Buffer either never decoded in this codec or has since been
             * evicted from the DPB. Leave RefFrameList[i] at the 0xFF
             * sentinel; do NOT set NonExistingFrameFlags (reserved for
             * actual gaps-in-frame_num, not list holes). */
            continue;
        }
        pp->RefFrameList[i].Index7Bits = (UCHAR)(slot & 0x7F);
        pp->RefFrameList[i].AssociatedFlag = desc->is_long_term[i] ? 1 : 0;
        pp->FieldOrderCntList[i][0] = (INT)desc->field_order_cnt_list[i][0];
        pp->FieldOrderCntList[i][1] = (INT)desc->field_order_cnt_list[i][1];
        /* Mesa stores frame_num for short-term refs and long-term pic num
         * (pic_id / LongTermPicNum) for long-term refs in frame_num_list[i].
         * When AssociatedFlag=1 (long-term) the DXVA driver interprets
         * FrameNumList[i] as LongTermPicNum automatically, which matches
         * ffmpeg's dxva2_h264.c and Mesa's va_dec_h264.c layout. */
        pp->FrameNumList[i] = (USHORT)desc->frame_num_list[i];

        /* UsedForReferenceFlags is 2 bits per entry: top + bottom. */
        if (desc->top_is_reference[i])
            pp->UsedForReferenceFlags |= (UINT)(1u << (i * 2 + 0));
        if (desc->bottom_is_reference[i])
            pp->UsedForReferenceFlags |= (UINT)(1u << (i * 2 + 1));
    }

    /* One-shot diagnostic: first few decodes, dump refs to confirm the DPB
     * array slices map correctly. Gate on a static counter — logging every
     * frame drowns the log with tens of KB. */
    {
        static unsigned logged = 0;
        if (logged < 6) {
            char rl[256] = {0};
            size_t off = 0;
            for (i = 0; i < 16 && i < desc->num_ref_frames; i++) {
                off += (size_t)snprintf(rl + off, sizeof(rl) - off,
                    " [%u]:bid=%u slot=%d LT=%u fn=%u",
                    i, desc->buffer_id[i],
                    (pp->RefFrameList[i].bPicEntry == 0xFF) ? -1 :
                        pp->RefFrameList[i].Index7Bits,
                    desc->is_long_term[i], desc->frame_num_list[i]);
                if (off >= sizeof(rl)) break;
            }
            virgl_warn("vid-h264 fr=%u curr_slot=%d num_refs=%u POC=%d/%d fields=%u%s\n",
                logged, pp->CurrPic.Index7Bits, desc->num_ref_frames,
                pp->CurrFieldOrderCnt[0], pp->CurrFieldOrderCnt[1],
                pp->UsedForReferenceFlags, rl);
            logged++;
        }
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

    /* Diag: first few frames, show first bytes of each slice buffer. */
    {
        static unsigned h264_slice_diag = 0;
        if (h264_slice_diag < 3) {
            char dbg[512]; size_t dbgoff = 0;
            dbgoff += (size_t)snprintf(dbg+dbgoff, sizeof(dbg)-dbgoff,
                    "h264 slice data: num_buffers=%u", num_buffers);
            for (i = 0; i < num_buffers && i < 8; i++) {
                const uint8_t *p = (const uint8_t *)buffers[i];
                if (!p || sizes[i] < 5) {
                    dbgoff += (size_t)snprintf(dbg+dbgoff, sizeof(dbg)-dbgoff,
                            " [%u]:sz=%u (empty)", i, sizes[i]);
                    continue;
                }
                dbgoff += (size_t)snprintf(dbg+dbgoff, sizeof(dbg)-dbgoff,
                        " [%u]:sz=%u b=%02x%02x%02x%02x%02x",
                        i, sizes[i], p[0], p[1], p[2], p[3], p[4]);
                if (dbgoff >= sizeof(dbg)) break;
            }
            virgl_warn("%s\n", dbg);
            h264_slice_diag++;
        }
    }

    /* Mesa's guest virgl VA driver delivers the whole picture in ONE buffer as
     * Annex B bytes (multiple NAL units with 0x00 0x00 0x01 or 0x00 0x00 0x00
     * 0x01 start codes). For multi-slice pictures we must split that stream
     * into one DXVA_Slice_H264_Short per slice NAL (nal_unit_type 1 or 5, per
     * H.264 Annex B §7.3.1). The picture params already describe the frame;
     * the slice control array is what tells the driver where each slice
     * starts. Scan for every start code and classify each NAL. */
    for (i = 0; i < num_buffers && slice_count < VIRGL_VIDEO_WIN32_MAX_SLICES; i++) {
        unsigned sz = sizes[i];
        const uint8_t *buf = (const uint8_t *)buffers[i];
        unsigned nal_offsets[VIRGL_VIDEO_WIN32_MAX_SLICES];
        unsigned nal_sizes[VIRGL_VIDEO_WIN32_MAX_SLICES];
        unsigned nal_count = 0;
        unsigned j;
        if (!buf || !sz)
            continue;
        if (bs_offset + sz > bs_buf_size) {
            virgl_error("virgl_video_win32: bitstream buffer overflow "
                        "(%u + %u > %u)\n", bs_offset, sz, bs_buf_size);
            ID3D11VideoContext_ReleaseDecoderBuffer(
                g_vid.video_context, codec->decoder,
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
            return -1;
        }
        /* Copy the whole Annex B blob verbatim — DXVA short format expects
         * NAL bytes including start codes. */
        memcpy((uint8_t *)bs_ptr + bs_offset, buf, sz);

        /* Walk NAL start codes to enumerate each NAL's [start, end). */
        for (j = 0; j + 3 < sz; ) {
            /* Match 0x000001 or 0x00000001 */
            unsigned sc_len = 0;
            if (buf[j] == 0 && buf[j+1] == 0) {
                if (buf[j+2] == 1) sc_len = 3;
                else if (buf[j+2] == 0 && j + 3 < sz && buf[j+3] == 1) sc_len = 4;
            }
            if (!sc_len) { j++; continue; }
            unsigned nal_start = j + sc_len;
            if (nal_start >= sz) break;
            uint8_t nal_byte = buf[nal_start];
            uint8_t nal_type = nal_byte & 0x1F;
            /* Slice NAL types: 1 (non-IDR) and 5 (IDR). Others (SPS=7, PPS=8,
             * SEI=6, AUD=9, etc.) are prefix NALs that DXVA doesn't consume
             * individually but must remain present in the bitstream blob. */
            if (nal_type == 1 || nal_type == 5 || nal_type == 20) {
                /* Find the NEXT start code to size this NAL. Include the
                 * current start code in the NAL bytes so the DXVA driver sees
                 * the Annex B prefix (it's what short format expects). */
                unsigned next = nal_start;
                while (next + 2 < sz) {
                    if (buf[next] == 0 && buf[next+1] == 0 &&
                        (buf[next+2] == 1 ||
                         (buf[next+2] == 0 && next + 3 < sz && buf[next+3] == 1)))
                        break;
                    next++;
                }
                if (next + 2 >= sz) next = sz;
                if (nal_count < VIRGL_VIDEO_WIN32_MAX_SLICES) {
                    nal_offsets[nal_count] = j;       /* include start code */
                    nal_sizes[nal_count]   = next - j;
                    nal_count++;
                }
                j = next;
            } else {
                j = nal_start;
            }
        }

        if (nal_count == 0) {
            /* No slice NAL found (shouldn't happen for a valid frame); treat
             * the whole buffer as one slice to avoid losing data. */
            slices[slice_count].BSNALunitDataLocation = bs_offset;
            slices[slice_count].SliceBytesInBuffer   = sz;
            slices[slice_count].wBadSliceChopping    = DXVA_SLICE_CHOPPING_NONE;
            slice_count++;
        } else {
            for (j = 0; j < nal_count &&
                        slice_count < VIRGL_VIDEO_WIN32_MAX_SLICES; j++) {
                slices[slice_count].BSNALunitDataLocation = bs_offset + nal_offsets[j];
                slices[slice_count].SliceBytesInBuffer   = nal_sizes[j];
                slices[slice_count].wBadSliceChopping    = DXVA_SLICE_CHOPPING_NONE;
                slice_count++;
            }
        }

        bs_offset += sz;
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

    /* CurrPic: Index7Bits is the DPB slot in codec->decode_tex_array that
     * the decoder is writing into this frame (assigned by begin_frame). */
    (void)codec_find_or_add_ref_slot(codec, target->id, target);
    self_slot = target->current_slot_in_codec;
    if (self_slot < 0 || self_slot >= VIRGL_VIDEO_WIN32_DPB_SIZE)
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
     * AssociatedFlag bit; we mark unused slots with bPicEntry=0xFF. We
     * preserve Mesa's positional mapping: desc->ref[i] -> RefPicList[i]
     * so that desc->RefPicSetStCurr{Before,After,LtCurr}[] indices
     * (which reference positions in desc->ref[]) remain valid indices
     * into pp->RefPicList[]. This matches the VA-API ReferenceFrames[]
     * convention that Mesa serialises one-for-one from.
     *
     * PicOrderCntValList[i] must be populated from desc->PicOrderCntVal[i]
     * unconditionally — even for invalid RefPicList entries. The driver
     * indexes PicOrderCntValList by the same i it reads RefPicList[] at
     * when resolving RefPicSetStCurr*, and a stale zero POC for a
     * supposedly-invalid slot that RefPicSet still references produces
     * wrong MV scaling on B-frames. This matches ffmpeg dxva2_hevc.c
     * which copies PicOrderCntVal[] verbatim. */
    for (i = 0; i < 15; i++) {
        dxva_picentry_hevc_invalidate(&pp->RefPicList[i]);
        pp->PicOrderCntValList[i] = desc->PicOrderCntVal[i];
    }
    for (i = 0; i < 15; i++) {
        uint32_t bid = desc->ref[i];
        struct virgl_video_buffer *refbuf = NULL;
        int slot = -1;
        unsigned j;

        if (bid == 0)
            continue;

        for (j = 0; j < VIRGL_VIDEO_WIN32_MAX_REFS; j++) {
            if (codec->refs[j].buffer_id == bid) {
                refbuf = codec->refs[j].buf;
                break;
            }
        }
        if (refbuf && refbuf->current_codec_holder == codec)
            slot = refbuf->current_slot_in_codec;
        if (slot < 0 || slot >= VIRGL_VIDEO_WIN32_DPB_SIZE) {
            /* Ref not resident in our DPB — leave the 0xFF sentinel in
             * place. PicOrderCntValList[i] is already filled above. */
            continue;
        }
        pp->RefPicList[i].Index7Bits    = (UCHAR)(slot & 0x7F);
        pp->RefPicList[i].AssociatedFlag = desc->IsLongTerm[i] ? 1 : 0;
    }

    /* Ref-pic-set indices into RefPicList[]: curr-before, curr-after,
     * lt-curr. Pad unused positions with 0xFF — Intel's HEVC driver
     * treats any non-0xFF entry past Num* as a valid reference and
     * reads phantom refs on B-frames otherwise. */
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

/* Parse-mode for how to split incoming virgl bitstream buffers into DXVA
 * slice-control entries. Needed because Mesa delivers all slices of a
 * picture concatenated in one buffer as Annex B bytes; short-format DXVA
 * wants one slice-control entry per slice NAL. */
enum virgl_video_parse_mode {
    VIRGL_VIDEO_PARSE_NONE,    /* one entry per input buffer (VP9, AV1) */
    VIRGL_VIDEO_PARSE_H264,    /* Annex B, 1-byte NAL header; slice types 1/5/20 */
    VIRGL_VIDEO_PARSE_HEVC,    /* Annex B, 2-byte NAL header; VCL types 0..31 */
};

/* Find the next Annex B start code (00 00 01 or 00 00 00 01) starting at
 * offset `from` within [buf, buf+sz). Returns either the offset of the
 * start code or `sz` if not found. Writes the start-code length (3 or 4)
 * to `*sc_len_out` when a code is found. */
static unsigned annex_b_next_start(const uint8_t *buf, unsigned sz,
                                   unsigned from, unsigned *sc_len_out)
{
    unsigned j;
    for (j = from; j + 2 < sz; j++) {
        if (buf[j] != 0 || buf[j+1] != 0)
            continue;
        if (buf[j+2] == 1) { if (sc_len_out) *sc_len_out = 3; return j; }
        if (buf[j+2] == 0 && j + 3 < sz && buf[j+3] == 1) {
            if (sc_len_out) *sc_len_out = 4;
            return j;
        }
    }
    return sz;
}

/* True if this NAL's first byte (H.264) or first two bytes (HEVC) identify
 * a VCL / slice NAL that should be reported to DXVA as its own slice-
 * control entry. Non-slice NAL units (SPS/PPS/SEI/VPS/etc.) must remain
 * embedded in the bitstream blob but are NOT listed in slice control. */
static int nal_is_slice(enum virgl_video_parse_mode mode,
                        const uint8_t *nal_bytes, unsigned nal_avail)
{
    if (nal_avail < 1)
        return 0;
    switch (mode) {
    case VIRGL_VIDEO_PARSE_H264: {
        uint8_t t = nal_bytes[0] & 0x1F;
        return (t == 1 || t == 5 || t == 20);
    }
    case VIRGL_VIDEO_PARSE_HEVC: {
        if (nal_avail < 2) return 0;
        uint8_t t = (nal_bytes[0] >> 1) & 0x3F;
        /* HEVC VCL NAL types are 0..31. IRAP (16..21) and non-IRAP (0..9)
         * are slice NALs; reserved (10..15, 22..31) would also be treated
         * as VCL. Non-VCL (32..47) are headers etc. */
        return t <= 31;
    }
    case VIRGL_VIDEO_PARSE_NONE:
    default:
        return 0;
    }
}

static int submit_short_format_decode(struct virgl_video_codec *codec,
                                      const void *pp, UINT pp_size,
                                      const void *qm, UINT qm_size,
                                      UINT sc_elem_size,
                                      build_sc_entry_fn build_sc,
                                      void *build_sc_user,
                                      enum virgl_video_parse_mode parse_mode,
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
        const uint8_t *buf = (const uint8_t *)buffers[i];
        if (!buf || !sz)
            continue;
        if (bs_offset + sz > bs_buf_size) {
            virgl_error("virgl_video_win32: bitstream buffer overflow "
                        "(%u + %u > %u)\n", bs_offset, sz, bs_buf_size);
            ID3D11VideoContext_ReleaseDecoderBuffer(
                g_vid.video_context, codec->decoder,
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
            return -1;
        }
        /* Always copy the whole blob — non-slice NAL units must stay embedded
         * so the decoder can find SPS/PPS/VPS/SEI. Only the slice-control
         * table (built below) differs per parse mode. */
        memcpy((uint8_t *)bs_ptr + bs_offset, buf, sz);

        if (parse_mode == VIRGL_VIDEO_PARSE_NONE) {
            /* Whole input buffer is one slice to DXVA. Used for VP9 / AV1
             * where Mesa delivers each frame as a single blob the driver
             * parses internally. */
            if (slice_count < VIRGL_VIDEO_WIN32_MAX_SLICES) {
                build_sc(slice_storage, slice_count, bs_offset, sz,
                         build_sc_user);
                slice_count++;
            }
        } else {
            /* Walk Annex B NAL units, emit a slice-control entry per
             * VCL slice NAL. Preserves start codes in the entry's byte
             * range — DXVA short-format wants that framing. */
            unsigned j = 0;
            while (j + 3 < sz && slice_count < VIRGL_VIDEO_WIN32_MAX_SLICES) {
                unsigned sc_len = 0;
                unsigned at = annex_b_next_start(buf, sz, j, &sc_len);
                if (at >= sz) break;
                unsigned nal_hdr = at + sc_len;
                if (nal_hdr >= sz) break;
                unsigned end = annex_b_next_start(buf, sz, nal_hdr, NULL);
                if (nal_is_slice(parse_mode, buf + nal_hdr, sz - nal_hdr)) {
                    build_sc(slice_storage, slice_count,
                             bs_offset + at, end - at, build_sc_user);
                    slice_count++;
                }
                j = end;
            }
            /* Fallback: no slice NAL located — treat the whole buffer as one
             * slice rather than dropping the picture. */
            if (slice_count == 0 && sz > 0) {
                build_sc(slice_storage, slice_count, bs_offset, sz,
                         build_sc_user);
                slice_count++;
            }
        }
        bs_offset += sz;
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
                                      VIRGL_VIDEO_PARSE_HEVC,
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

    /* Cache the current target and pick its DPB slot (assigned by
     * begin_frame). */
    (void)codec_find_or_add_ref_slot(codec, target->id, target);
    self_slot = target->current_slot_in_codec;
    if (self_slot < 0 || self_slot >= VIRGL_VIDEO_WIN32_DPB_SIZE)
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
        int slot = -1;
        struct virgl_video_buffer *refbuf = NULL;
        unsigned j;

        if (bid == 0)
            continue;

        /* Find tracked buf to extract coded width/height. */
        for (j = 0; j < VIRGL_VIDEO_WIN32_MAX_REFS; j++) {
            if (codec->refs[j].buffer_id == bid) {
                refbuf = codec->refs[j].buf;
                break;
            }
        }
        if (refbuf && refbuf->current_codec_holder == codec)
            slot = refbuf->current_slot_in_codec;
        if (slot < 0 || slot >= VIRGL_VIDEO_WIN32_DPB_SIZE)
            continue;   /* leave 0xFF sentinel in place */
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

    /* frame_refs[3]: last, golden, altref — indices into ref_frame_map.
     * Per the DXVA VP9 spec, AssociatedFlag for these entries carries the
     * reference sign bias for each ref (0 = no bias, 1 = negative bias).
     * Leaving them at 0 breaks MV scaling on any P-frame where the alt
     * ref has a positive-direction sign bias (most non-trivial VP9
     * content), which matches the slight-but-steady degradation we see
     * from frame 2 onward on multi-ref VP9 streams. Also propagated in
     * pp->ref_frame_sign_bias[] below; DXVA expects both. */
    for (i = 0; i < 3; i++)
        dxva_picentry_vpx_invalidate(&pp->frame_refs[i]);
    pp->frame_refs[0].Index7Bits =
        (UCHAR)(d->picture_parameter.pic_fields.last_ref_frame & 0x7);
    pp->frame_refs[0].AssociatedFlag =
        d->picture_parameter.pic_fields.last_ref_frame_sign_bias ? 1 : 0;
    pp->frame_refs[1].Index7Bits =
        (UCHAR)(d->picture_parameter.pic_fields.golden_ref_frame & 0x7);
    pp->frame_refs[1].AssociatedFlag =
        d->picture_parameter.pic_fields.golden_ref_frame_sign_bias ? 1 : 0;
    pp->frame_refs[2].Index7Bits =
        (UCHAR)(d->picture_parameter.pic_fields.alt_ref_frame & 0x7);
    pp->frame_refs[2].AssociatedFlag =
        d->picture_parameter.pic_fields.alt_ref_frame_sign_bias ? 1 : 0;

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
                                      VIRGL_VIDEO_PARSE_NONE,
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
    bool uses_lr;
    bool apply_grain;

    memset(pp, 0, sizeof(*pp));

    pp->width       = d->picture_parameter.frame_width;
    pp->height      = d->picture_parameter.frame_height;
    pp->max_width   = d->picture_parameter.max_width;
    pp->max_height  = d->picture_parameter.max_height;

    /* CurrPicTextureIndex: DPB slot in codec->decode_tex_array the decoder
     * is writing into this frame (assigned by begin_frame). */
    (void)codec_find_or_add_ref_slot(codec, target->id, target);
    self_slot = target->current_slot_in_codec;
    if (self_slot < 0 || self_slot >= VIRGL_VIDEO_WIN32_DPB_SIZE)
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
     * heights per-tile-col / per-tile-row. Only the first tile_cols / tile_rows
     * entries are meaningful; leave the remainder zero (ffmpeg does the same).
     *
     * IMPORTANT: DXVA's tiles.widths[i] / tiles.heights[i] fields are
     * TileWidthInSbMinus1 / TileHeightInSbMinus1 — that is, the tile size
     * in superblocks MINUS ONE (per MS-DXVA AV1 spec and ffmpeg's
     * dxva2_av1.c). Mesa's virgl_av1_picture_desc.width_in_sbs[i] /
     * height_in_sbs[i] hold the RAW count (TileWidthInSb), matching the
     * AV1 bitstream order but NOT DXVA's minus-1 convention. Subtract 1
     * before stuffing into DXVA — without this the driver reads one
     * extra SB per tile and produces corrupt output across every frame
     * (intra and inter alike). */
    pp->tiles.cols = d->picture_parameter.tile_cols;
    pp->tiles.rows = d->picture_parameter.tile_rows;
    pp->tiles.context_update_id = d->picture_parameter.context_update_tile_id;
    for (i = 0; i < pp->tiles.cols && i < 64; i++) {
        uint16_t w = d->picture_parameter.width_in_sbs[i];
        pp->tiles.widths[i] = (USHORT)(w > 0 ? w - 1 : 0);
    }
    for (i = 0; i < pp->tiles.rows && i < 64; i++) {
        uint16_t h = d->picture_parameter.height_in_sbs[i];
        pp->tiles.heights[i] = (USHORT)(h > 0 ? h - 1 : 0);
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
    /* restoration is derived from per-plane frame_restoration_type values
     * further below (uses_lr). virgl doesn't carry seq->enable_restoration
     * directly, but if any plane has restoration enabled the sequence header
     * must have enabled it. */
    pp->coding.restoration = 0;
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
    /* ffmpeg always sets reference_frame_update=1 (the driver resolves the
     * actual ref update from the frame header); matching that for safety. */
    pp->coding.reference_frame_update = 1;

    /* FormatAndPictureInfoFlags */
    pp->format.frame_type =
        d->picture_parameter.pic_info_fields.frame_type;
    pp->format.show_frame =
        d->picture_parameter.pic_info_fields.show_frame;
    pp->format.showable_frame =
        d->picture_parameter.pic_info_fields.showable_frame;
    /* virgl_av1_picture_desc does not expose seq_info.color_config.subsampling_x/y
     * (Mesa's VA-API frontend comments them out when filling the desc). Derive
     * from seq_profile per AV1 spec:
     *   profile 0 => 4:2:0 (subsampling_x=1, subsampling_y=1)
     *   profile 1 => 4:4:4 (0, 0)
     *   profile 2 => varies by bit_depth (12-bit can be 4:2:2, otherwise 4:2:0).
     * For mono_chrome the subsampling bits are formally (1,1) per spec. */
    if (d->picture_parameter.seq_info_fields.mono_chrome) {
        pp->format.subsampling_x = 1;
        pp->format.subsampling_y = 1;
    } else if (d->picture_parameter.profile == 1) {
        pp->format.subsampling_x = 0;
        pp->format.subsampling_y = 0;
    } else if (d->picture_parameter.profile == 2 &&
               d->picture_parameter.bit_depth_idx == 2) {
        /* Profile 2 / 12-bit can be 4:2:2; without subsampling bits in desc
         * we conservatively assume 4:2:2 (subsampling_x=1, subsampling_y=0). */
        pp->format.subsampling_x = 1;
        pp->format.subsampling_y = 0;
    } else {
        /* Profile 0 and Profile 2 at <=10-bit: 4:2:0. */
        pp->format.subsampling_x = 1;
        pp->format.subsampling_y = 1;
    }
    pp->format.mono_chrome =
        d->picture_parameter.seq_info_fields.mono_chrome;

    pp->primary_ref_frame = d->picture_parameter.primary_ref_frame;
    pp->order_hint        = d->picture_parameter.order_hint;
    /* AV1 spec: OrderHintBits is 0 when enable_order_hint is false; otherwise
     * order_hint_bits_minus_1 + 1. ffmpeg follows this convention. */
    pp->order_hint_bits   =
        d->picture_parameter.seq_info_fields.enable_order_hint ?
        (UCHAR)(d->picture_parameter.order_hint_bits_minus_1 + 1) : 0;

    /* RefFrameMapTextureIndex: 8 entries, mapping AV1 ref-slot -> decode
     * texture slot. ffmpeg fills this first then uses ref_frame_idx[i] to
     * index into it for the frame_refs[] entries. */
    for (i = 0; i < 8; i++)
        pp->RefFrameMapTextureIndex[i] = 0xFF;
    for (i = 0; i < 8; i++) {
        uint32_t bid = d->ref[i];
        struct virgl_video_buffer *refbuf = NULL;
        int slot = -1;
        unsigned k;
        if (bid == 0)
            continue;
        for (k = 0; k < VIRGL_VIDEO_WIN32_MAX_REFS; k++) {
            if (codec->refs[k].buffer_id == bid) {
                refbuf = codec->refs[k].buf;
                break;
            }
        }
        if (refbuf && refbuf->current_codec_holder == codec)
            slot = refbuf->current_slot_in_codec;
        if (slot < 0 || slot >= VIRGL_VIDEO_WIN32_DPB_SIZE)
            continue;
        pp->RefFrameMapTextureIndex[i] = (UCHAR)(slot & 0x7F);
    }

    /* frame_refs[7]: per-ref width/height/global-motion. Index references an
     * entry in RefFrameMapTextureIndex[] (i.e. the AV1 ref-buffer slot, 0-7),
     * not a texture slot directly. If the referenced slot has no backing
     * frame, Index must be 0xFF (sentinel) per the DXVA spec. */
    for (i = 0; i < 7; i++) {
        uint8_t ref_idx = d->picture_parameter.ref_frame_idx[i];
        struct virgl_video_buffer *refbuf = NULL;
        uint32_t ref_w = 0, ref_h = 0;

        memset(&pp->frame_refs[i], 0, sizeof(pp->frame_refs[i]));

        if (ref_idx < 8) {
            uint32_t bid = d->ref[ref_idx];
            /* Find tracked buf to extract coded width/height. */
            if (bid != 0) {
                for (j = 0; j < VIRGL_VIDEO_WIN32_MAX_REFS; j++) {
                    if (codec->refs[j].buffer_id == bid) {
                        refbuf = codec->refs[j].buf;
                        break;
                    }
                }
            }
            if (refbuf) {
                ref_w = refbuf->width;
                ref_h = refbuf->height;
                pp->frame_refs[i].Index = ref_idx;
            } else if (pp->RefFrameMapTextureIndex[ref_idx] != 0xFF) {
                /* Slot has a texture entry but we didn't cache dims; fall
                 * back to current-frame size (same as ffmpeg with a NULL
                 * AVFrame, but with non-zero dims so driver doesn't trip). */
                ref_w = d->picture_parameter.frame_width;
                ref_h = d->picture_parameter.frame_height;
                pp->frame_refs[i].Index = ref_idx;
            } else {
                pp->frame_refs[i].Index = 0xFF;
            }
        } else {
            pp->frame_refs[i].Index = 0xFF;
        }

        pp->frame_refs[i].width  = ref_w;
        pp->frame_refs[i].height = ref_h;
        pp->frame_refs[i].wminvalid = d->picture_parameter.wm[i].invalid ? 1 : 0;
        pp->frame_refs[i].wmtype    = (UCHAR)(d->picture_parameter.wm[i].wmtype & 0x3);
        for (j = 0; j < 6; j++)
            pp->frame_refs[i].wmmat[j] = d->picture_parameter.wm[i].wmmat[j];
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
    /* frame_restoration_type: both VA-API and DXVA encode the AV1 spec
     * enum {NONE=0, WIENER=1, SGRPROJ=2, SWITCHABLE=3} directly. Mesa fills
     * the virgl desc from the VA-API value, so passthrough is correct (no
     * lr_type bitstream remap needed here). */
    pp->loop_filter.frame_restoration_type[0] =
        (UCHAR)d->picture_parameter.loop_restoration_fields.yframe_restoration_type;
    pp->loop_filter.frame_restoration_type[1] =
        (UCHAR)d->picture_parameter.loop_restoration_fields.cbframe_restoration_type;
    pp->loop_filter.frame_restoration_type[2] =
        (UCHAR)d->picture_parameter.loop_restoration_fields.crframe_restoration_type;

    /* DXVA wants log2(unit_size), NOT the raw unit size. Mesa stores the
     * actual size (1 << (6 + lr_unit_shift)) in lr_unit_size[], so we have
     * to derive log2 from loop_restoration_fields directly (matching the
     * ffmpeg formula: uses_lr ? (6 + lr_unit_shift) : 8 for Y; subtract
     * lr_uv_shift for U/V). This is the single biggest AV1 marshalling bug
     * — passing 128/256 where the driver expected 7/8 was causing systemic
     * plane corruption across the entire frame. */
    uses_lr = (pp->loop_filter.frame_restoration_type[0] ||
               pp->loop_filter.frame_restoration_type[1] ||
               pp->loop_filter.frame_restoration_type[2]);
    {
        uint8_t lr_unit_shift =
            d->picture_parameter.loop_restoration_fields.lr_unit_shift;
        uint8_t lr_uv_shift =
            d->picture_parameter.loop_restoration_fields.lr_uv_shift;
        pp->loop_filter.log2_restoration_unit_size[0] =
            uses_lr ? (USHORT)(6 + lr_unit_shift) : 8;
        pp->loop_filter.log2_restoration_unit_size[1] =
            uses_lr ? (USHORT)(6 + lr_unit_shift - lr_uv_shift) : 8;
        pp->loop_filter.log2_restoration_unit_size[2] =
            pp->loop_filter.log2_restoration_unit_size[1];
    }

    /* Set loop-restoration enable bit based on whether any plane has it on.
     * (virgl desc doesn't carry seq->enable_restoration directly, but if any
     * plane has restoration the sequence header must have enabled it.) */
    pp->coding.restoration = uses_lr ? 1 : 0;

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
    /* qm_y/u/v must be 0xFF when qmatrix is disabled (ffmpeg convention;
     * drivers use 0xFF as the "no qmatrix" sentinel). */
    if (d->picture_parameter.qmatrix_fields.using_qmatrix) {
        pp->quantization.qm_y = (UCHAR)d->picture_parameter.qmatrix_fields.qm_y;
        pp->quantization.qm_u = (UCHAR)d->picture_parameter.qmatrix_fields.qm_u;
        pp->quantization.qm_v = (UCHAR)d->picture_parameter.qmatrix_fields.qm_v;
    } else {
        pp->quantization.qm_y = 0xFF;
        pp->quantization.qm_u = 0xFF;
        pp->quantization.qm_v = 0xFF;
    }

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

    /* Film grain: only populate when apply_grain is set. ffmpeg zeroes the
     * whole struct and only fills when apply_grain — driver reads stale AR
     * coefficients / scaling points otherwise and produces garbage output. */
    apply_grain =
        d->picture_parameter.film_grain_info.film_grain_info_fields.apply_grain;
    if (apply_grain) {
        pp->film_grain.apply_grain              = 1;
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
        /* matrix_coeff_is_identity: 1 iff seq->color_config.matrix_coefficients
         * == AV1_MC_IDENTITY (which maps to AVCOL_SPC_RGB = 0). */
        pp->film_grain.matrix_coeff_is_identity =
            (d->picture_parameter.matrix_coefficients == 0) ? 1 : 0;
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
    }

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
    /* ffmpeg always uses 0xFF as the sentinel here; Mesa's VA-API frontend
     * does not track anchor frames, so the raw byte can be a stale zero
     * that confuses the driver. Match ffmpeg unconditionally. */
    arr[idx].anchor_frame  = 0xFF;
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
                                      VIRGL_VIDEO_PARSE_NONE,
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

/*
 * ---------------------------------------------------------------------------
 * Encode implementation — Media Foundation-backed H.264 / HEVC hardware MFT.
 *
 * Flow:
 *   - virgl_video_create_codec() with entrypoint=ENCODE allocates the codec
 *     shell and calls encoder_create_mft() to enumerate a hardware MFT
 *     matching the requested profile.
 *   - On the first virgl_video_encode_bitstream() we set input/output media
 *     types and send MFT_MESSAGE_NOTIFY_BEGIN_STREAMING.
 *   - Each call wraps the NV12 source frame into an IMFSample, feeds it to
 *     ProcessInput, then drains ProcessOutput until the MFT says "need more
 *     input" and delivers the coded bytes via encode_completed.
 *   - virgl_video_destroy_codec() releases the MFT and drops the MF refcount.
 *
 * NV12 staging texture model: the encode source comes in as a
 * virgl_video_buffer whose guest-side GL texture was read back to CPU by
 * vrend_video.c and stashed in buf->encode_src_nv12 via
 * virgl_video_buffer_cpu_writeback(). We memcpy that slab into an
 * IMFMediaBuffer. The (rarely-hit) fallback path also Maps() buf->staging_tex
 * to read whatever was most recently copied there by an end_frame decode.
 * ---------------------------------------------------------------------------
 */

static bool profile_encode_supported(enum pipe_video_profile profile)
{
    /* We advertise H.264 + HEVC encode; AV1/VP9 encoders in Media Foundation
     * are either nonexistent or flaky on current Intel/AMD drivers. */
    switch (profile) {
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
        return true;
    default:
        return false;
    }
}

static bool profile_encode_output_guid(enum pipe_video_profile profile,
                                       GUID *out)
{
    switch (profile) {
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
        *out = MFVideoFormat_H264;
        return true;
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
        *out = VIRGL_MFVIDEOFORMAT_HEVC;
        return true;
    default:
        return false;
    }
}

/* Pack frame_rate_num/den into the upper/lower 32-bits of a UINT64 per MF
 * conventions (MFFrameRate). The MFSetAttributeSize / MFSetAttributeRatio
 * helpers in mfapi.h are C++-only inline functions on MinGW — we have to
 * reimplement them as a raw SetUINT64 call. */
static UINT64 mf_pack_ratio(UINT32 high, UINT32 low)
{
    return (((UINT64)high) << 32) | (UINT64)low;
}

static HRESULT mf_set_attr_size(IMFAttributes *attrs, REFGUID key,
                                UINT32 width, UINT32 height)
{
    return IMFAttributes_SetUINT64(attrs, key, mf_pack_ratio(width, height));
}

static HRESULT mf_set_attr_ratio(IMFAttributes *attrs, REFGUID key,
                                 UINT32 num, UINT32 den)
{
    return IMFAttributes_SetUINT64(attrs, key, mf_pack_ratio(num, den));
}

/* Enumerate hardware MFTs for the given output codec and return the first
 * one. Caller releases. */
static HRESULT find_hw_encoder_mft(const GUID *output_subtype,
                                   IMFTransform **out_mft)
{
    HRESULT hr;
    MFT_REGISTER_TYPE_INFO in_info  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, *output_subtype };
    IMFActivate **activates = NULL;
    UINT32 count = 0;
    UINT32 i;

    *out_mft = NULL;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   &in_info, &out_info,
                   &activates, &count);
    if (FAILED(hr))
        return hr;
    if (count == 0) {
        CoTaskMemFree(activates);
        return E_FAIL;
    }

    for (i = 0; i < count; i++) {
        IMFTransform *mft = NULL;
        hr = IMFActivate_ActivateObject(activates[i], &IID_IMFTransform,
                                        (void **)&mft);
        if (SUCCEEDED(hr) && mft) {
            *out_mft = mft;
            /* Detach remaining activates and release them. */
            IMFActivate_Release(activates[i]);
            i++;
            break;
        }
        IMFActivate_Release(activates[i]);
    }
    for (; i < count; i++)
        IMFActivate_Release(activates[i]);
    CoTaskMemFree(activates);

    if (!*out_mft)
        return E_FAIL;
    return S_OK;
}

/* Query MFTEnumEx without instantiating anything, to answer "does the host
 * expose a hardware MFT for (NV12 -> output_subtype)?". */
static bool have_hw_encoder_for(const GUID *output_subtype)
{
    HRESULT hr;
    MFT_REGISTER_TYPE_INFO in_info  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, *output_subtype };
    IMFActivate **activates = NULL;
    UINT32 count = 0;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                   &in_info, &out_info,
                   &activates, &count);
    if (FAILED(hr))
        return false;
    if (activates) {
        UINT32 i;
        for (i = 0; i < count; i++)
            IMFActivate_Release(activates[i]);
        CoTaskMemFree(activates);
    }
    return count > 0;
}

/* Enumerate the MFT's available types on `stream_id` until we find one whose
 * MF_MT_SUBTYPE matches `want_subtype`, clone it into a fresh IMFMediaType,
 * and return it. The caller owns the returned reference.
 *
 * Why clone instead of hand-crafting: hardware MFTs (especially Intel/AMD
 * HEVC) advertise mandatory vendor-specific attributes on their available
 * types (NominalRange, ChromaSubsampling, VideoPrimaries, TransferFunction,
 * plus undocumented driver-private attributes). If any of those are missing
 * on the type we pass to SetInputType/SetOutputType, the MFT returns
 * MF_E_INVALIDMEDIATYPE (0xc00d6d77). Cloning the MFT's own advertised type
 * and only overwriting the attributes we *must* change (frame size, frame
 * rate, interlace mode, aspect ratio, bitrate, profile) preserves every
 * driver-specific attribute verbatim. */
static HRESULT encoder_clone_available_type(IMFTransform *mft,
                                            DWORD stream_id,
                                            bool is_input,
                                            const GUID *want_subtype,
                                            IMFMediaType **out_type)
{
    HRESULT hr;
    DWORD i;
    IMFMediaType *found = NULL;
    IMFMediaType *cloned = NULL;

    *out_type = NULL;

    for (i = 0; ; i++) {
        IMFMediaType *avail = NULL;
        GUID sub;

        if (is_input)
            hr = IMFTransform_GetInputAvailableType(mft, stream_id, i, &avail);
        else
            hr = IMFTransform_GetOutputAvailableType(mft, stream_id, i, &avail);

        if (hr == MF_E_NO_MORE_TYPES || hr == E_NOTIMPL)
            break;
        if (FAILED(hr))
            return hr;

        hr = IMFMediaType_GetGUID(avail, &MF_MT_SUBTYPE, &sub);
        if (SUCCEEDED(hr) && IsEqualGUID(&sub, want_subtype)) {
            found = avail;
            break;
        }
        IMFMediaType_Release(avail);
    }

    if (!found)
        return MF_E_INVALIDMEDIATYPE;

    hr = MFCreateMediaType(&cloned);
    if (FAILED(hr)) {
        IMFMediaType_Release(found);
        return hr;
    }
    hr = IMFMediaType_CopyAllItems(found, (IMFAttributes *)cloned);
    IMFMediaType_Release(found);
    if (FAILED(hr)) {
        IMFMediaType_Release(cloned);
        return hr;
    }

    *out_type = cloned;
    return S_OK;
}

static HRESULT encoder_set_types(struct virgl_video_codec *codec)
{
    HRESULT hr;
    IMFMediaType *in_type = NULL;
    IMFMediaType *out_type = NULL;
    IMFAttributes *mft_attrs = NULL;
    const GUID in_subtype = MFVideoFormat_NV12;

    if (codec->encoder_fps_num == 0 || codec->encoder_fps_den == 0) {
        codec->encoder_fps_num = 30;
        codec->encoder_fps_den = 1;
    }

    /* Enable low-latency mode on the MFT itself before any SetOutputType
     * call. Hardware H.264/HEVC encoders tend to behave more predictably
     * (and, crucially, some will start enumerating input types only once
     * the output side is fully configured) when MF_LOW_LATENCY=1 is set.
     * Any failure here is non-fatal — the attribute is a hint. */
    if (SUCCEEDED(IMFTransform_GetAttributes(codec->encoder_mft,
                                             &mft_attrs)) && mft_attrs) {
        IMFAttributes_SetUINT32(mft_attrs, &VIRGL_MF_LOW_LATENCY, 1);
        IMFAttributes_Release(mft_attrs);
    }

    /* OUTPUT type — hand-constructed.
     *
     * Hardware MFTs for H.264/HEVC (Intel QuickSync, AMD AMF, NVENC via MF)
     * frequently do NOT enumerate any output types via GetOutputAvailableType
     * until after SetOutputType has been called at least once: their
     * available-type list is only populated once the encoder is
     * parameter-configured, which is a chicken-and-egg problem for our
     * earlier clone-based approach. The workaround used by ffmpeg's
     * libavcodec/mfenc.c and the Microsoft SDK samples is to hand-construct
     * the output type from scratch with just the mandatory framing /
     * bitrate / subtype attributes — that set is universally accepted. */
    hr = MFCreateMediaType(&out_type);
    if (FAILED(hr))
        goto done;
    IMFMediaType_SetGUID(out_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(out_type, &MF_MT_SUBTYPE, &codec->encoder_output_guid);
    IMFMediaType_SetUINT32(out_type, &MF_MT_AVG_BITRATE,
                           codec->encoder_bitrate > 0 ?
                           codec->encoder_bitrate : 8 * 1000 * 1000);
    IMFMediaType_SetUINT32(out_type, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);
    mf_set_attr_size((IMFAttributes *)out_type, &MF_MT_FRAME_SIZE,
                     codec->encoder_width, codec->encoder_height);
    mf_set_attr_ratio((IMFAttributes *)out_type, &MF_MT_FRAME_RATE,
                      codec->encoder_fps_num, codec->encoder_fps_den);
    mf_set_attr_ratio((IMFAttributes *)out_type, &MF_MT_PIXEL_ASPECT_RATIO,
                      1, 1);
    /* Pin H.264 to Main profile; HEVC gets its default (Main 8-bit 4:2:0). */
    if (IsEqualGUID(&codec->encoder_output_guid, &MFVideoFormat_H264))
        IMFMediaType_SetUINT32(out_type, &MF_MT_MPEG2_PROFILE,
                               eAVEncH264VProfile_Main);
    hr = IMFTransform_SetOutputType(codec->encoder_mft, 0, out_type, 0);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: SetOutputType failed 0x%lx\n",
                    (unsigned long)hr);
        goto done;
    }

    /* INPUT type — enumerate-and-clone.
     *
     * Unlike the output side, hardware MFT input types carry vendor-private
     * attributes (NominalRange, ChromaSubsampling, VideoPrimaries,
     * TransferFunction, and undocumented driver-private items) whose absence
     * yields MF_E_INVALIDMEDIATYPE (0xc00d6d77) from SetInputType. Cloning
     * an MFT-advertised NV12 type and overriding only frame size / rate /
     * interlace / aspect preserves those attributes intact. Input types
     * are only reliably enumerable AFTER SetOutputType has succeeded,
     * which is why this must stay in this order. */
    hr = encoder_clone_available_type(codec->encoder_mft, 0, true,
                                      &in_subtype, &in_type);
    if (FAILED(hr) || !in_type) {
        /* Some hardware MFTs don't enumerate input types even after the output
         * type is set. Fall back to a hand-constructed NV12 type including
         * colorimetry hints — Intel/NVIDIA H.264 and HEVC encoders typically
         * accept this form. */
        virgl_warn("virgl_video_win32: input enum empty, hand-constructing NV12 "
                   "type (prev hr=0x%lx)\n", (unsigned long)hr);
        if (in_type) { IMFMediaType_Release(in_type); in_type = NULL; }
        hr = MFCreateMediaType(&in_type);
        if (FAILED(hr)) goto done;
        IMFMediaType_SetGUID(in_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(in_type, &MF_MT_SUBTYPE, &in_subtype);
        /* Rec. 709 progressive SDR — safe default accepted by all major
         * hardware encoders. */
        IMFMediaType_SetUINT32(in_type, &MF_MT_VIDEO_NOMINAL_RANGE,
                               MFNominalRange_16_235);
        IMFMediaType_SetUINT32(in_type, &MF_MT_VIDEO_PRIMARIES,
                               MFVideoPrimaries_BT709);
        IMFMediaType_SetUINT32(in_type, &MF_MT_TRANSFER_FUNCTION,
                               MFVideoTransFunc_709);
        IMFMediaType_SetUINT32(in_type, &MF_MT_YUV_MATRIX,
                               MFVideoTransferMatrix_BT709);
        IMFMediaType_SetUINT32(in_type, &MF_MT_VIDEO_CHROMA_SITING,
                               MFVideoChromaSubsampling_MPEG2);
    }
    IMFMediaType_SetUINT32(in_type, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_Progressive);
    mf_set_attr_size((IMFAttributes *)in_type, &MF_MT_FRAME_SIZE,
                     codec->encoder_width, codec->encoder_height);
    mf_set_attr_ratio((IMFAttributes *)in_type, &MF_MT_FRAME_RATE,
                      codec->encoder_fps_num, codec->encoder_fps_den);
    mf_set_attr_ratio((IMFAttributes *)in_type, &MF_MT_PIXEL_ASPECT_RATIO,
                      1, 1);
    hr = IMFTransform_SetInputType(codec->encoder_mft, 0, in_type, 0);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: SetInputType failed 0x%lx\n",
                    (unsigned long)hr);
        goto done;
    }

done:
    if (in_type)  IMFMediaType_Release(in_type);
    if (out_type) IMFMediaType_Release(out_type);
    return hr;
}

/* Lazily (on first encode call) transition the MFT from "types set" to
 * "streaming" by issuing the required MFT_MESSAGE sequence. */
static HRESULT encoder_start_streaming(struct virgl_video_codec *codec)
{
    HRESULT hr;

    if (codec->encoder_stream_started)
        return S_OK;

    hr = IMFTransform_ProcessMessage(codec->encoder_mft,
                                     MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(hr))
        return hr;
    hr = IMFTransform_ProcessMessage(codec->encoder_mft,
                                     MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr))
        return hr;
    hr = IMFTransform_ProcessMessage(codec->encoder_mft,
                                     MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr))
        return hr;

    codec->encoder_stream_started = true;
    return S_OK;
}

/* Copy the NV12 pixels for `source` into an IMFMediaBuffer, then wrap in an
 * IMFSample with the requested sample time and keyframe flag.
 *
 * Preferred source is the CPU slab latched by virgl_video_buffer_cpu_writeback
 * (populated by vrend_video.c reading the guest's GL textures with
 * glGetTexImage). If that slab is absent for any reason we fall back to
 * reading the buffer's staging texture — this only produces meaningful pixels
 * when `source` was the target of a recent decode on this same buffer; for
 * the normal guest-side encode path, relying on that fallback would feed
 * garbage (typically zeroes, i.e. a green frame) to the MFT. */
static HRESULT encoder_build_sample(struct virgl_video_codec *codec,
                                    struct virgl_video_buffer *source,
                                    bool force_keyframe,
                                    IMFSample **out_sample)
{
    HRESULT hr;
    D3D11_MAPPED_SUBRESOURCE mapped;
    IMFSample *sample = NULL;
    IMFMediaBuffer *mbuf = NULL;
    BYTE *mdata = NULL;
    DWORD mcap = 0;
    BYTE *y_dst;
    BYTE *uv_dst;
    unsigned row;
    UINT64 sample_time_100ns;
    UINT64 sample_dur_100ns;
    bool mapped_staging = false;

    *out_sample = NULL;

    /* NV12 has 1.5 bytes per pixel (Y plane full res + UV half-height). */
    DWORD total = source->width * source->height * 3 / 2;
    hr = MFCreateMemoryBuffer(total, &mbuf);
    if (FAILED(hr)) goto fail;
    hr = IMFMediaBuffer_Lock(mbuf, &mdata, &mcap, NULL);
    if (FAILED(hr)) goto fail;

    if (source->encode_src_nv12 && source->encode_src_size >= total) {
        /* Fast path: vrend_video.c already handed us a densely packed NV12
         * blob. Just memcpy it straight into the MF media buffer. */
        memcpy(mdata, source->encode_src_nv12, total);
    } else {
        /* Fallback: read whatever's already in the buffer's staging texture.
         * The encode path normally receives NV12 bytes via
         * virgl_video_buffer_cpu_writeback() (above), so this branch only
         * fires when vrend_video.c failed to call that helper — in which case
         * the staging texture still holds pixels from the most recent decode
         * on this same buffer (if any) or zeroes (green frame). Previously
         * this path did a per-buffer decode_tex -> staging copy, but with the
         * DPB moved onto the codec there's no per-buffer decode texture to
         * re-stage from; the previously-copied staging content is the best
         * we can do. */
        UINT y_pitch;
        UINT uv_pitch;
        BYTE *y_src;
        BYTE *uv_src;

        hr = ID3D11DeviceContext_Map(g_vid.context,
                                     (ID3D11Resource *)source->staging_tex,
                                     0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            IMFMediaBuffer_Unlock(mbuf);
            goto fail;
        }
        mapped_staging = true;

        y_pitch  = mapped.RowPitch;
        uv_pitch = mapped.RowPitch;   /* NV12 interleaved UV uses same pitch */
        y_src    = (BYTE *)mapped.pData;
        uv_src   = y_src + (size_t)y_pitch * source->height;

        /* Pack row-by-row to get rid of the D3D RowPitch padding. */
        y_dst  = mdata;
        uv_dst = mdata + (size_t)source->width * source->height;
        for (row = 0; row < source->height; row++)
            memcpy(y_dst + (size_t)row * source->width,
                   y_src + (size_t)row * y_pitch,
                   source->width);
        for (row = 0; row < source->height / 2u; row++)
            memcpy(uv_dst + (size_t)row * source->width,
                   uv_src + (size_t)row * uv_pitch,
                   source->width);
    }

    IMFMediaBuffer_Unlock(mbuf);
    IMFMediaBuffer_SetCurrentLength(mbuf, total);

    if (mapped_staging) {
        ID3D11DeviceContext_Unmap(g_vid.context,
                                  (ID3D11Resource *)source->staging_tex, 0);
        mapped_staging = false;
    }

    hr = MFCreateSample(&sample);
    if (FAILED(hr)) goto fail;
    IMFSample_AddBuffer(sample, mbuf);

    /* Monotonic presentation time. 100ns units; frame period = 1e7 *
     * fps_den / fps_num. */
    sample_dur_100ns  = (UINT64)10000000ULL *
                       (codec->encoder_fps_den ? codec->encoder_fps_den : 1) /
                       (codec->encoder_fps_num ? codec->encoder_fps_num : 30);
    sample_time_100ns = codec->encoder_frame_count * sample_dur_100ns;
    IMFSample_SetSampleTime(sample, (LONGLONG)sample_time_100ns);
    IMFSample_SetSampleDuration(sample, (LONGLONG)sample_dur_100ns);

    /* Keyframe hint. MFSampleExtension_CleanPoint marks a sample as a
     * random-access point; most hardware MFTs interpret that as "emit an
     * IDR here". MFSampleExtension_ForceKeyFrame would be more forceful
     * but isn't declared in MSYS2 MinGW headers, so we rely on CleanPoint
     * plus implicit IDR-at-frame-0 behaviour. */
    if (force_keyframe || codec->encoder_frame_count == 0) {
        IMFSample_SetUINT32(sample, &MFSampleExtension_CleanPoint, 1);
    }

    IMFMediaBuffer_Release(mbuf);
    *out_sample = sample;
    return S_OK;

fail:
    if (mbuf)   IMFMediaBuffer_Release(mbuf);
    if (sample) IMFSample_Release(sample);
    if (mapped_staging)
        ID3D11DeviceContext_Unmap(g_vid.context,
                                  (ID3D11Resource *)source->staging_tex, 0);
    return FAILED(hr) ? hr : E_FAIL;
}

/* Append the bytes from an output IMFSample into the codec's coded buffer,
 * growing it on demand. */
static HRESULT encoder_collect_output(struct virgl_video_codec *codec,
                                      IMFSample *sample)
{
    HRESULT hr;
    DWORD total_len = 0;
    DWORD i;
    DWORD buffer_count = 0;

    IMFSample_GetBufferCount(sample, &buffer_count);
    for (i = 0; i < buffer_count; i++) {
        IMFMediaBuffer *mb = NULL;
        BYTE *data = NULL;
        DWORD cap = 0, cur = 0;

        hr = IMFSample_GetBufferByIndex(sample, i, &mb);
        if (FAILED(hr)) return hr;
        hr = IMFMediaBuffer_Lock(mb, &data, &cap, &cur);
        if (FAILED(hr)) { IMFMediaBuffer_Release(mb); return hr; }

        if (codec->encoder_coded_size + cur > codec->encoder_coded_capacity) {
            unsigned new_cap = codec->encoder_coded_capacity ?
                               codec->encoder_coded_capacity * 2 : 65536;
            while (new_cap < codec->encoder_coded_size + cur)
                new_cap *= 2;
            uint8_t *grown = realloc(codec->encoder_coded_buf, new_cap);
            if (!grown) {
                IMFMediaBuffer_Unlock(mb);
                IMFMediaBuffer_Release(mb);
                return E_OUTOFMEMORY;
            }
            codec->encoder_coded_buf      = grown;
            codec->encoder_coded_capacity = new_cap;
        }
        memcpy(codec->encoder_coded_buf + codec->encoder_coded_size, data, cur);
        codec->encoder_coded_size += cur;

        IMFMediaBuffer_Unlock(mb);
        IMFMediaBuffer_Release(mb);
        total_len += cur;
    }
    (void)total_len;
    return S_OK;
}

/* Drain ProcessOutput until it signals MF_E_TRANSFORM_NEED_MORE_INPUT. */
static HRESULT encoder_drain_output(struct virgl_video_codec *codec)
{
    HRESULT hr;
    MFT_OUTPUT_STREAM_INFO stream_info;
    IMFSample *out_sample = NULL;

    memset(&stream_info, 0, sizeof(stream_info));
    hr = IMFTransform_GetOutputStreamInfo(codec->encoder_mft, 0, &stream_info);
    if (FAILED(hr))
        return hr;

    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out_buf;
        DWORD status = 0;
        bool provided_sample = false;

        memset(&out_buf, 0, sizeof(out_buf));
        out_buf.dwStreamID = 0;

        /* If the MFT does not provide its own output samples, we have to
         * supply one. Hardware MFTs on Windows 10+ typically provide
         * samples themselves, but check the flag. */
        if (!(stream_info.dwFlags &
              (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            IMFMediaBuffer *buf = NULL;
            hr = MFCreateMemoryBuffer(stream_info.cbSize ?
                                      stream_info.cbSize : 1024 * 1024,
                                      &buf);
            if (FAILED(hr)) return hr;
            hr = MFCreateSample(&out_sample);
            if (FAILED(hr)) { IMFMediaBuffer_Release(buf); return hr; }
            IMFSample_AddBuffer(out_sample, buf);
            IMFMediaBuffer_Release(buf);
            out_buf.pSample = out_sample;
            provided_sample = true;
        }

        hr = IMFTransform_ProcessOutput(codec->encoder_mft, 0, 1,
                                        &out_buf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (provided_sample && out_sample)
                IMFSample_Release(out_sample);
            if (out_buf.pEvents)
                IUnknown_Release((IUnknown *)out_buf.pEvents);
            return S_OK;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            /* Renegotiate output type silently; re-set and retry. */
            if (provided_sample && out_sample)
                IMFSample_Release(out_sample);
            if (out_buf.pEvents)
                IUnknown_Release((IUnknown *)out_buf.pEvents);
            encoder_set_types(codec);
            continue;
        }
        if (FAILED(hr)) {
            if (provided_sample && out_sample)
                IMFSample_Release(out_sample);
            if (out_buf.pEvents)
                IUnknown_Release((IUnknown *)out_buf.pEvents);
            return hr;
        }

        if (out_buf.pSample) {
            encoder_collect_output(codec, out_buf.pSample);
            IMFSample_Release(out_buf.pSample);
        }
        if (out_buf.pEvents)
            IUnknown_Release((IUnknown *)out_buf.pEvents);
        out_sample = NULL;
    }
}

static int encoder_create_mft(struct virgl_video_codec *codec,
                              const struct virgl_video_create_codec_args *args)
{
    HRESULT hr;

    if (!profile_encode_output_guid(args->profile, &codec->encoder_output_guid)) {
        virgl_error("virgl_video_win32: profile %d has no MF encoder guid\n",
                    (int)args->profile);
        return -1;
    }

    hr = virgl_video_mf_acquire();
    if (FAILED(hr))
        return -1;

    hr = find_hw_encoder_mft(&codec->encoder_output_guid, &codec->encoder_mft);
    if (FAILED(hr) || !codec->encoder_mft) {
        virgl_error("virgl_video_win32: no hardware MFT for profile %d "
                    "(hr=0x%lx)\n", (int)args->profile, (unsigned long)hr);
        virgl_video_mf_release();
        return -1;
    }

    codec->encoder_width  = args->width;
    codec->encoder_height = args->height;
    codec->encoder_fps_num = 30;
    codec->encoder_fps_den = 1;
    codec->encoder_bitrate = 8 * 1000 * 1000;   /* 8 Mbps placeholder */

    hr = encoder_set_types(codec);
    if (FAILED(hr)) {
        IMFTransform_Release(codec->encoder_mft);
        codec->encoder_mft = NULL;
        virgl_video_mf_release();
        return -1;
    }

    virgl_info("virgl_video_win32: encoder MFT ready for profile %d "
               "(%ux%u)\n", (int)args->profile, args->width, args->height);
    return 0;
}

static void encoder_destroy_mft(struct virgl_video_codec *codec)
{
    if (codec->encoder_mft) {
        /* Drain anything pending (best-effort). COMMAND_DRAIN tells the MFT
         * "no more input"; we then must pull ProcessOutput until it returns
         * MF_E_TRANSFORM_NEED_MORE_INPUT before NOTIFY_END_STREAMING — ffmpeg
         * mfenc.c follows the same sequence. */
        if (codec->encoder_stream_started) {
            IMFTransform_ProcessMessage(codec->encoder_mft,
                                        MFT_MESSAGE_COMMAND_DRAIN, 0);
            encoder_drain_output(codec);
            IMFTransform_ProcessMessage(codec->encoder_mft,
                                        MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            IMFTransform_ProcessMessage(codec->encoder_mft,
                                        MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        IMFTransform_Release(codec->encoder_mft);
        codec->encoder_mft = NULL;
        virgl_video_mf_release();
    }
    if (codec->encoder_coded_buf) {
        free(codec->encoder_coded_buf);
        codec->encoder_coded_buf = NULL;
    }
    codec->encoder_coded_size = 0;
    codec->encoder_coded_capacity = 0;
    codec->encoder_stream_started = false;
}

/* Extract bitrate / keyframe hints from the virgl encoder picture desc.
 * h264_enc_picture_desc and h265_enc_picture_desc have parallel-ish layouts:
 * both carry a rate_ctrl struct and a picture_type flag. */
static void encoder_extract_hints(struct virgl_video_codec *codec,
                                  const union virgl_picture_desc *desc,
                                  uint32_t *out_bitrate,
                                  uint32_t *out_fps_num,
                                  uint32_t *out_fps_den,
                                  bool *out_force_keyframe)
{
    uint32_t bitrate = codec->encoder_bitrate;
    uint32_t num = codec->encoder_fps_num;
    uint32_t den = codec->encoder_fps_den;
    bool kf = false;

    switch (codec->profile) {
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH: {
        const struct virgl_h264_enc_picture_desc *d = &desc->h264_enc;
        if (d->rate_ctrl[0].target_bitrate)
            bitrate = d->rate_ctrl[0].target_bitrate;
        if (d->rate_ctrl[0].frame_rate_num && d->rate_ctrl[0].frame_rate_den) {
            num = d->rate_ctrl[0].frame_rate_num;
            den = d->rate_ctrl[0].frame_rate_den;
        }
        /* pipe_h2645_enc_picture_type: 0=P,1=B,2=I,3=IDR,4=SKIP (per Mesa) */
        if (d->picture_type == 2 /* I */ || d->picture_type == 3 /* IDR */)
            kf = true;
        break;
    }
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10: {
        const struct virgl_h265_enc_picture_desc *d = &desc->h265_enc;
        if (d->rc.target_bitrate)
            bitrate = d->rc.target_bitrate;
        if (d->rc.frame_rate_num && d->rc.frame_rate_den) {
            num = d->rc.frame_rate_num;
            den = d->rc.frame_rate_den;
        }
        if (d->picture_type == 2 || d->picture_type == 3)
            kf = true;
        break;
    }
    default:
        break;
    }

    *out_bitrate = bitrate;
    *out_fps_num = num;
    *out_fps_den = den;
    *out_force_keyframe = kf;
}

static int encode_one_frame(struct virgl_video_codec *codec,
                            struct virgl_video_buffer *source,
                            const union virgl_picture_desc *desc)
{
    HRESULT hr;
    IMFSample *in_sample = NULL;
    uint32_t bitrate, fps_num, fps_den;
    bool kf = false;

    if (!codec->encoder_mft)
        return -ENOSYS;

    encoder_extract_hints(codec, desc, &bitrate, &fps_num, &fps_den, &kf);

    /* Re-negotiate output type if the rate/fps changed appreciably. The MFT
     * will reject SetOutputType once streaming has begun on most drivers; we
     * only update if still pre-stream, and cache for future runs otherwise. */
    if (!codec->encoder_stream_started) {
        bool changed = false;
        if (bitrate != codec->encoder_bitrate) {
            codec->encoder_bitrate = bitrate;
            changed = true;
        }
        if (fps_num != codec->encoder_fps_num ||
            fps_den != codec->encoder_fps_den) {
            codec->encoder_fps_num = fps_num;
            codec->encoder_fps_den = fps_den;
            changed = true;
        }
        if (changed)
            encoder_set_types(codec);
        hr = encoder_start_streaming(codec);
        if (FAILED(hr)) {
            virgl_error("virgl_video_win32: encoder_start_streaming failed "
                        "0x%lx\n", (unsigned long)hr);
            return -1;
        }
    }

    /*
     * Ask vrend_video.c to copy the guest-side source NV12 bytes into our
     * per-buffer CPU slab via virgl_video_buffer_cpu_writeback(). Without
     * this, encoder_build_sample() would fall back to reading the buffer's
     * staging texture (which is never written on the encode-only path) and
     * the MFT would see a solid green/zeroed frame every time.
     *
     * The callback shape matches the libva side (virgl_video_dma_buf), but
     * on Windows the fd fields are unused — vrend_video.c just needs the
     * buffer pointer to find its GL-side planes.
     */
    if (g_vid.callbacks && g_vid.callbacks->encode_upload_picture) {
        struct virgl_video_dma_buf upload_dmabuf;
        memset(&upload_dmabuf, 0, sizeof(upload_dmabuf));
        upload_dmabuf.buf = source;
        upload_dmabuf.drm_format = drm_fourcc_nv12();
        upload_dmabuf.width = source->width;
        upload_dmabuf.height = source->height;
        upload_dmabuf.flags = VIRGL_VIDEO_DMABUF_WRITE_ONLY;
        upload_dmabuf.num_planes = 0;
        g_vid.callbacks->encode_upload_picture(codec, &upload_dmabuf);
    }

    hr = encoder_build_sample(codec, source, kf, &in_sample);
    if (FAILED(hr) || !in_sample) {
        virgl_error("virgl_video_win32: encoder_build_sample failed 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }

    /* Drop any residue from a prior call. */
    codec->encoder_coded_size = 0;

    hr = IMFTransform_ProcessInput(codec->encoder_mft, 0, in_sample, 0);
    IMFSample_Release(in_sample);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: ProcessInput failed 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }
    codec->encoder_frame_count++;

    hr = encoder_drain_output(codec);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: encoder_drain_output failed 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }

    /* Hand the coded bytes back to vrend_video.c. The callback signature
     * wants num_coded_bufs + coded_bufs + coded_sizes arrays; we publish a
     * single buffer (the concatenated AU/NALU stream from this frame). */
    if (g_vid.callbacks && g_vid.callbacks->encode_completed &&
        codec->encoder_coded_size > 0) {
        struct virgl_video_dma_buf dmabuf;
        const void *bufs[1]    = { codec->encoder_coded_buf };
        const unsigned sizes[1] = { codec->encoder_coded_size };

        memset(&dmabuf, 0, sizeof(dmabuf));
        dmabuf.buf = source;
        dmabuf.drm_format = drm_fourcc_nv12();
        dmabuf.width = source->width;
        dmabuf.height = source->height;
        dmabuf.flags = VIRGL_VIDEO_DMABUF_READ_ONLY;
        dmabuf.num_planes = 0;   /* src buffer is opaque to the guest here */

        g_vid.callbacks->encode_completed(codec, &dmabuf, NULL,
                                          1, bufs, sizes);
    }

    return 0;
}

int virgl_video_encode_bitstream(struct virgl_video_codec *codec,
                                 struct virgl_video_buffer *source,
                                 const union virgl_picture_desc *desc)
{
    if (!g_vid.initialized || !codec || !source || !desc)
        return -EINVAL;
    if (codec->entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE)
        return -ENOSYS;

    return encode_one_frame(codec, source, desc);
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
    int slot;

    if (!g_vid.initialized || !codec || !target || !codec->decoder)
        return -1;
    if (!codec->decode_tex_array) {
        virgl_error("virgl_video_win32: end_frame without DPB array\n");
        return -1;
    }

    hr = ID3D11VideoContext_DecoderEndFrame(g_vid.video_context,
                                            codec->decoder);
    if (FAILED(hr)) {
        virgl_error("virgl_video_win32: DecoderEndFrame failed: 0x%lx\n",
                    (unsigned long)hr);
        return -1;
    }

    unmap_staging_if_needed(target);

    /* Copy the just-decoded slice of the DPB array into the per-buffer
     * staging texture for CPU readback. Staging remains per-buffer so
     * multiple outputs can be mapped concurrently by the guest. */
    slot = target->current_slot_in_codec;
    if (slot < 0 || slot >= VIRGL_VIDEO_WIN32_DPB_SIZE) {
        virgl_error("virgl_video_win32: end_frame with no DPB slot (slot=%d)\n",
                    slot);
        return -1;
    }
    ID3D11DeviceContext_CopySubresourceRegion(
            g_vid.context,
            (ID3D11Resource *)target->staging_tex, 0, /* DstSubresource */
            0, 0, 0,
            (ID3D11Resource *)codec->decode_tex_array,
            (UINT)slot,                               /* SrcSubresource */
            NULL);

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

/*
 * ---------------------------------------------------------------------------
 * virgl_video_buffer_cpu_writeback.
 *
 * Mirror of the readback helper, used by vrend_video.c on the encode path.
 * The vrend layer reads the guest's GL textures back to CPU with
 * glGetTexImage, rearranges the planes into NV12 (Y followed by interleaved
 * UV), and hands us the packed blob here. We stash it on the buffer so
 * encoder_build_sample() can copy it straight into the MF media buffer on
 * the next ProcessInput.
 * ---------------------------------------------------------------------------
 */

int virgl_video_buffer_cpu_writeback(struct virgl_video_buffer *buf,
                                     const void *nv12_data,
                                     uint32_t nv12_size)
{
    if (!buf || !nv12_data || nv12_size == 0)
        return -1;

    if (nv12_size > buf->encode_src_capacity) {
        uint8_t *grown = realloc(buf->encode_src_nv12, nv12_size);
        if (!grown)
            return -1;
        buf->encode_src_nv12     = grown;
        buf->encode_src_capacity = nv12_size;
    }

    memcpy(buf->encode_src_nv12, nv12_data, nv12_size);
    buf->encode_src_size = nv12_size;
    return 0;
}
