/**************************************************************************
 *
 * Copyright (C) 2022 Kylin Software Co., Ltd.
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
 * The video implementation of the vrend renderer.
 *
 * It is based on the general virgl video submodule and handles data transfer
 * and synchronization between host and guest.
 *
 * The relationship between vaSurface and video buffer objects:
 *
 *           GUEST (Mesa)           |       HOST (Virglrenderer)
 *                                  |
 *         +------------+           |          +------------+
 *         | vaSurface  |           |          | vaSurface  | <------+
 *         +------------+           |          +------------+        |
 *               |                  |                                |
 *  +---------------------------+   |   +-------------------------+  |
 *  |    virgl_video_buffer     |   |   |    vrend_video_buffer   |  |
 *  | +-----------------------+ |   |   |  +-------------------+  |  |
 *  | |    vl_video_buffer    | |   |   |  | vrend_resource(s) |  |  |
 *  | | +-------------------+ | |<--+-->|  +-------------------+  |  |
 *  | | | virgl_resource(s) | | |   |   |  +--------------------+ |  |
 *  | | +-------------------+ | |   |   |  | virgl_video_buffer |-+--+
 *  | +-----------------------+ |   |   |  +--------------------+ |
 *  +---------------------------+   |   +-------------------------+
 *
 * The relationship between vaContext and video codec objects:
 *
 *           GUEST (Mesa)         |         HOST (Virglrenderer)
 *                                |
 *         +------------+         |           +------------+
 *         | vaContext  |         |           | vaContext  | <-------+
 *         +------------+         |           +------------+         |
 *               |                |                                  |
 *  +------------------------+    |    +--------------------------+  |
 *  |    virgl_video_codec   | <--+--> |    vrend_video_codec     |  |
 *  +------------------------+    |    |  +--------------------+  |  |
 *                                |    |  | virgl_video_codec  | -+--+
 *                                |    |  +--------------------+  |
 *                                |    +--------------------------+
 *
 * @author Feng Jiang <jiangfeng@kylinos.cn>
 */


#include "config.h"

#include "virgl_video.h"
#include "virgl_video_hw.h"

#include "vrend_debug.h"
#include "vrend_winsys.h"
#include "vrend_renderer.h"
#include "vrend_video.h"

/*
 * On Windows we don't have dma-buf / EGL_LINUX_DMA_BUF — the D3D11 video
 * backend hands us CPU NV12 planes via virgl_video_buffer_cpu_readback().
 * The EGLImage path would drag in eglCreateImageKHR / GL_OES_EGL_image which
 * are unavailable against Mesa-on-Windows today, so we compile out the
 * import/export helpers entirely on Win32 and use glTexSubImage2D to upload
 * the CPU pixel data into the guest-visible resource textures instead.
 */
#if defined(ENABLE_VIDEO_WIN32) || defined(_WIN32)
#  define VREND_VIDEO_WIN32_CPU_UPLOAD 1
#else
#  define VREND_VIDEO_WIN32_CPU_UPLOAD 0
#endif

struct vrend_context;

struct vrend_video_context {
    struct vrend_context *ctx;
    struct list_head codecs;
    struct list_head buffers;
};

struct vrend_video_codec {
    struct virgl_video_codec *codec;
    uint32_t handle;
    struct vrend_resource *feed_res;    /* encoding feedback */
    struct vrend_resource *dest_res;    /* encoding coded buffer */
    struct vrend_video_context *ctx;
    struct list_head head;
};

struct vrend_video_plane {
    uint32_t res_handle;
    GLuint texture;         /* texture for temporary use */
    GLuint framebuffer;     /* framebuffer for temporary use */
#if !VREND_VIDEO_WIN32_CPU_UPLOAD
    EGLImageKHR egl_image;  /* egl image for temporary use */
#endif
};

struct vrend_video_buffer {
    struct virgl_video_buffer *buffer;

    uint32_t handle;
    struct vrend_video_context *ctx;
    struct list_head head;

    uint32_t num_planes;
    struct vrend_video_plane planes[3];
};

static struct vrend_video_codec *vrend_video_codec(
        struct virgl_video_codec *codec)
{
    return virgl_video_codec_opaque_data(codec);
}

static struct vrend_video_buffer *vrend_video_buffer(
        struct virgl_video_buffer *buffer)
{
    return virgl_video_buffer_opaque_data(buffer);
}

static struct vrend_video_codec *get_video_codec(
                                        struct vrend_video_context *ctx,
                                        uint32_t cdc_handle)
{
    list_for_each_entry(struct vrend_video_codec, cdc, &ctx->codecs, head) {
        if (cdc->handle == cdc_handle)
            return cdc;
    }

    return NULL;
}

static struct vrend_video_buffer *get_video_buffer(
                                        struct vrend_video_context *ctx,
                                        uint32_t buf_handle)
{
    list_for_each_entry(struct vrend_video_buffer, buf, &ctx->buffers, head) {
        if (buf->handle == buf_handle)
            return buf;
    }

    return NULL;
}


