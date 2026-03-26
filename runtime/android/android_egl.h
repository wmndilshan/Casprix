/*
 * android_egl.h — EGL surface management for Android native rendering
 *
 * Sets up an OpenGL ES 3.0 context on an ANativeWindow so Skia can
 * render directly to the screen via its GL backend — exactly like Flutter.
 *
 * Usage:
 *   AndroidEGL* egl = android_egl_create(window);
 *   android_egl_make_current(egl);
 *   // ... Skia renders to GL framebuffer ...
 *   android_egl_swap_buffers(egl);
 *   android_egl_destroy(egl);
 */
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AndroidEGL {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    EGLConfig  config;
    int width;
    int height;
} AndroidEGL;

/* Create EGL display, context (GLES 3.0), and window surface */
AndroidEGL* android_egl_create(ANativeWindow* window);

/* Bind the context to the current thread */
int android_egl_make_current(AndroidEGL* egl);

/* Present the rendered frame */
int android_egl_swap_buffers(AndroidEGL* egl);

/* Recreate the window surface after a resize or resume */
int android_egl_surface_changed(AndroidEGL* egl, ANativeWindow* window);

/* Query current drawable size */
void android_egl_get_size(AndroidEGL* egl, int* width, int* height);

/* Destroy EGL resources */
void android_egl_destroy(AndroidEGL* egl);

#ifdef __cplusplus
}
#endif
