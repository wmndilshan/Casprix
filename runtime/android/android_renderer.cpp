/*
 * android_renderer.c — Skia GrDirectContext on GLES backend
 *
 * This is the core "Flutter-like" rendering layer.
 * Skia draws everything; Android never touches the View system.
 */

#include "android_renderer.h"
#include <android/log.h>
#include <GLES3/gl3.h>
#include <stdlib.h>
#include <string.h>

/* Skia C++ headers — compiled as C++ translation unit */
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/gl/GrGLAssembleInterface.h"
#include "include/gpu/gl/GrGLInterface.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "CasprixRenderer", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CasprixRenderer", __VA_ARGS__)

struct AndroidRenderer {
    AndroidEGL*             egl;
    sk_sp<GrDirectContext>  gr_context;
    sk_sp<SkSurface>        surface;
    int                     width;
    int                     height;
};

static sk_sp<const GrGLInterface> make_gl_interface(void) {
    return GrGLMakeAssembledInterface(nullptr,
        [](void*, const char* name) -> GrGLFuncPtr {
            return reinterpret_cast<GrGLFuncPtr>(eglGetProcAddress(name));
        });
}

static sk_sp<SkSurface> make_surface(GrDirectContext* ctx, int w, int h) {
    GrGLint framebuffer = 0;
    GrGLint stencil_bits = 0;
    GrGLFramebufferInfo fb_info;
    auto color_space = SkColorSpace::MakeSRGB();
    auto color_type = kRGBA_8888_SkColorType;

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer == 0) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    }
    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);

    fb_info.fFBOID = static_cast<GrGLuint>(framebuffer);
    fb_info.fFormat = GL_RGBA8;

    GrBackendRenderTarget backend_rt(w, h, 0, stencil_bits, fb_info);
    return SkSurfaces::WrapBackendRenderTarget(
        ctx,
        backend_rt,
        kBottomLeft_GrSurfaceOrigin,
        color_type,
        color_space,
        nullptr);
}

AndroidRenderer* android_renderer_create(AndroidEGL* egl) {
    AndroidRenderer* r = new AndroidRenderer();
    r->egl = egl;

    android_egl_get_size(egl, &r->width, &r->height);

    auto gl_iface = make_gl_interface();
    if (!gl_iface) {
        LOGE("Failed to create GL interface");
        delete r;
        return nullptr;
    }

    r->gr_context = GrDirectContext::MakeGL(gl_iface);
    if (!r->gr_context) {
        LOGE("GrDirectContexts::MakeGL failed");
        delete r;
        return nullptr;
    }

    r->surface = make_surface(r->gr_context.get(), r->width, r->height);
    if (!r->surface) {
        LOGE("SkSurfaces::WrapBackendRenderTarget failed");
        delete r;
        return nullptr;
    }

    LOGI("Skia GPU renderer created (%dx%d)", r->width, r->height);
    return r;
}

void android_renderer_resize(AndroidRenderer* r, int width, int height) {
    if (!r || (r->width == width && r->height == height)) return;
    r->width = width;
    r->height = height;
    if (r->gr_context) {
        r->gr_context->resetContext();
    }
    r->surface = make_surface(r->gr_context.get(), width, height);
    LOGI("Renderer resized: %dx%d", width, height);
}

void* android_renderer_begin_frame(AndroidRenderer* r) {
    if (!r || !r->surface) return nullptr;
    SkCanvas* canvas = r->surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    return (void*)canvas;
}

void android_renderer_end_frame(AndroidRenderer* r) {
    if (!r || !r->surface || !r->gr_context) return;
    r->gr_context->flushAndSubmit(r->surface, false);
    android_egl_swap_buffers(r->egl);
}

void android_renderer_destroy(AndroidRenderer* r) {
    if (!r) return;
    r->surface.reset();
    r->gr_context.reset();
    delete r;
}