#if !VREND_VIDEO_WIN32_CPU_UPLOAD
static int sync_dmabuf_to_video_buffer(struct vrend_video_buffer *buf,
                                       const struct virgl_video_dma_buf *dmabuf)
{
    if (!(dmabuf->flags & VIRGL_VIDEO_DMABUF_READ_ONLY)) {
        virgl_error("%s: dmabuf is not readable\n", __func__);
        return -1;
    }

    for (unsigned i = 0; i < dmabuf->num_planes && i < buf->num_planes; i++) {
        struct vrend_video_plane *plane = &buf->planes[i];
        struct vrend_resource *res;

        res = vrend_renderer_ctx_res_lookup(buf->ctx->ctx, plane->res_handle);
        if (!res) {
            virgl_error("%s: res %d not found\n", __func__, plane->res_handle);
            continue;
        }

        /* dmabuf -> eglimage */
        if (EGL_NO_IMAGE_KHR == plane->egl_image) {
            EGLint img_attrs[16] = {
                EGL_LINUX_DRM_FOURCC_EXT,       dmabuf->planes[i].drm_format,
                EGL_WIDTH,                      dmabuf->width / (i + 1),
                EGL_HEIGHT,                     dmabuf->height / (i + 1),
                EGL_DMA_BUF_PLANE0_FD_EXT,      dmabuf->planes[i].fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT,  dmabuf->planes[i].offset,
                EGL_DMA_BUF_PLANE0_PITCH_EXT,   dmabuf->planes[i].pitch,
                EGL_NONE
            };

            plane->egl_image = eglCreateImageKHR(eglGetCurrentDisplay(),
                    EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
        }

        if (EGL_NO_IMAGE_KHR == plane->egl_image) {
            virgl_error("%s: create egl image failed\n", __func__);
            continue;
        }

        /* eglimage -> texture */
        glBindTexture(GL_TEXTURE_2D, plane->texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                                    (GLeglImageOES)(plane->egl_image));

        /* texture -> framebuffer */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, plane->framebuffer);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, plane->texture, 0);

        /* framebuffer -> vrend_video_buffer.planes[i] */
        glBindTexture(GL_TEXTURE_2D, res->gl_id);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                            res->base.width0, res->base.height0);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return 0;
}

static int sync_video_buffer_to_dmabuf(struct vrend_video_buffer *buf,
                                       const struct virgl_video_dma_buf *dmabuf)
{
    if (!(dmabuf->flags & VIRGL_VIDEO_DMABUF_WRITE_ONLY)) {
        virgl_error("%s: dmabuf is not writable\n", __func__);
        return -1;
    }

    for (unsigned i = 0; i < dmabuf->num_planes && i < buf->num_planes; i++) {
        struct vrend_video_plane *plane = &buf->planes[i];
        struct vrend_resource *res;

        res = vrend_renderer_ctx_res_lookup(buf->ctx->ctx, plane->res_handle);
        if (!res) {
            virgl_error("%s: res %d not found\n", __func__, plane->res_handle);
            continue;
        }

        /* dmabuf -> eglimage */
        if (EGL_NO_IMAGE_KHR == plane->egl_image) {
            EGLint img_attrs[16] = {
                EGL_LINUX_DRM_FOURCC_EXT,       dmabuf->planes[i].drm_format,
                EGL_WIDTH,                      dmabuf->width / (i + 1),
                EGL_HEIGHT,                     dmabuf->height / (i + 1),
                EGL_DMA_BUF_PLANE0_FD_EXT,      dmabuf->planes[i].fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT,  dmabuf->planes[i].offset,
                EGL_DMA_BUF_PLANE0_PITCH_EXT,   dmabuf->planes[i].pitch,
                EGL_NONE
            };

            plane->egl_image = eglCreateImageKHR(eglGetCurrentDisplay(),
                    EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
        }

        if (EGL_NO_IMAGE_KHR == plane->egl_image) {
            virgl_error("%s: create egl image failed\n", __func__);
            continue;
        }

        /* eglimage -> texture */
        glBindTexture(GL_TEXTURE_2D, plane->texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                                    (GLeglImageOES)(plane->egl_image));

        /* vrend_video_buffer.planes[i] -> framebuffer */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, plane->framebuffer);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, res->gl_id, 0);

        /* framebuffer -> texture */
        glBindTexture(GL_TEXTURE_2D, plane->texture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                            res->base.width0, res->base.height0);

    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return 0;
}
#endif /* !VREND_VIDEO_WIN32_CPU_UPLOAD */

#if VREND_VIDEO_WIN32_CPU_UPLOAD
/*
 * Windows CPU path. The D3D11 backend has already copied the NV12 frame into
 * a CPU-mapped staging texture by the time decode_completed fires; we pull
 * plane pointers out via virgl_video_buffer_cpu_readback() and push them into
 * the guest-visible resource textures using glTexSubImage2D.
 *
 * NV12 plane 0 is a single-channel 8-bit Y plane at full resolution. Plane 1
 * is two interleaved 8-bit channels (U,V) at half resolution in each axis.
 * On the guest side Mesa allocates the resource textures with the matching
 * internal formats (R8 and RG8), so we can just upload with GL_RED /
 * GL_RG and GL_UNSIGNED_BYTE.
 */
