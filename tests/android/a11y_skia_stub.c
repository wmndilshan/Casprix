/*
 * a11y_skia_stub.c — minimal no-op skia_* surface so the a11y-bridge test can
 * link the scene-graph / widget / text runtime without a real Skia backend.
 * None of these are reached on the paths test_a11y_bridge.c exercises (the
 * bridge reads node fields; it never paints). Compiled WITHOUT skia_c.h so the
 * float/double signatures are irrelevant — only the symbol names matter.
 */
#include <stdint.h>
#include <string.h>

typedef void* P;

/* canvas */
void skia_canvas_clip_rect(P a, double b, double c, double d, double e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void skia_canvas_clip_rrect(P a, double b, double c, double d, double e, double f, double g) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; }
void skia_canvas_draw_image_rect(P a, P b, double c, double d, double e, double f, double g, double h, double i, double j, P k) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k; }
void skia_canvas_draw_path(P a, P b, P c) { (void)a;(void)b;(void)c; }
void skia_canvas_draw_rect(P a, double b, double c, double d, double e, P f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void skia_canvas_draw_rrect(P a, double b, double c, double d, double e, double f, double g, P h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; }
void skia_canvas_draw_text(P a, const char* b, double c, double d, P e, P f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void skia_canvas_restore(P a) { (void)a; }
void skia_canvas_rotate(P a, double b) { (void)a;(void)b; }
void skia_canvas_save(P a) { (void)a; }
void skia_canvas_scale(P a, double b, double c) { (void)a;(void)b;(void)c; }
void skia_canvas_translate(P a, double b, double c) { (void)a;(void)b;(void)c; }

/* font */
P     skia_font_create(const char* a, double b) { (void)a;(void)b; return (P)1; }
void  skia_font_destroy(P a) { (void)a; }
float skia_font_get_ascent(P a) { (void)a; return -16.0f; }
double skia_font_get_height(P a) { (void)a; return 20.0; }
double skia_font_measure_text(P a, const char* b) { (void)a; return b ? (double)strlen(b) * 10.0 : 0.0; }
void  skia_font_get_char_widths(P a, const char* b, float* w) { (void)a; int n = b ? (int)strlen(b) : 0; for (int i = 0; i < n; i++) w[i] = 10.0f; }
void  skia_font_set_bold(P a, int b) { (void)a;(void)b; }
void  skia_font_set_italic(P a, int b) { (void)a;(void)b; }

/* image */
void skia_image_destroy(P a) { (void)a; }
int  skia_image_get_height(P a) { (void)a; return 0; }
int  skia_image_get_width(P a) { (void)a; return 0; }
P    skia_image_load_from_file(const char* a) { (void)a; return 0; }

/* paint */
void skia_paint_clear_blur(P a) { (void)a; }
P    skia_paint_create(void) { return (P)1; }
void skia_paint_destroy(P a) { (void)a; }
void skia_paint_set_alpha(P a, int b) { (void)a;(void)b; }
void skia_paint_set_antialias(P a, int b) { (void)a;(void)b; }
void skia_paint_set_blur(P a, double b) { (void)a;(void)b; }
void skia_paint_set_color(P a, uint32_t b) { (void)a;(void)b; }
void skia_paint_set_shader(P a, P b) { (void)a;(void)b; }
void skia_paint_set_stroke_width(P a, double b) { (void)a;(void)b; }
void skia_paint_set_style(P a, int b) { (void)a;(void)b; }

/* path / shader / blob */
void skia_path_destroy(P a) { (void)a; }
void skia_shader_destroy(P a) { (void)a; }
P    skia_shader_linear_gradient(double a, double b, double c, double d, const uint32_t* e, const float* f, int g, int h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return 0; }
P    skia_shader_radial_gradient(double a, double b, double c, const uint32_t* d, const float* e, int f, int g) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; return 0; }
P    skia_text_blob_make(const char* a, P b) { (void)a;(void)b; return 0; }
void skia_text_blob_destroy(P a) { (void)a; }
