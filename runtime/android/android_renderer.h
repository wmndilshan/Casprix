/*
 * android_renderer.h — Skia renderer on top of EGL/GLES for Android
 *
 * Creates a GrDirectContext (Skia GPU backend on OpenGL ES) and an SkSurface
 * backed by the EGL framebuffer.  The Casprix scene graph renders into this
 * SkSurface, then we call eglSwapBuffers to present.
 *
 * Architecture (identical to pre-Impeller Flutter):
 *
 *   ANativeWindow
 *       └─ AndroidEGL (GLES 3.0 context)
 *           └─ GrDirectContext (Skia GPU context)
 *               └─ SkSurface (wraps EGL renderbuffer)
 *                   └─ SkCanvas  ←  scene_graph_render(&canvas)
 */
#pragma once

#include "android_egl.h"

/* Opaque renderer handle */
typedef struct AndroidRenderer AndroidRenderer;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create the Skia GPU context + surface.
 * Must be called after android_egl_make_current().
 */
AndroidRenderer* android_renderer_create(AndroidEGL* egl);

/*
 * Resize the SkSurface (call when window size changes).
 */
void android_renderer_resize(AndroidRenderer* r, int width, int height);

/*
 * Begin a frame — returns the raw SkCanvas pointer (cast to void* for C API).
 * Caller draws the entire scene graph onto this canvas.
 */
void* android_renderer_begin_frame(AndroidRenderer* r);

/*
 * End the frame — flushes Skia commands and calls eglSwapBuffers.
 */
void android_renderer_end_frame(AndroidRenderer* r);

/*
 * Destroy the Skia GPU context and surface.
 */
void android_renderer_destroy(AndroidRenderer* r);

#ifdef __cplusplus
}
#endif