static int sync_cpu_planes_to_video_buffer(struct vrend_video_buffer *buf,
                                           const struct virgl_video_dma_buf *dmabuf)
{
    void *planes[4] = { NULL };
    uint32_t pitches[4] = { 0 };
    unsigned n, i;

    n = virgl_video_buffer_cpu_readback(buf->buffer, planes, pitches);
    if (n == 0) {
        virgl_error("%s: backend returned no CPU planes\n", __func__);
        return -1;
    }

    /*
     * The D3D11 backend always delivers NV12 (2 planes: Y + interleaved UV).
     * The guest may have allocated the surface as NV12 (2 GL planes: R8 Y +
     * RG8 UV) or as I420/YV12 (3 GL planes: R8 Y + R8 U + R8 V). Split the
     * UV plane if the guest expects three planes. Assumes IYUV/I420 order
     * (Y, U, V) which is the common default in Mesa's VA-API frontend.
     */
    uint8_t *u_scratch = NULL, *v_scratch = NULL;
    bool need_split = (buf->num_planes == 3 && n == 2);
    if (need_split) {
        unsigned uv_w = dmabuf->width / 2;
        unsigned uv_h = dmabuf->height / 2;
        u_scratch = calloc((size_t)uv_w * uv_h, 1);
        v_scratch = calloc((size_t)uv_w * uv_h, 1);
        if (!u_scratch || !v_scratch) {
            free(u_scratch); free(v_scratch);
            virgl_error("%s: UV split scratch alloc failed\n", __func__);
            return -1;
        }
        const uint8_t *uv_src = (const uint8_t *)planes[1];
        uint32_t src_pitch = pitches[1];
        for (unsigned y = 0; y < uv_h; y++) {
            const uint8_t *row = uv_src + (size_t)y * src_pitch;
            for (unsigned x = 0; x < uv_w; x++) {
                u_scratch[y * uv_w + x] = row[2 * x];
                v_scratch[y * uv_w + x] = row[2 * x + 1];
            }
        }
    }

    /* Drain any prior GL errors so our post-upload check is meaningful. */
    while (glGetError() != GL_NO_ERROR) { }

    /* One-shot diagnostic: dump dimensions on the first decode so we can see
     * what the guest's actual plane layout looks like. */
    static bool logged_once = false;
    if (!logged_once) {
        virgl_warn("vid-diag: num_planes=%u dmabuf %ux%u src_pitches=%u,%u "
                   "need_split=%d\n",
                   buf->num_planes, dmabuf->width, dmabuf->height,
                   pitches[0], pitches[1], need_split ? 1 : 0);
        for (unsigned p = 0; p < buf->num_planes; p++) {
            struct vrend_resource *r =
                vrend_renderer_ctx_res_lookup(buf->ctx->ctx,
                                              buf->planes[p].res_handle);
            if (r) {
                virgl_warn("vid-diag:   plane %u res=%u w0=%u h0=%u "
                           "target=0x%x gl_id=%u\n",
                           p, buf->planes[p].res_handle,
                           r->base.width0, r->base.height0,
                           r->target, r->gl_id);
            }
        }
        logged_once = true;
    }

    for (i = 0; i < buf->num_planes; i++) {
        struct vrend_video_plane *plane = &buf->planes[i];
        struct vrend_resource *res;
        GLenum ext_format, gl_target;
        GLsizei width, height;
        const void *src;
        GLint row_stride_elems;     /* UNPACK_ROW_LENGTH value */
        GLenum err;

        res = vrend_renderer_ctx_res_lookup(buf->ctx->ctx, plane->res_handle);
        if (!res) {
            virgl_error("%s: res %d not found\n", __func__, plane->res_handle);
            continue;
        }

        gl_target = res->target ? res->target : GL_TEXTURE_2D;
        width  = (GLsizei)res->base.width0;
        height = (GLsizei)res->base.height0;

        if (i == 0) {
            /* Y plane: R8 full-res, source is NV12's Y plane. */
            ext_format = GL_RED;
            src = planes[0];
            row_stride_elems = (GLint)pitches[0];   /* 1 byte/element */
        } else if (!need_split) {
            /* 2-plane NV12 guest buffer: source is the NV12 UV plane. */
            ext_format = GL_RG;
            src = planes[1];
            row_stride_elems = (GLint)(pitches[1] / 2);  /* 2 bytes/element */
        } else if (i == 1) {
            /* 3-plane I420 guest buffer: U plane from our split. */
            ext_format = GL_RED;
            src = u_scratch;
            row_stride_elems = (GLint)(dmabuf->width / 2);
        } else {
            /* 3-plane I420 guest buffer: V plane from our split. */
            ext_format = GL_RED;
            src = v_scratch;
            row_stride_elems = (GLint)(dmabuf->width / 2);
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_stride_elems);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        glBindTexture(gl_target, res->gl_id);
        glTexSubImage2D(gl_target, 0, 0, 0, width, height,
                        ext_format, GL_UNSIGNED_BYTE, src);

        err = glGetError();
        if (err != GL_NO_ERROR) {
            virgl_warn("%s: plane %u glTexSubImage2D error 0x%x "
                       "(target=0x%x w=%d h=%d fmt=0x%x stride=%d)\n",
                       __func__, i, err, gl_target, width, height,
                       ext_format, row_stride_elems);
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    free(u_scratch);
    free(v_scratch);
    return 0;
}
#endif /* VREND_VIDEO_WIN32_CPU_UPLOAD */


static void vrend_video_decode_completed(
                                struct virgl_video_codec *codec,
                                const struct virgl_video_dma_buf *dmabuf)
{
    struct vrend_video_buffer *buf = vrend_video_buffer(dmabuf->buf);

    (void)codec;

#if VREND_VIDEO_WIN32_CPU_UPLOAD
    sync_cpu_planes_to_video_buffer(buf, dmabuf);
#else
    sync_dmabuf_to_video_buffer(buf, dmabuf);
#endif
}


#if VREND_VIDEO_WIN32_CPU_UPLOAD
/*
 * Mirror of sync_cpu_planes_to_video_buffer() for the encode direction. The
 * Windows backend cannot reach the guest's GL textures directly (no DMA-BUF,
 * no WGL_NV_DX_interop2 yet), so we pull each plane back to CPU with
 * glGetTexImage, interleave the chroma if the guest laid it out as I420, and
 * hand the packed NV12 blob off to the backend via
 * virgl_video_buffer_cpu_writeback(). The backend then has everything
 * encoder_build_sample() needs on its next ProcessInput.
 *
 * Assumes 4:2:0 chroma subsampling (NV12 or I420/YV12). 4:2:2 / 4:4:4
 * encoder surfaces are not expected here — Mesa's gallium video frontend
 * currently hard-codes NV12 / I420 for virgl encode paths, and the MFT input
 * type is already set to MFVideoFormat_NV12, so any other layout would
 * mis-encode regardless. If the guest ever sends YV12 (V before U), the
 * interleave loop below would swap chroma channels; detecting that would
 * require inspecting the pipe_format on the guest-side resource, which isn't
 * plumbed through to this layer.
 */
static int sync_video_buffer_to_cpu_slab(struct vrend_video_buffer *buf)
{
    struct vrend_resource *y_res;
    unsigned y_w, y_h;
    size_t y_size, uv_size, nv12_size;
    uint8_t *nv12 = NULL;
    uint8_t *y_scratch = NULL;
    uint8_t *uv_scratch = NULL;
    uint8_t *u_scratch = NULL;
    uint8_t *v_scratch = NULL;
    int ret = -1;

    if (buf->num_planes == 0)
        return -1;

    /* Size the encode source off the Y-plane resource so we pick up the
     * guest's true coded dimensions even if the backend rounded them. */
    y_res = vrend_renderer_ctx_res_lookup(buf->ctx->ctx,
                                          buf->planes[0].res_handle);
    if (!y_res) {
        virgl_error("%s: Y plane res %d not found\n",
                    __func__, buf->planes[0].res_handle);
        return -1;
    }
    y_w = y_res->base.width0;
    y_h = y_res->base.height0;
    if (!y_w || !y_h)
        return -1;

    y_size   = (size_t)y_w * y_h;
    uv_size  = y_size / 2;            /* 4:2:0 NV12: UV is half-height, 2BPP */
    nv12_size = y_size + uv_size;

    nv12 = malloc(nv12_size);
    if (!nv12)
        return -1;

    /* Drain any pre-existing GL errors so our post-read checks are valid. */
    while (glGetError() != GL_NO_ERROR) { }

    /* Plane 0: Y. Read directly into nv12[0..y_size). */
    {
        struct vrend_video_plane *plane = &buf->planes[0];
        GLenum gl_target = y_res->target ? y_res->target : GL_TEXTURE_2D;
        GLenum err;

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glBindTexture(gl_target, y_res->gl_id);
        glGetTexImage(gl_target, 0, GL_RED, GL_UNSIGNED_BYTE, nv12);
        err = glGetError();
        if (err != GL_NO_ERROR) {
            virgl_warn("%s: Y glGetTexImage error 0x%x (target=0x%x w=%u h=%u "
                       "gl_id=%u)\n",
                       __func__, err, gl_target, y_w, y_h, y_res->gl_id);
            /* Keep going — partially read Y is still better than the
             * decode_tex fallback producing solid green. */
        }
        (void)plane;
    }

    if (buf->num_planes >= 3) {
        /* I420/YV12 guest layout: planes 1 and 2 are single-channel U and V,
         * each half-resolution. Read into scratch, then interleave. */
        struct vrend_resource *u_res = vrend_renderer_ctx_res_lookup(
                buf->ctx->ctx, buf->planes[1].res_handle);
        struct vrend_resource *v_res = vrend_renderer_ctx_res_lookup(
                buf->ctx->ctx, buf->planes[2].res_handle);
        unsigned uv_w = y_w / 2;
        unsigned uv_h = y_h / 2;
        size_t plane_size = (size_t)uv_w * uv_h;

        u_scratch = malloc(plane_size);
        v_scratch = malloc(plane_size);
        if (!u_scratch || !v_scratch || !u_res || !v_res) {
            virgl_error("%s: I420 chroma plane lookup/alloc failed\n",
                        __func__);
            goto out;
        }

        GLenum u_target = u_res->target ? u_res->target : GL_TEXTURE_2D;
        GLenum v_target = v_res->target ? v_res->target : GL_TEXTURE_2D;

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glBindTexture(u_target, u_res->gl_id);
        glGetTexImage(u_target, 0, GL_RED, GL_UNSIGNED_BYTE, u_scratch);
        glBindTexture(v_target, v_res->gl_id);
        glGetTexImage(v_target, 0, GL_RED, GL_UNSIGNED_BYTE, v_scratch);

        /* Interleave U and V byte-by-byte into the back of the NV12 blob. */
        {
            uint8_t *uv_dst = nv12 + y_size;
            size_t i;
            for (i = 0; i < plane_size; i++) {
                uv_dst[2 * i + 0] = u_scratch[i];
                uv_dst[2 * i + 1] = v_scratch[i];
            }
        }
    } else if (buf->num_planes == 2) {
        /* NV12 guest layout: plane 1 is already interleaved UV at half-res
         * with 2 bytes per pixel. GL_RG / R8G8 reads straight into the UV
         * section of the NV12 blob. */
        struct vrend_resource *uv_res = vrend_renderer_ctx_res_lookup(
                buf->ctx->ctx, buf->planes[1].res_handle);
        if (!uv_res) {
            virgl_error("%s: UV plane res %d not found\n",
                        __func__, buf->planes[1].res_handle);
            goto out;
        }
        GLenum uv_target = uv_res->target ? uv_res->target : GL_TEXTURE_2D;

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glBindTexture(uv_target, uv_res->gl_id);
        glGetTexImage(uv_target, 0, GL_RG, GL_UNSIGNED_BYTE,
                      nv12 + y_size);
    } else {
        /* Single-plane guest buffer — unexpected for NV12/I420 encode sources.
         * Leave the UV section of nv12 zeroed (black chroma) so at least the
         * luma makes it through. */
        memset(nv12 + y_size, 0x80, uv_size);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    if (virgl_video_buffer_cpu_writeback(buf->buffer, nv12,
                                         (uint32_t)nv12_size) != 0) {
        virgl_error("%s: backend writeback failed\n", __func__);
        goto out;
    }

    ret = 0;

out:
    free(y_scratch);
    free(uv_scratch);
    free(u_scratch);
    free(v_scratch);
    free(nv12);
    return ret;
}
#endif /* VREND_VIDEO_WIN32_CPU_UPLOAD */

static void vrend_video_enocde_upload_picture(
                                struct virgl_video_codec *codec,
                                const struct virgl_video_dma_buf *dmabuf)
{
    struct vrend_video_buffer *buf = vrend_video_buffer(dmabuf->buf);

    (void)codec;

#if VREND_VIDEO_WIN32_CPU_UPLOAD
    (void)dmabuf;
    sync_video_buffer_to_cpu_slab(buf);
#else
    sync_video_buffer_to_dmabuf(buf, dmabuf);
#endif
}

static void vrend_video_encode_completed(
                                struct virgl_video_codec *codec,
                                const struct virgl_video_dma_buf *src_buf,
                                const struct virgl_video_dma_buf *ref_buf,
                                unsigned num_coded_bufs,
                                const void * const *coded_bufs,
                                const unsigned *coded_sizes)
{
    void *buf;
    unsigned i, size, data_size;
    struct virgl_video_encode_feedback feedback;
    struct vrend_video_codec *cdc = vrend_video_codec(codec);

    (void)src_buf;
    (void)ref_buf;

    if (!cdc->dest_res || !cdc->feed_res)
        return;

    memset(&feedback, 0, sizeof(feedback));

    /* sync coded data to guest */
    if (has_bit(cdc->dest_res->storage_bits, VREND_STORAGE_GL_BUFFER)) {
        glBindBufferARB(cdc->dest_res->target, cdc->dest_res->gl_id);
        buf = glMapBufferRange(cdc->dest_res->target, 0,
                               cdc->dest_res->base.width0, GL_MAP_WRITE_BIT);
        for (i = 0, data_size = 0; i < num_coded_bufs &&
                    data_size < cdc->dest_res->base.width0; i++) {
            size = MIN2(cdc->dest_res->base.width0 - data_size, coded_sizes[i]);
            memcpy((uint8_t *)buf + data_size, coded_bufs[i], size);
            vrend_write_to_iovec(cdc->dest_res->iov, cdc->dest_res->num_iovs,
                                 data_size, coded_bufs[i], size);
            data_size += size;
        }
        glUnmapBuffer(cdc->dest_res->target);
        glBindBufferARB(cdc->dest_res->target, 0);
        feedback.stat = VIRGL_VIDEO_ENCODE_STAT_SUCCESS;
        feedback.coded_size = data_size;
    } else {
        virgl_warn("unexcepted coded res type\n");
        feedback.stat = VIRGL_VIDEO_ENCODE_STAT_FAILURE;
        feedback.coded_size = 0;
    }

    /* send feedback */
    vrend_write_to_iovec(cdc->feed_res->iov, cdc->feed_res->num_iovs,
                         0, (char *)(&feedback),
                         MIN2(cdc->feed_res->base.width0, sizeof(feedback)));

    cdc->dest_res = NULL;
    cdc->feed_res = NULL;
}

static struct virgl_video_callbacks video_callbacks = {
    .decode_completed           = vrend_video_decode_completed,
    .encode_upload_picture      = vrend_video_enocde_upload_picture,
    .encode_completed           = vrend_video_encode_completed,
};

int vrend_video_init(int drm_fd)
{
#ifndef _WIN32
    /* POSIX hosts: libva needs a real DRM render-node fd. */
    if (drm_fd < 0)
        return -1;
#else
    /* Windows backend uses D3D11 device enumeration, not a DRM fd. */
    (void)drm_fd;
#endif

    return virgl_video_init(drm_fd, &video_callbacks, 0);
}

void vrend_video_fini(void)
{
    virgl_video_destroy();
}

int vrend_video_fill_caps(union virgl_caps *caps)
{
    return virgl_video_fill_caps(caps);
}

int vrend_video_create_codec(struct vrend_video_context *ctx,
                             uint32_t handle,
                             uint32_t profile,
                             uint32_t entrypoint,
                             uint32_t chroma_format,
                             uint32_t level,
                             uint32_t width,
                             uint32_t height,
                             uint32_t max_ref,
                             uint32_t flags)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, handle);
    struct virgl_video_create_codec_args args;

    if (cdc)
        return 0;

    if (profile <= PIPE_VIDEO_PROFILE_UNKNOWN ||
        profile >= PIPE_VIDEO_PROFILE_MAX)
        return -1;

    if (entrypoint <= PIPE_VIDEO_ENTRYPOINT_UNKNOWN ||
        entrypoint > PIPE_VIDEO_ENTRYPOINT_ENCODE)
        return -1;

    if (chroma_format >= PIPE_VIDEO_CHROMA_FORMAT_NONE)
        return -1;

    if (!width || !height)
        return -1;

    cdc = (struct vrend_video_codec *)calloc(1, sizeof(*cdc));
    if (!cdc)
        return -1;

    args.profile = profile;
    args.entrypoint = entrypoint;
    args.chroma_format = chroma_format;
    args.level = level;
    args.width = width;
    args.height = height;
    args.max_references = max_ref;
    args.flags = flags;
    args.opaque = cdc;
    cdc->codec = virgl_video_create_codec(&args);
    if (!cdc->codec) {
        free(cdc);
        return -1;
    }

    cdc->handle = handle;
    cdc->ctx = ctx;
    list_add(&cdc->head, &ctx->codecs);

    return 0;
}

static void destroy_video_codec(struct vrend_video_codec *cdc)
{
    if (cdc) {
        list_del(&cdc->head);
        virgl_video_destroy_codec(cdc->codec);
        free(cdc);
    }
}

void vrend_video_destroy_codec(struct vrend_video_context *ctx,
                               uint32_t handle)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, handle);

    destroy_video_codec(cdc);
}

