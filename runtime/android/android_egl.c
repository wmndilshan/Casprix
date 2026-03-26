/*
 * android_egl.c — EGL / OpenGL ES 3.0 surface initialization
 *
 * Provides the OpenGL ES context that Skia's GPU backend draws into.
 * This is the exact same approach Flutter uses: ANativeWindow → EGL → GLES.
 */

#include "android_egl.h"
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "CasprixEGL", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CasprixEGL", __VA_ARGS__)

/* EGL attribute lists for GLES 3.0 with 8888 RGBA + depth */
static const EGLint CONFIG_ATTRIBS[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      0,   /* Skia manages depth internally */
    EGL_STENCIL_SIZE,    8,   /* Skia needs 8-bit stencil */
    EGL_NONE
};

static const EGLint CONTEXT_ATTRIBS[] = {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE
};

/* ──────────────────────────────────────────────────────────────────────────── */

AndroidEGL* android_egl_create(ANativeWindow* window) {
    AndroidEGL* egl = (AndroidEGL*)calloc(1, sizeof(AndroidEGL));
    if (!egl) return NULL;

    /* 1. Get default display */
    egl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl->display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        free(egl); return NULL;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl->display, &major, &minor)) {
        LOGE("eglInitialize failed");
        free(egl); return NULL;
    }
    LOGI("EGL %d.%d initialized", major, minor);

    /* 2. Choose config */
    EGLint num_configs = 0;
    if (!eglChooseConfig(egl->display, CONFIG_ATTRIBS, &egl->config, 1, &num_configs)
        || num_configs == 0) {
        LOGE("eglChooseConfig failed (num=%d)", num_configs);
        eglTerminate(egl->display);
        free(egl); return NULL;
    }

    /* 3. Create GL context */
    egl->context = eglCreateContext(egl->display, egl->config,
                                    EGL_NO_CONTEXT, CONTEXT_ATTRIBS);
    if (egl->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed (err 0x%x)", eglGetError());
        eglTerminate(egl->display);
        free(egl); return NULL;
    }

    /* 4. Create window surface */
    egl->surface = eglCreateWindowSurface(egl->display, egl->config,
                                          window, NULL);
    if (egl->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed (err 0x%x)", eglGetError());
        eglDestroyContext(egl->display, egl->context);
        eglTerminate(egl->display);
        free(egl); return NULL;
    }

    /* 5. Query surface size */
    eglQuerySurface(egl->display, egl->surface, EGL_WIDTH,  &egl->width);
    eglQuerySurface(egl->display, egl->surface, EGL_HEIGHT, &egl->height);
    LOGI("EGL surface: %d x %d", egl->width, egl->height);

    return egl;
}

int android_egl_make_current(AndroidEGL* egl) {
    if (!eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context)) {
        LOGE("eglMakeCurrent failed (err 0x%x)", eglGetError());
        return 0;
    }
    return 1;
}

int android_egl_swap_buffers(AndroidEGL* egl) {
    return eglSwapBuffers(egl->display, egl->surface) == EGL_TRUE ? 1 : 0;
}

int android_egl_surface_changed(AndroidEGL* egl, ANativeWindow* window) {
    /* Destroy old surface and recreate for the new window */
    if (egl->surface != EGL_NO_SURFACE) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(egl->display, egl->surface);
        egl->surface = EGL_NO_SURFACE;
    }
    egl->surface = eglCreateWindowSurface(egl->display, egl->config, window, NULL);
    if (egl->surface == EGL_NO_SURFACE) {
        LOGE("surface recreate failed");
        return 0;
    }
    eglQuerySurface(egl->display, egl->surface, EGL_WIDTH,  &egl->width);
    eglQuerySurface(egl->display, egl->surface, EGL_HEIGHT, &egl->height);
    LOGI("Surface resized: %d x %d", egl->width, egl->height);
    return android_egl_make_current(egl);
}

void android_egl_get_size(AndroidEGL* egl, int* w, int* h) {
    if (w) *w = egl->width;
    if (h) *h = egl->height;
}

void android_egl_destroy(AndroidEGL* egl) {
    if (!egl) return;
    if (egl->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl->surface  != EGL_NO_SURFACE)  eglDestroySurface(egl->display, egl->surface);
        if (egl->context  != EGL_NO_CONTEXT)  eglDestroyContext(egl->display, egl->context);
        eglTerminate(egl->display);
    }
    free(egl);
}
