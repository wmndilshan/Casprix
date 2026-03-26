/*
 * Skia C Binding Layer for Casperix/Casprix Runtime
 *
 * Thin C wrapper around Skia C++ API. All Skia objects are exposed as
 * opaque void* handles. Implementation in skia_c.cpp.
 *
 * This header is pure C — safe to include from any .c file.
 */

#ifndef SKIA_C_H
#define SKIA_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Opaque Handle Types
 * ======================================================================== */

typedef void* SkiaCanvas;
typedef void* SkiaPaint;
typedef void* SkiaPath;
typedef void* SkiaSurface;
typedef void* SkiaFont;
typedef void* SkiaImage;
typedef void* SkiaShader;
typedef void* SkiaTextBlob;
typedef void* SkiaTypeface;

/* ========================================================================
 * Color Utility
 * ======================================================================== */

/* Pack RGBA into ARGB uint32 (Skia's native format) */
uint32_t skia_color_rgba(int r, int g, int b, int a);
uint32_t skia_color_rgb(int r, int g, int b);

/* Named colors */
#define SKIA_COLOR_BLACK       0xFF000000
#define SKIA_COLOR_WHITE       0xFFFFFFFF
#define SKIA_COLOR_RED         0xFFFF0000
#define SKIA_COLOR_GREEN       0xFF00FF00
#define SKIA_COLOR_BLUE        0xFF0000FF
#define SKIA_COLOR_TRANSPARENT 0x00000000

/* ========================================================================
 * Surface — Raster rendering target
 * ======================================================================== */

/* Create an RGBA raster surface (CPU rendering) */
SkiaSurface skia_surface_make_raster(int width, int height);

/* Create raster surface backed by caller-owned pixel buffer.
 * pixels must be at least width * height * 4 bytes (BGRA8888). */
SkiaSurface skia_surface_make_raster_direct(int width, int height,
                                             void* pixels, int row_bytes);

void        skia_surface_destroy(SkiaSurface surface);
SkiaCanvas  skia_surface_get_canvas(SkiaSurface surface);

/* Get direct access to pixel data. Returns pointer to pixel buffer.
 * row_bytes is set to the number of bytes per row. */
void*       skia_surface_peek_pixels(SkiaSurface surface, int* row_bytes);

/* Snapshot surface to an immutable image */
SkiaImage   skia_surface_snapshot(SkiaSurface surface);

int         skia_surface_get_width(SkiaSurface surface);
int         skia_surface_get_height(SkiaSurface surface);

/* ========================================================================
 * Canvas — Drawing commands
 * ======================================================================== */

/* Clear entire canvas with color */
void skia_canvas_clear(SkiaCanvas c, uint32_t color);

/* Primitives */
void skia_canvas_draw_rect(SkiaCanvas c, double x, double y, double w, double h,
                            SkiaPaint p);
void skia_canvas_draw_rrect(SkiaCanvas c, double x, double y, double w, double h,
                             double rx, double ry, SkiaPaint p);
void skia_canvas_draw_circle(SkiaCanvas c, double cx, double cy, double r,
                              SkiaPaint p);
void skia_canvas_draw_oval(SkiaCanvas c, double x, double y, double w, double h,
                            SkiaPaint p);
void skia_canvas_draw_line(SkiaCanvas c, double x1, double y1, double x2, double y2,
                            SkiaPaint p);
void skia_canvas_draw_path(SkiaCanvas c, SkiaPath path, SkiaPaint p);

/* Text */
void skia_canvas_draw_text(SkiaCanvas c, const char* text, double x, double y,
                            SkiaFont font, SkiaPaint p);
void skia_canvas_draw_text_blob(SkiaCanvas c, SkiaTextBlob blob, float x, float y,
                                 SkiaPaint p);

/* Image */
void skia_canvas_draw_image(SkiaCanvas c, SkiaImage img, double x, double y,
                             SkiaPaint p);
void skia_canvas_draw_image_rect(SkiaCanvas c, SkiaImage img,
                                  float sx, float sy, float sw, float sh,
                                  float dx, float dy, float dw, float dh,
                                  SkiaPaint p);

/* Transform stack */
void skia_canvas_save(SkiaCanvas c);
void skia_canvas_restore(SkiaCanvas c);
int  skia_canvas_get_save_count(SkiaCanvas c);
void skia_canvas_restore_to_count(SkiaCanvas c, int count);
void skia_canvas_translate(SkiaCanvas c, double dx, double dy);
void skia_canvas_scale(SkiaCanvas c, double sx, double sy);
void skia_canvas_rotate(SkiaCanvas c, double degrees);

/* Clipping */
void skia_canvas_clip_rect(SkiaCanvas c, double x, double y, double w, double h);
void skia_canvas_clip_rrect(SkiaCanvas c, float x, float y, float w, float h,
                             float rx, float ry);
void skia_canvas_clip_path(SkiaCanvas c, SkiaPath path);