int vrend_video_create_buffer(struct vrend_video_context *ctx,
                              uint32_t handle,
                              uint32_t format,
                              uint32_t width,
                              uint32_t height,
                              uint32_t *res_handles,
                              unsigned int num_res)
{
    unsigned i;
    struct vrend_video_plane *plane;
    struct vrend_video_buffer *buf = get_video_buffer(ctx, handle);
    struct virgl_video_create_buffer_args args;

    if (buf)
        return 0;

    if (format <= PIPE_FORMAT_NONE || format >= PIPE_FORMAT_COUNT){
        virgl_error("Invalid vrend video buffer format: %d\n", format);
        return -1;
    }

    if (!width || !height || !res_handles || !num_res)
        return -1;

    buf = (struct vrend_video_buffer *)calloc(1, sizeof(*buf));
    if (!buf)
        return -1;

    args.format = format;
    args.width = width;
    args.height = height;
    args.interlaced = 0;
    args.opaque = buf;
    buf->buffer = virgl_video_create_buffer(&args);
    if (!buf->buffer) {
        free(buf);
        return -1;
    }

#if !VREND_VIDEO_WIN32_CPU_UPLOAD
    for (i = 0; i < ARRAY_SIZE(buf->planes); i++)
        buf->planes[i].egl_image = EGL_NO_IMAGE_KHR;
#endif

    for (i = 0, buf->num_planes = 0;
         i < num_res && buf->num_planes < ARRAY_SIZE(buf->planes); i++) {

        if (!res_handles[i])
            continue;

        plane = &buf->planes[buf->num_planes++];
        plane->res_handle = res_handles[i];
        glGenFramebuffers(1, &plane->framebuffer);
        glGenTextures(1, &plane->texture);
        glBindTexture(GL_TEXTURE_2D, plane->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    buf->handle = handle;
    buf->ctx = ctx;
    list_add(&buf->head, &ctx->buffers);

    return 0;
}

static void destroy_video_buffer(struct vrend_video_buffer *buf)
{
    unsigned i;
    struct vrend_video_plane *plane;

    if (!buf)
        return;

    list_del(&buf->head);

    for (i = 0; i < buf->num_planes; i++) {
        plane = &buf->planes[i];

        glDeleteTextures(1, &plane->texture);
        glDeleteFramebuffers(1, &plane->framebuffer);
#if !VREND_VIDEO_WIN32_CPU_UPLOAD
        if (plane->egl_image == EGL_NO_IMAGE_KHR)
            eglDestroyImageKHR(eglGetCurrentDisplay(), plane->egl_image);
#endif
    }

    virgl_video_destroy_buffer(buf->buffer);

    free(buf);
}

void vrend_video_destroy_buffer(struct vrend_video_context *ctx,
                                uint32_t handle)
{
    struct vrend_video_buffer *buf = get_video_buffer(ctx, handle);

    destroy_video_buffer(buf);
}

struct vrend_video_context *vrend_video_create_context(struct vrend_context *ctx)
{
    struct vrend_video_context *vctx;

    vctx = (struct vrend_video_context *)calloc(1, sizeof(*vctx));
    if (vctx) {
        vctx->ctx = ctx;
        list_inithead(&vctx->codecs);
        list_inithead(&vctx->buffers);
    }

    return vctx;
}

void vrend_video_destroy_context(struct vrend_video_context *ctx)
{
   list_for_each_entry_safe(struct vrend_video_codec, vcdc, &ctx->codecs, head)
      destroy_video_codec(vcdc);

   list_for_each_entry_safe(struct vrend_video_buffer, vbuf, &ctx->buffers, head)
      destroy_video_buffer(vbuf);

   free(ctx);
}

int vrend_video_begin_frame(struct vrend_video_context *ctx,
                            uint32_t cdc_handle,
                            uint32_t tgt_handle)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *tgt = get_video_buffer(ctx, tgt_handle);

    if (!cdc || !tgt)
        return -1;

    return virgl_video_begin_frame(cdc->codec, tgt->buffer);
}

static void modify_h264_picture_desc(struct vrend_video_codec *cdc,
                                     struct vrend_video_buffer *tgt,
                                     struct virgl_h264_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->buffer_id); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->buffer_id[i]);
        desc->buffer_id[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_h265_picture_desc(struct vrend_video_codec *cdc,
                                     struct vrend_video_buffer *tgt,
                                     struct virgl_h265_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_mpeg12_picture_desc(struct vrend_video_codec *cdc,
                                       struct vrend_video_buffer *tgt,
                                       struct virgl_mpeg12_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}


static void modify_mjpeg_picture_desc(struct vrend_video_codec *cdc,
                                      struct vrend_video_buffer *tgt,
                                      struct virgl_mjpeg_picture_desc *desc)
{
    (void)cdc;
    (void)tgt;
    (void)desc;
}

static void modify_vc1_picture_desc(struct vrend_video_codec *cdc,
                                    struct vrend_video_buffer *tgt,
                                    struct virgl_vc1_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_vp9_picture_desc(struct vrend_video_codec *cdc,
                                     struct vrend_video_buffer *tgt,
                                     struct virgl_vp9_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }
}

static void modify_av1_picture_desc(struct vrend_video_codec *cdc,
                                    struct vrend_video_buffer *tgt,
                                    struct virgl_av1_picture_desc *desc)
{
    unsigned i;
    struct vrend_video_buffer *vbuf;

    (void)tgt;

    for (i = 0; i < ARRAY_SIZE(desc->ref); i++) {
        vbuf = get_video_buffer(cdc->ctx, desc->ref[i]);
        desc->ref[i] = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
    }

    vbuf = get_video_buffer(cdc->ctx, desc->film_grain_target);
    desc->film_grain_target = virgl_video_buffer_id(vbuf ? vbuf->buffer : NULL);
}

static void modify_picture_desc(struct vrend_video_codec *cdc,
                                struct vrend_video_buffer *tgt,
                                union virgl_picture_desc *desc)
{
    switch(virgl_video_codec_profile(cdc->codec)) {
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH10:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH422:
    case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH444:
        modify_h264_picture_desc(cdc, tgt, &desc->h264);
        break;
    case PIPE_VIDEO_PROFILE_HEVC_MAIN:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_10:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_STILL:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_12:
    case PIPE_VIDEO_PROFILE_HEVC_MAIN_444:
        modify_h265_picture_desc(cdc, tgt, &desc->h265);
        break;
    case PIPE_VIDEO_PROFILE_MPEG2_MAIN:
    case PIPE_VIDEO_PROFILE_MPEG2_SIMPLE:
        modify_mpeg12_picture_desc(cdc, tgt, &desc->mpeg12);
        break;
    case PIPE_VIDEO_PROFILE_JPEG_BASELINE:
        modify_mjpeg_picture_desc(cdc, tgt, &desc->mjpeg);
        break;
    case PIPE_VIDEO_PROFILE_VC1_SIMPLE:
    case PIPE_VIDEO_PROFILE_VC1_MAIN:
    case PIPE_VIDEO_PROFILE_VC1_ADVANCED:
        modify_vc1_picture_desc(cdc, tgt, &desc->vc1);
        break;
    case PIPE_VIDEO_PROFILE_VP9_PROFILE0:
    case PIPE_VIDEO_PROFILE_VP9_PROFILE2:
        modify_vp9_picture_desc(cdc, tgt, &desc->vp9);
        break;
    case PIPE_VIDEO_PROFILE_AV1_MAIN:
        modify_av1_picture_desc(cdc, tgt, &desc->av1);
        break;
    default:
        break;
    }
}

int vrend_video_decode_bitstream(struct vrend_video_context *ctx,
                                 uint32_t cdc_handle,
                                 uint32_t tgt_handle,
                                 uint32_t desc_handle,
                                 unsigned num_buffers,
                                 const uint32_t *buffer_handles,
                                 const uint32_t *buffer_sizes)
{
    int err = -1;
    unsigned i, num_bs, *bs_sizes = NULL;
    void **bs_buffers = NULL;
    struct vrend_resource *res;
    struct vrend_video_codec  *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *tgt = get_video_buffer(ctx, tgt_handle);
    union virgl_picture_desc desc;

    if (!cdc || !tgt){
        virgl_error("video codec: %p, video buffer: %p, invalid.\n", (void *)cdc, (void *)tgt);
        return -1;
    }

    bs_buffers = calloc(num_buffers, sizeof(void *));
    if (!bs_buffers) {
        virgl_error("%s: alloc bs_buffers failed\n", __func__);
        return -1;
    }

    bs_sizes = calloc(num_buffers, sizeof(unsigned));
    if (!bs_sizes) {
        virgl_error("%s: alloc bs_sizes failed\n", __func__);
        goto err;
    }

    for (i = 0, num_bs = 0; i < num_buffers; i++) {
        res = vrend_renderer_ctx_res_lookup(ctx->ctx, buffer_handles[i]);
        if (!res || !res->ptr) {
            virgl_warn("%s: bs res %d invalid or not found",
                       __func__, buffer_handles[i]);
            continue;
        }

        vrend_read_from_iovec(res->iov, res->num_iovs, 0,
                              res->ptr, buffer_sizes[i]);
        bs_buffers[num_bs] = res->ptr;
        bs_sizes[num_bs] = buffer_sizes[i];
        num_bs++;
    }

    res = vrend_renderer_ctx_res_lookup(ctx->ctx, desc_handle);
    if (!res) {
        virgl_error("%s: desc res %d not found\n", __func__, desc_handle);
        goto err;
    }
    memset(&desc, 0, sizeof(desc));
    vrend_read_from_iovec(res->iov, res->num_iovs, 0, (char *)(&desc),
                          MIN2(res->base.width0, sizeof(desc)));
    modify_picture_desc(cdc, tgt, &desc);

    err = virgl_video_decode_bitstream(cdc->codec, tgt->buffer, &desc,
                           num_bs, (const void * const *)bs_buffers, bs_sizes);

err:
    free(bs_buffers);
    free(bs_sizes);

    return err;
}

int vrend_video_encode_bitstream(struct vrend_video_context *ctx,
                                 uint32_t cdc_handle,
                                 uint32_t src_handle,
                                 uint32_t dest_handle,
                                 uint32_t desc_handle,
                                 uint32_t feed_handle)
{
    union virgl_picture_desc desc;
    struct vrend_resource *dest_res, *desc_res, *feed_res;
    struct vrend_video_codec  *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *src = get_video_buffer(ctx, src_handle);

    if (!cdc || !src)
        return -1;

    /* Feedback resource */
    feed_res = vrend_renderer_ctx_res_lookup(ctx->ctx, feed_handle);
    if (!feed_res) {
        virgl_error("%s: feedback res %d not found\n", __func__, feed_handle);
        return -1;
    }

    /* Picture descriptor resource */
    desc_res = vrend_renderer_ctx_res_lookup(ctx->ctx, desc_handle);
    if (!desc_res) {
        virgl_error("%s: desc res %d not found\n", __func__, desc_handle);
        return -1;
    }
    memset(&desc, 0, sizeof(desc));
    vrend_read_from_iovec(desc_res->iov, desc_res->num_iovs, 0, (char *)(&desc),
                          MIN2(desc_res->base.width0, sizeof(desc)));

    /* Destination buffer resource. */
    dest_res = vrend_renderer_ctx_res_lookup(ctx->ctx, dest_handle);
    if (!dest_res) {
        virgl_error("%s: dest res %d not found\n", __func__, dest_handle);
        return -1;
    }

    cdc->feed_res = feed_res;
    cdc->dest_res = dest_res;

    return virgl_video_encode_bitstream(cdc->codec, src->buffer, &desc);
}

int vrend_video_end_frame(struct vrend_video_context *ctx,
                          uint32_t cdc_handle,
                          uint32_t tgt_handle)
{
    struct vrend_video_codec *cdc = get_video_codec(ctx, cdc_handle);
    struct vrend_video_buffer *tgt = get_video_buffer(ctx, tgt_handle);

    if (!cdc || !tgt)
        return -1;

    return virgl_video_end_frame(cdc->codec, tgt->buffer);
}

