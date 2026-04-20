/*
 * Platform Window Host Interface for Casperix/Casprix UI
 *
 * This header defines the generic window + event host contract used by
 * SGApp/frame_loop. Platform backends (Win32 today, X11/Wayland later)
 * implement this API and translate native system input into SGEvent values.
 */

#ifndef SKIA_WINDOW_HOST_H
#define SKIA_WINDOW_HOST_H

#include "skia_c.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SkiaWindow SkiaWindow;

typedef void (*SkiaWindowPaintCallback)(SkiaWindow* win, SkiaCanvas canvas, void* ctx);
typedef void (*SkiaWindowEventCallback)(SkiaWindow* win, const SGEvent* event, void* ctx);

SkiaWindow* skia_window_create(const char* title, int width, int height);
void        skia_window_destroy(SkiaWindow* win);

void        skia_window_show(SkiaWindow* win);
void        skia_window_hide(SkiaWindow* win);
void        skia_window_present(SkiaWindow* win);

SkiaCanvas  skia_window_get_canvas(SkiaWindow* win);
int         skia_window_get_width(SkiaWindow* win);
int         skia_window_get_height(SkiaWindow* win);
void        skia_window_set_title(SkiaWindow* win, const char* title);

void        skia_window_invalidate(SkiaWindow* win);
int         skia_window_needs_redraw(SkiaWindow* win);

int         skia_window_run(SkiaWindow* win);
int         skia_window_poll(SkiaWindow* win);
void        skia_window_quit(SkiaWindow* win);

void        skia_window_set_paint_callback(SkiaWindow* win,
                                           SkiaWindowPaintCallback cb, void* ctx);
void        skia_window_set_event_callback(SkiaWindow* win,
                                           SkiaWindowEventCallback cb, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* SKIA_WINDOW_HOST_H */