/* ========================================================================
 * Paint — Styling for draw commands
 * ======================================================================== */

SkiaPaint skia_paint_create(void);
SkiaPaint skia_paint_clone(SkiaPaint p);
void      skia_paint_destroy(SkiaPaint p);

void skia_paint_set_color(SkiaPaint p, uint32_t argb);
void skia_paint_set_alpha(SkiaPaint p, int alpha);  /* 0-255 */
void skia_paint_set_antialias(SkiaPaint p, int aa);

/* Style: 0=fill, 1=stroke, 2=fill+stroke */
void skia_paint_set_style(SkiaPaint p, int style);
void skia_paint_set_stroke_width(SkiaPaint p, double w);
void skia_paint_set_stroke_cap(SkiaPaint p, int cap);    /* 0=butt, 1=round, 2=square */
void skia_paint_set_stroke_join(SkiaPaint p, int join);   /* 0=miter, 1=round, 2=bevel */

/* Shader (gradients, etc.) */
void skia_paint_set_shader(SkiaPaint p, SkiaShader shader);

/* Blur mask filter (for shadows) */
void skia_paint_set_blur(SkiaPaint p, float sigma);  /* Gaussian blur radius */
void skia_paint_clear_blur(SkiaPaint p);

/* ========================================================================
 * Path — Vector paths
 * ======================================================================== */

SkiaPath skia_path_create(void);
SkiaPath skia_path_clone(SkiaPath path);
void     skia_path_destroy(SkiaPath path);
void     skia_path_reset(SkiaPath path);

/* Path construction */
void skia_path_move_to(SkiaPath path, double x, double y);
void skia_path_line_to(SkiaPath path, double x, double y);
void skia_path_quad_to(SkiaPath path, double cx, double cy, double x, double y);
void skia_path_cubic_to(SkiaPath path, double c1x, double c1y,
                         double c2x, double c2y, double x, double y);
void skia_path_arc_to(SkiaPath path, float rx, float ry, float angle,
                       int large_arc, int sweep, float x, float y);
void skia_path_close(SkiaPath path);

/* Convenience shapes */
void skia_path_add_rect(SkiaPath path, double x, double y, double w, double h);
void skia_path_add_rrect(SkiaPath path, double x, double y, double w, double h,
                          double rx, double ry);
void skia_path_add_circle(SkiaPath path, double cx, double cy, double r);
void skia_path_add_oval(SkiaPath path, float x, float y, float w, float h);

/* Query */
int skia_path_contains(SkiaPath path, float x, float y);

/* ========================================================================
 * Font & Text
 * ======================================================================== */

/* Create font from system font family name */
SkiaFont skia_font_create(const char* family, double size);

/* Create font from a .ttf/.otf file */
SkiaFont skia_font_create_from_file(const char* path, double size);

/* Set font properties */
void  skia_font_set_size(SkiaFont font, double size);
void  skia_font_set_bold(SkiaFont font, int bold);
void  skia_font_set_italic(SkiaFont font, int italic);
void  skia_font_destroy(SkiaFont font);

/* Measurement */
double skia_font_measure_text(SkiaFont font, const char* text);
double skia_font_get_height(SkiaFont font);      /* Line height */
float skia_font_get_ascent(SkiaFont font);        /* Ascent (negative) */
float skia_font_get_descent(SkiaFont font);       /* Descent */
float skia_font_get_leading(SkiaFont font);       /* Inter-line spacing */

/* Measure individual character positions (for cursor placement).
 * widths array must have at least strlen(text) entries. */
void skia_font_get_char_widths(SkiaFont font, const char* text, float* widths);

/* Text blob (pre-shaped, cacheable) */
SkiaTextBlob skia_text_blob_make(const char* text, SkiaFont font);
void         skia_text_blob_destroy(SkiaTextBlob blob);

/* ========================================================================
 * Image — Bitmap images
 * ======================================================================== */

SkiaImage skia_image_load_from_file(const char* path);
SkiaImage skia_image_load_from_memory(const void* data, int size);
void      skia_image_destroy(SkiaImage img);
int       skia_image_get_width(SkiaImage img);
int       skia_image_get_height(SkiaImage img);

/* ========================================================================
 * Shader — Gradients and patterns
 * ======================================================================== */

/* Linear gradient from (x0,y0) to (x1,y1) */
SkiaShader skia_shader_linear_gradient(float x0, float y0, float x1, float y1,
                                        const uint32_t* colors,
                                        const float* positions, int count);

/* Radial gradient centered at (cx,cy) with given radius */
SkiaShader skia_shader_radial_gradient(float cx, float cy, float radius,
                                        const uint32_t* colors,
                                        const float* positions, int count);

/* Sweep (angular) gradient */
SkiaShader skia_shader_sweep_gradient(float cx, float cy,
                                       const uint32_t* colors,
                                       const float* positions, int count);

void skia_shader_destroy(SkiaShader shader);

#ifdef __cplusplus
}
#endif

#endif /* SKIA_C_H */
