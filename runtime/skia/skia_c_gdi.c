/*
 * GDI + Software AA Rendering Backend for Casprix Skia UI Framework
 *
 * Implements the full skia_c.h API using:
 *   - SDF-based pixel renderer for antialiased shapes (rect, rrect, circle)
 *   - Multi-stop gradient sampling (linear + radial)
 *   - Alpha-blended compositing (src-over)
 *   - Win32 GDI for text (ClearType), paths, and clipping
 *   - BMP image loading via Win32 LoadImage
 *
 * No Skia C++ dependency — compiles with MinGW.
 *
 * Remaining limitations vs real Skia:
 *   - No rotation support for shapes (translation + scale only)
 *   - Gradients on stroked shapes not supported
 *   - Path rendering uses GDI (no AA for arbitrary paths)
 */

#ifdef _WIN32

#include "skia_c.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ========================================================================
 * Internal Types
 * ======================================================================== */

typedef struct GdiCanvasImpl GdiCanvasImpl;

typedef struct {
    float tx, ty, sx, sy;
    int dc_save_id;
} CanvasState;

struct GdiCanvasImpl {
    HDC hdc;
    struct GdiSurfaceImpl* surface;
    CanvasState states[64];
    int save_count;
    float tx, ty, sx, sy;
};

typedef struct GdiSurfaceImpl {
    int width, height;
    HDC hdc;
    HBITMAP hbitmap;
    HBITMAP old_bitmap;
    void* bits;
    int row_bytes;
    /* For make_raster_direct: external pixel buffer */
    void* ext_pixels;
    int ext_row_bytes;
    /* Embedded canvas */
    GdiCanvasImpl canvas;
    int canvas_init;
} GdiSurfaceImpl;

typedef struct {
    uint32_t color;
    int style;          /* 0=fill, 1=stroke, 2=fill+stroke */
    float stroke_width;
    int antialias;
    int alpha;          /* 0-255 */
    float blur_sigma;
    void* shader;       /* GdiShaderImpl* */
} GdiPaintImpl;

typedef struct {
    HFONT hfont;
    char family[64];
    float size;
    int bold, italic;
    TEXTMETRICA tm;
} GdiFontImpl;

typedef enum { PATH_MOVE, PATH_LINE, PATH_QUAD, PATH_CUBIC, PATH_CLOSE } PathOp;
typedef struct { PathOp op; float pts[6]; } PathCmd;

typedef struct {
    PathCmd* cmds;
    int count, capacity;
} GdiPathImpl;

typedef struct {
    int type;   /* 0=linear, 1=radial, 2=sweep */
    float x0, y0, x1, y1, radius;
    uint32_t* colors;
    float* positions;
    int count;
} GdiShaderImpl;

typedef struct {
    char* text;
    void* font;  /* GdiFontImpl* */
} GdiTextBlobImpl;

typedef struct {
    int width, height;
    uint32_t* pixels;  /* ARGB pixel data (top-down) */
} GdiImageImpl;

/* ========================================================================
 * Helpers
 * ======================================================================== */

static COLORREF argb_to_cr(uint32_t argb) {
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

/* Transform local coords to device coords (integer) */
static int dev_x(GdiCanvasImpl* c, float x) { return (int)(c->tx + x * c->sx + 0.5f); }
static int dev_y(GdiCanvasImpl* c, float y) { return (int)(c->ty + y * c->sy + 0.5f); }
static int dev_w(GdiCanvasImpl* c, float w) { return (int)(w * c->sx + 0.5f); }
static int dev_h(GdiCanvasImpl* c, float h) { return (int)(h * c->sy + 0.5f); }

/* Transform local coords to device coords (float, for AA renderer) */
static float dev_xf(GdiCanvasImpl* c, float x) { return c->tx + x * c->sx; }
static float dev_yf(GdiCanvasImpl* c, float y) { return c->ty + y * c->sy; }
static float dev_wf(GdiCanvasImpl* c, float w) { return w * c->sx; }
static float dev_hf(GdiCanvasImpl* c, float h) { return h * c->sy; }

/* Select brush+pen for paint style, return old objects for cleanup */
typedef struct { HBRUSH ob, nb; HPEN op, np; } PaintState;

static PaintState select_paint(HDC hdc, GdiPaintImpl* p) {
    PaintState s;
    COLORREF cr = argb_to_cr(p->color);

    if (p->style == 0 || p->style == 2) {
        s.nb = CreateSolidBrush(cr);
    } else {
        s.nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    }
    s.ob = (HBRUSH)SelectObject(hdc, s.nb);

    if (p->style == 1 || p->style == 2) {
        int w = (int)(p->stroke_width + 0.5f);
        if (w < 1) w = 1;
        s.np = CreatePen(PS_SOLID, w, cr);
    } else {
        s.np = (HPEN)GetStockObject(NULL_PEN);
    }
    s.op = (HPEN)SelectObject(hdc, s.np);
    return s;
}

static void restore_paint(HDC hdc, PaintState* s) {
    SelectObject(hdc, s->ob);
    SelectObject(hdc, s->op);
    if (s->nb != (HBRUSH)GetStockObject(NULL_BRUSH)) DeleteObject(s->nb);
    if (s->np != (HPEN)GetStockObject(NULL_PEN)) DeleteObject(s->np);
}

static void recreate_gdi_font(GdiFontImpl* f) {
    if (f->hfont) DeleteObject(f->hfont);
    int height = -(int)(f->size + 0.5f);
    f->hfont = CreateFontA(height, 0, 0, 0,
        f->bold ? FW_BOLD : FW_NORMAL,
        f->italic, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, f->family);
    HDC hdc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, f->hfont);
    GetTextMetricsA(hdc, &f->tm);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
}

static void path_push(GdiPathImpl* p, PathOp op,
                       float a, float b, float c, float d, float e, float f_) {
    if (p->count >= p->capacity) {
        p->capacity = p->capacity ? p->capacity * 2 : 32;
        p->cmds = (PathCmd*)realloc(p->cmds, (size_t)p->capacity * sizeof(PathCmd));
    }
    PathCmd* cmd = &p->cmds[p->count++];
    cmd->op = op;
    cmd->pts[0] = a; cmd->pts[1] = b;
    cmd->pts[2] = c; cmd->pts[3] = d;
    cmd->pts[4] = e; cmd->pts[5] = f_;
}

/* ========================================================================
 * Software Antialiased Renderer (SDF-based)
 *
 * Renders shapes directly to the pixel buffer with subpixel-accurate
 * antialiased edges via signed distance fields. This bypasses GDI for
 * the common UI shapes (rect, rrect, circle) to achieve smooth edges.
 * ======================================================================== */

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* SDF for rounded rectangle centered at origin; b = half-extents, r = corner radius */
static float sdf_rrect(float px, float py, float bx, float by, float r) {
    float dx = fabsf(px) - bx + r;
    float dy = fabsf(py) - by + r;
    float outside = sqrtf(fmaxf(dx, 0) * fmaxf(dx, 0) + fmaxf(dy, 0) * fmaxf(dy, 0)) - r;
    float inside = fminf(fmaxf(dx, dy), 0);
    return outside + inside;
}

/* Alpha-blend src over dst with coverage [0..255] */
static uint32_t blend_over(uint32_t dst, uint32_t src, int coverage) {
    int sa = ((src >> 24) & 0xFF) * coverage / 255;
    if (sa <= 0) return dst;
    if (sa >= 255) return (src & 0x00FFFFFF) | 0xFF000000;
    int inv = 255 - sa;
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    int sr = (src >> 16) & 0xFF, sg_ = (src >> 8) & 0xFF, sb = src & 0xFF;
    int da = (dst >> 24) & 0xFF;
    int oa = sa + (da * inv) / 255;
    int or_ = (sr * sa + dr * inv) / 255;
    int og = (sg_ * sa + dg * inv) / 255;
    int ob = (sb * sa + db * inv) / 255;
    return ((uint32_t)oa << 24) | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | (uint32_t)ob;
}

/* Sample multi-stop gradient at parametric t ∈ [0,1] */
static uint32_t sample_gradient(GdiShaderImpl* sh, float t) {
    if (!sh || sh->count < 2) return 0xFF000000;
    t = clampf(t, 0.0f, 1.0f);
    float local_t = t;
    int i0 = 0, i1 = 1;
    for (int i = 0; i < sh->count - 1; i++) {
        float p0 = sh->positions ? sh->positions[i] : (float)i / (sh->count - 1);
        float p1 = sh->positions ? sh->positions[i + 1] : (float)(i + 1) / (sh->count - 1);
        if (t >= p0 && t <= p1) {
            i0 = i; i1 = i + 1;
            float range = p1 - p0;
            local_t = range > 1e-4f ? (t - p0) / range : 0.0f;
            break;
        }
    }
    /* Check last stop */
    if (t >= 1.0f) { i0 = sh->count - 2; i1 = sh->count - 1; local_t = 1.0f; }
    uint32_t c0 = sh->colors[i0], c1 = sh->colors[i1];
    float s = local_t, inv = 1.0f - s;
    int a = (int)(((c0 >> 24) & 0xFF) * inv + ((c1 >> 24) & 0xFF) * s);
    int r = (int)(((c0 >> 16) & 0xFF) * inv + ((c1 >> 16) & 0xFF) * s);
    int g = (int)(((c0 >> 8) & 0xFF) * inv + ((c1 >> 8) & 0xFF) * s);
    int b = (int)((c0 & 0xFF) * inv + (c1 & 0xFF) * s);
    return ((uint32_t)(a & 0xFF) << 24) | ((uint32_t)(r & 0xFF) << 16) |
           ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
}

/* Get gradient color at pixel position (in local coords) */
static uint32_t gradient_at(GdiShaderImpl* sh, float lx, float ly) {
    if (sh->type == 0) { /* Linear */
        float dx = sh->x1 - sh->x0, dy = sh->y1 - sh->y0;
        float len2 = dx * dx + dy * dy;
        if (len2 < 1e-4f) return sh->colors[0];
        float t = ((lx - sh->x0) * dx + (ly - sh->y0) * dy) / len2;
        return sample_gradient(sh, t);
    } else if (sh->type == 1) { /* Radial */
        float dx = lx - sh->x0, dy = ly - sh->y0;
        float t = sh->radius > 0.0f ? sqrtf(dx * dx + dy * dy) / sh->radius : 0.0f;
        return sample_gradient(sh, t);
    }
    return sh->colors[0];
}

/* Get fill color for a paint at a pixel position */
static uint32_t paint_color_at(GdiPaintImpl* paint, GdiCanvasImpl* cv,
                                 float dev_px, float dev_py) {
    if (paint->shader) {
        float lx = (dev_px - cv->tx) / cv->sx;
        float ly = (dev_py - cv->ty) / cv->sy;
        return gradient_at((GdiShaderImpl*)paint->shader, lx, ly);
    }
    return paint->color;
}

/* AA-fill a rounded rectangle (or plain rect when r=0) */
static void aa_fill_rrect(GdiSurfaceImpl* surf, GdiCanvasImpl* cv,
                            float x, float y, float w, float h,
                            float r, GdiPaintImpl* paint) {
    GdiFlush();
    uint32_t* pixels = (uint32_t*)surf->bits;
    int stride = surf->width;

    float dx = dev_xf(cv, x), dy = dev_yf(cv, y);
    float dw = dev_wf(cv, w), dh = dev_hf(cv, h);
    float dr = r * cv->sx;
    if (dr > dw * 0.5f) dr = dw * 0.5f;
    if (dr > dh * 0.5f) dr = dh * 0.5f;
    float cx = dx + dw * 0.5f, cy = dy + dh * 0.5f;
    float hw = dw * 0.5f, hh = dh * 0.5f;
    int pa = paint->alpha;

    int x0 = (int)floorf(dx - 1); if (x0 < 0) x0 = 0;
    int y0 = (int)floorf(dy - 1); if (y0 < 0) y0 = 0;
    int x1 = (int)ceilf(dx + dw + 1); if (x1 > surf->width) x1 = surf->width;
    int y1 = (int)ceilf(dy + dh + 1); if (y1 > surf->height) y1 = surf->height;

    for (int py = y0; py < y1; py++) {
        float fpy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++) {
            float fpx = (float)px + 0.5f;
            float dist = sdf_rrect(fpx - cx, fpy - cy, hw, hh, dr);
            float cov = clampf(0.5f - dist, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            int coverage = (int)(cov * pa + 0.5f);
            if (coverage <= 0) continue;
            uint32_t fill = paint_color_at(paint, cv, fpx, fpy);
            pixels[py * stride + px] = blend_over(pixels[py * stride + px], fill, coverage);
        }
    }
}

/* AA-fill a circle */
static void aa_fill_circle(GdiSurfaceImpl* surf, GdiCanvasImpl* cv,
                             float cx_l, float cy_l, float radius,
                             GdiPaintImpl* paint) {
    GdiFlush();
    uint32_t* pixels = (uint32_t*)surf->bits;
    int stride = surf->width;

    float dcx = dev_xf(cv, cx_l), dcy = dev_yf(cv, cy_l);
    float dr = radius * cv->sx;
    int pa = paint->alpha;

    int x0 = (int)floorf(dcx - dr - 1); if (x0 < 0) x0 = 0;
    int y0 = (int)floorf(dcy - dr - 1); if (y0 < 0) y0 = 0;
    int x1 = (int)ceilf(dcx + dr + 1); if (x1 > surf->width) x1 = surf->width;
    int y1 = (int)ceilf(dcy + dr + 1); if (y1 > surf->height) y1 = surf->height;

    for (int py = y0; py < y1; py++) {
        float fpy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++) {
            float fpx = (float)px + 0.5f;
            float ddx = fpx - dcx, ddy = fpy - dcy;
            float dist = sqrtf(ddx * ddx + ddy * ddy) - dr;
            float cov = clampf(0.5f - dist, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            int coverage = (int)(cov * pa + 0.5f);
            if (coverage <= 0) continue;
            uint32_t fill = paint_color_at(paint, cv, fpx, fpy);
            pixels[py * stride + px] = blend_over(pixels[py * stride + px], fill, coverage);
        }
    }
}

/* AA-stroke a rounded rectangle outline */
static void aa_stroke_rrect(GdiSurfaceImpl* surf, GdiCanvasImpl* cv,
                              float x, float y, float w, float h,
                              float r, GdiPaintImpl* paint) {
    GdiFlush();
    uint32_t* pixels = (uint32_t*)surf->bits;
    int stride = surf->width;

    float dx = dev_xf(cv, x), dy = dev_yf(cv, y);
    float dw = dev_wf(cv, w), dh = dev_hf(cv, h);
    float dr = r * cv->sx;
    if (dr > dw * 0.5f) dr = dw * 0.5f;
    if (dr > dh * 0.5f) dr = dh * 0.5f;
    float cx = dx + dw * 0.5f, cy = dy + dh * 0.5f;
    float hw = dw * 0.5f, hh = dh * 0.5f;
    float sw = paint->stroke_width * cv->sx;
    float half_s = sw * 0.5f;
    int pa = paint->alpha;
    uint32_t col = paint->color;

    int x0 = (int)floorf(dx - half_s - 1); if (x0 < 0) x0 = 0;
    int y0 = (int)floorf(dy - half_s - 1); if (y0 < 0) y0 = 0;
    int x1 = (int)ceilf(dx + dw + half_s + 1); if (x1 > surf->width) x1 = surf->width;
    int y1 = (int)ceilf(dy + dh + half_s + 1); if (y1 > surf->height) y1 = surf->height;

    for (int py = y0; py < y1; py++) {
        float fpy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++) {
            float fpx = (float)px + 0.5f;
            float dist = sdf_rrect(fpx - cx, fpy - cy, hw, hh, dr);
            float sd = fabsf(dist) - half_s;
            float cov = clampf(0.5f - sd, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            int coverage = (int)(cov * pa + 0.5f);
            if (coverage <= 0) continue;
            pixels[py * stride + px] = blend_over(pixels[py * stride + px], col, coverage);
        }
    }
}

/* ========================================================================
 * Color Utility
 * ======================================================================== */

uint32_t skia_color_rgba(int r, int g, int b, int a) {
    return ((uint32_t)(a & 0xFF) << 24) | ((uint32_t)(r & 0xFF) << 16) |
           ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
}

uint32_t skia_color_rgb(int r, int g, int b) {
    return skia_color_rgba(r, g, b, 255);
}

/* ========================================================================
 * Surface
 * ======================================================================== */

SkiaSurface skia_surface_make_raster(int width, int height) {
    GdiSurfaceImpl* s = (GdiSurfaceImpl*)calloc(1, sizeof(GdiSurfaceImpl));
    if (!s) return NULL;
    s->width = width;
    s->height = height;
    s->row_bytes = width * 4;

    HDC screen_dc = GetDC(NULL);
    s->hdc = CreateCompatibleDC(screen_dc);
    ReleaseDC(NULL, screen_dc);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s->hbitmap = CreateDIBSection(s->hdc, &bmi, DIB_RGB_COLORS, &s->bits, NULL, 0);
    if (!s->hbitmap) {
        DeleteDC(s->hdc);
        free(s);
        return NULL;
    }
    s->old_bitmap = (HBITMAP)SelectObject(s->hdc, s->hbitmap);
    SetBkMode(s->hdc, TRANSPARENT);
    return (SkiaSurface)s;
}

SkiaSurface skia_surface_make_raster_direct(int width, int height,
                                             void* pixels, int row_bytes) {
    GdiSurfaceImpl* s = (GdiSurfaceImpl*)skia_surface_make_raster(width, height);
    if (!s) return NULL;
    s->ext_pixels = pixels;
    s->ext_row_bytes = row_bytes;
    return (SkiaSurface)s;
}

void skia_surface_destroy(SkiaSurface surface) {
    GdiSurfaceImpl* s = (GdiSurfaceImpl*)surface;
    if (!s) return;
    if (s->old_bitmap) SelectObject(s->hdc, s->old_bitmap);
    if (s->hbitmap) DeleteObject(s->hbitmap);
    if (s->hdc) DeleteDC(s->hdc);
    free(s);
}

SkiaCanvas skia_surface_get_canvas(SkiaSurface surface) {
    GdiSurfaceImpl* s = (GdiSurfaceImpl*)surface;
    if (!s) return NULL;
    if (!s->canvas_init) {
        s->canvas.hdc = s->hdc;
        s->canvas.surface = s;
        s->canvas.tx = 0; s->canvas.ty = 0;
        s->canvas.sx = 1.0f; s->canvas.sy = 1.0f;
        s->canvas.save_count = 0;
        s->canvas_init = 1;
    }
    return (SkiaCanvas)&s->canvas;
}

void* skia_surface_peek_pixels(SkiaSurface surface, int* row_bytes) {
    GdiSurfaceImpl* s = (GdiSurfaceImpl*)surface;
    if (!s) return NULL;
    GdiFlush();
    /* If using external pixel buffer (raster_direct), copy internal DIB to it */
    if (s->ext_pixels && s->ext_pixels != s->bits) {
        int copy_row = s->width * 4;
        if (copy_row == s->ext_row_bytes) {
            memcpy(s->ext_pixels, s->bits, (size_t)copy_row * s->height);
        } else {
            for (int y = 0; y < s->height; y++) {
                memcpy((char*)s->ext_pixels + y * s->ext_row_bytes,
                       (char*)s->bits + y * s->row_bytes,
                       (size_t)(s->width * 4));
            }
        }
    }
    if (row_bytes) *row_bytes = s->ext_pixels ? s->ext_row_bytes : s->row_bytes;
    return s->ext_pixels ? s->ext_pixels : s->bits;
}

SkiaImage skia_surface_snapshot(SkiaSurface surface) {
    (void)surface;
    return NULL; /* stub */
}

int skia_surface_get_width(SkiaSurface surface) {
    GdiSurfaceImpl* s = (GdiSurfaceImpl*)surface;
    return s ? s->width : 0;
}

int skia_surface_get_height(SkiaSurface surface) {
    GdiSurfaceImpl* s = (GdiSurfaceImpl*)surface;
    return s ? s->height : 0;
}

/* ========================================================================
 * Canvas — Clear
 * ======================================================================== */

void skia_canvas_clear(SkiaCanvas c, uint32_t color) {
    GdiCanvasImpl* canvas = (GdiCanvasImpl*)c;
    if (!canvas || !canvas->surface) return;
    GdiSurfaceImpl* s = canvas->surface;
    GdiFlush(); /* Flush any pending GDI ops before direct pixel access */
    uint32_t* px = (uint32_t*)s->bits;
    int count = s->width * s->height;
    for (int i = 0; i < count; i++) px[i] = color;
}

/* ========================================================================
 * Canvas — Drawing Primitives
 * ======================================================================== */

void skia_canvas_draw_rect(SkiaCanvas c, double x, double y, double w, double h,
                           SkiaPaint p) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (!cv || !paint || !cv->surface) return;

    if (paint->style == 0 || paint->style == 2) {
        aa_fill_rrect(cv->surface, cv, x, y, w, h, 0.0f, paint);
    }
    if (paint->style == 1 || paint->style == 2) {
        aa_stroke_rrect(cv->surface, cv, x, y, w, h, 0.0f, paint);
    }
}

void skia_canvas_draw_rrect(SkiaCanvas c, double x, double y, double w, double h,
                             double rx, double ry, SkiaPaint p) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (!cv || !paint || !cv->surface) return;
    float r = fminf(rx, ry);

    if (paint->style == 0 || paint->style == 2) {
        aa_fill_rrect(cv->surface, cv, x, y, w, h, r, paint);
    }
    if (paint->style == 1 || paint->style == 2) {
        aa_stroke_rrect(cv->surface, cv, x, y, w, h, r, paint);
    }
}

void skia_canvas_draw_circle(SkiaCanvas c, double cx, double cy, double r,
                             SkiaPaint p) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (!cv || !paint || !cv->surface) return;

    if (paint->style == 0 || paint->style == 2) {
        aa_fill_circle(cv->surface, cv, cx, cy, r, paint);
    }
    if (paint->style == 1 || paint->style == 2) {
        /* Stroke circle as stroke of rrect with radius = r */
        aa_stroke_rrect(cv->surface, cv, cx - r, cy - r, r * 2, r * 2, r, paint);
    }
}

void skia_canvas_draw_oval(SkiaCanvas c, double x, double y, double w, double h,
                           SkiaPaint p) {
    /* Approximate oval as rounded rect with full corner radius */
    float rx = w * 0.5f, ry = h * 0.5f;
    skia_canvas_draw_rrect(c, x, y, w, h, rx, ry, p);
}

void skia_canvas_draw_line(SkiaCanvas c, double x1, double y1, double x2, double y2,
                           SkiaPaint p) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (!cv || !paint) return;
    COLORREF cr = argb_to_cr(paint->color);
    int w = (int)(paint->stroke_width + 0.5f);
    if (w < 1) w = 1;
    HPEN pen = CreatePen(PS_SOLID, w, cr);
    HPEN old = (HPEN)SelectObject(cv->hdc, pen);
    MoveToEx(cv->hdc, dev_x(cv, x1), dev_y(cv, y1), NULL);
    LineTo(cv->hdc, dev_x(cv, x2), dev_y(cv, y2));
    SelectObject(cv->hdc, old);
    DeleteObject(pen);
}

void skia_canvas_draw_path(SkiaCanvas c, SkiaPath path, SkiaPaint p) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    GdiPathImpl* gp = (GdiPathImpl*)path;
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (!cv || !gp || !paint || gp->count == 0) return;

    PaintState ps = select_paint(cv->hdc, paint);
    BeginPath(cv->hdc);
    for (int i = 0; i < gp->count; i++) {
        PathCmd* cmd = &gp->cmds[i];
        switch (cmd->op) {
            case PATH_MOVE:
                MoveToEx(cv->hdc, dev_x(cv, cmd->pts[0]), dev_y(cv, cmd->pts[1]), NULL);
                break;
            case PATH_LINE:
                LineTo(cv->hdc, dev_x(cv, cmd->pts[0]), dev_y(cv, cmd->pts[1]));
                break;
            case PATH_QUAD: {
                /* Approximate quadratic bezier as cubic */
                POINT pts[3];
                pts[0].x = dev_x(cv, cmd->pts[0]); pts[0].y = dev_y(cv, cmd->pts[1]);
                pts[1].x = dev_x(cv, cmd->pts[0]); pts[1].y = dev_y(cv, cmd->pts[1]);
                pts[2].x = dev_x(cv, cmd->pts[2]); pts[2].y = dev_y(cv, cmd->pts[3]);
                PolyBezierTo(cv->hdc, pts, 3);
                break;
            }
            case PATH_CUBIC: {
                POINT pts[3];
                pts[0].x = dev_x(cv, cmd->pts[0]); pts[0].y = dev_y(cv, cmd->pts[1]);
                pts[1].x = dev_x(cv, cmd->pts[2]); pts[1].y = dev_y(cv, cmd->pts[3]);
                pts[2].x = dev_x(cv, cmd->pts[4]); pts[2].y = dev_y(cv, cmd->pts[5]);
                PolyBezierTo(cv->hdc, pts, 3);
                break;
            }
            case PATH_CLOSE:
                CloseFigure(cv->hdc);
                break;
        }
    }
    EndPath(cv->hdc);

    if (paint->style == 0) FillPath(cv->hdc);
    else if (paint->style == 1) StrokePath(cv->hdc);
    else StrokeAndFillPath(cv->hdc);

    restore_paint(cv->hdc, &ps);
}

/* ========================================================================
 * Canvas — Text
 * ======================================================================== */

void skia_canvas_draw_text(SkiaCanvas c, const char* text, double x, double y,
                           SkiaFont font, SkiaPaint p) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    GdiFontImpl* f = (GdiFontImpl*)font;
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (!cv || !f || !text) return;

    HFONT old_font = (HFONT)SelectObject(cv->hdc, f->hfont);
    SetTextColor(cv->hdc, argb_to_cr(paint ? paint->color : 0xFF000000));
    /* Skia y = baseline; GDI y = top of cell */
    int gdi_y = dev_y(cv, y) - f->tm.tmAscent;
    TextOutA(cv->hdc, dev_x(cv, x), gdi_y, text, (int)strlen(text));
    SelectObject(cv->hdc, old_font);
}

void skia_canvas_draw_text_blob(SkiaCanvas c, SkiaTextBlob blob, float x, float y,
                                 SkiaPaint p) {
    GdiTextBlobImpl* tb = (GdiTextBlobImpl*)blob;
    if (!tb || !tb->text) return;
    skia_canvas_draw_text(c, tb->text, x, y, (SkiaFont)tb->font, p);
}

/* ========================================================================
 * Canvas — Image (stubs)
 * ======================================================================== */

void skia_canvas_draw_image(SkiaCanvas c, SkiaImage img, double x, double y,
                            SkiaPaint p) {
    GdiImageImpl* im = (GdiImageImpl*)img;
    if (!im) return;
    skia_canvas_draw_image_rect(c, img, 0, 0, (float)im->width, (float)im->height,
                                 x, y, (float)im->width, (float)im->height, p);
}

void skia_canvas_draw_image_rect(SkiaCanvas c, SkiaImage img,
                                  float sx, float sy, float sw, float sh,
                                  float dx, float dy, float dw, float dh,
                                  SkiaPaint p) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    GdiImageImpl* im = (GdiImageImpl*)img;
    if (!cv || !im || !im->pixels || !cv->surface) return;
    (void)p;

    GdiFlush();
    uint32_t* dest = (uint32_t*)cv->surface->bits;
    int dest_stride = cv->surface->width;

    /* Device-space destination rect */
    float ddx = dev_xf(cv, dx), ddy = dev_yf(cv, dy);
    float ddw = dev_wf(cv, dw), ddh = dev_hf(cv, dh);

    int x0 = (int)floorf(ddx); if (x0 < 0) x0 = 0;
    int y0 = (int)floorf(ddy); if (y0 < 0) y0 = 0;
    int x1 = (int)ceilf(ddx + ddw); if (x1 > cv->surface->width) x1 = cv->surface->width;
    int y1 = (int)ceilf(ddy + ddh); if (y1 > cv->surface->height) y1 = cv->surface->height;

    for (int py = y0; py < y1; py++) {
        float v = (py - ddy) / ddh;  /* 0..1 within dest rect */
        int src_y = (int)(sy + v * sh);
        if (src_y < 0 || src_y >= im->height) continue;

        for (int px = x0; px < x1; px++) {
            float u = (px - ddx) / ddw;
            int src_x = (int)(sx + u * sw);
            if (src_x < 0 || src_x >= im->width) continue;

            uint32_t src_pixel = im->pixels[src_y * im->width + src_x];
            int sa = (src_pixel >> 24) & 0xFF;
            if (sa >= 255) {
                dest[py * dest_stride + px] = src_pixel;
            } else if (sa > 0) {
                dest[py * dest_stride + px] = blend_over(
                    dest[py * dest_stride + px], src_pixel, sa);
            }
        }
    }
}

/* ========================================================================
 * Canvas — Transform Stack
 * ======================================================================== */

void skia_canvas_save(SkiaCanvas c) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    if (!cv || cv->save_count >= 64) return;
    CanvasState* st = &cv->states[cv->save_count];
    st->tx = cv->tx; st->ty = cv->ty;
    st->sx = cv->sx; st->sy = cv->sy;
    st->dc_save_id = SaveDC(cv->hdc);
    cv->save_count++;
}

void skia_canvas_restore(SkiaCanvas c) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    if (!cv || cv->save_count <= 0) return;
    cv->save_count--;
    CanvasState* st = &cv->states[cv->save_count];
    cv->tx = st->tx; cv->ty = st->ty;
    cv->sx = st->sx; cv->sy = st->sy;
    RestoreDC(cv->hdc, st->dc_save_id);
}

int skia_canvas_get_save_count(SkiaCanvas c) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    return cv ? cv->save_count : 0;
}

void skia_canvas_restore_to_count(SkiaCanvas c, int count) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    if (!cv) return;
    while (cv->save_count > count) skia_canvas_restore(c);
}

void skia_canvas_translate(SkiaCanvas c, double dx, double dy) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    if (!cv) return;
    cv->tx += dx * cv->sx;
    cv->ty += dy * cv->sy;
}

void skia_canvas_scale(SkiaCanvas c, double sx, double sy) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    if (!cv) return;
    cv->sx *= sx;
    cv->sy *= sy;
}

void skia_canvas_rotate(SkiaCanvas c, double degrees) {
    (void)c; (void)degrees;
    /* Rotation not supported in GDI manual-transform mode.
     * UI framework rarely uses rotation, so this is acceptable. */
}

/* ========================================================================
 * Canvas — Clipping
 * ======================================================================== */

void skia_canvas_clip_rect(SkiaCanvas c, double x, double y, double w, double h) {
    GdiCanvasImpl* cv = (GdiCanvasImpl*)c;
    if (!cv) return;
    int x0 = dev_x(cv, x), y0 = dev_y(cv, y);
    IntersectClipRect(cv->hdc, x0, y0, x0 + dev_w(cv, w), y0 + dev_h(cv, h));
}

void skia_canvas_clip_rrect(SkiaCanvas c, float x, float y, float w, float h,
                             float rx, float ry) {
    /* Approximate as rect clip */
    (void)rx; (void)ry;
    skia_canvas_clip_rect(c, x, y, w, h);
}

void skia_canvas_clip_path(SkiaCanvas c, SkiaPath path) {
    (void)c; (void)path; /* stub */
}

/* ========================================================================
 * Paint
 * ======================================================================== */

SkiaPaint skia_paint_create(void) {
    GdiPaintImpl* p = (GdiPaintImpl*)calloc(1, sizeof(GdiPaintImpl));
    if (!p) return NULL;
    p->color = 0xFF000000;
    p->style = 0;
    p->stroke_width = 1.0f;
    p->alpha = 255;
    return (SkiaPaint)p;
}

SkiaPaint skia_paint_clone(SkiaPaint paint) {
    GdiPaintImpl* src = (GdiPaintImpl*)paint;
    if (!src) return NULL;
    GdiPaintImpl* dst = (GdiPaintImpl*)malloc(sizeof(GdiPaintImpl));
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(GdiPaintImpl));
    return (SkiaPaint)dst;
}

void skia_paint_destroy(SkiaPaint p) {
    free(p);
}

void skia_paint_set_color(SkiaPaint p, uint32_t argb) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) { paint->color = argb; paint->alpha = (argb >> 24) & 0xFF; }
}

void skia_paint_set_alpha(SkiaPaint p, int alpha) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) {
        paint->alpha = alpha & 0xFF;
        paint->color = (paint->color & 0x00FFFFFF) | ((uint32_t)(alpha & 0xFF) << 24);
    }
}

void skia_paint_set_antialias(SkiaPaint p, int aa) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) paint->antialias = aa;
}

void skia_paint_set_style(SkiaPaint p, int style) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) paint->style = style;
}

void skia_paint_set_stroke_width(SkiaPaint p, double w) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) paint->stroke_width = w;
}

void skia_paint_set_stroke_cap(SkiaPaint p, int cap) {
    (void)p; (void)cap; /* GDI pen caps set at creation; ignored here */
}

void skia_paint_set_stroke_join(SkiaPaint p, int join) {
    (void)p; (void)join;
}

void skia_paint_set_shader(SkiaPaint p, SkiaShader shader) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) paint->shader = shader;
}

void skia_paint_set_blur(SkiaPaint p, float sigma) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) paint->blur_sigma = sigma;
}

void skia_paint_clear_blur(SkiaPaint p) {
    GdiPaintImpl* paint = (GdiPaintImpl*)p;
    if (paint) paint->blur_sigma = 0.0f;
}

/* ========================================================================
 * Path
 * ======================================================================== */

SkiaPath skia_path_create(void) {
    GdiPathImpl* p = (GdiPathImpl*)calloc(1, sizeof(GdiPathImpl));
    return (SkiaPath)p;
}

SkiaPath skia_path_clone(SkiaPath path) {
    GdiPathImpl* src = (GdiPathImpl*)path;
    if (!src) return NULL;
    GdiPathImpl* dst = (GdiPathImpl*)calloc(1, sizeof(GdiPathImpl));
    if (!dst) return NULL;
    if (src->count > 0) {
        dst->cmds = (PathCmd*)malloc((size_t)src->count * sizeof(PathCmd));
        memcpy(dst->cmds, src->cmds, (size_t)src->count * sizeof(PathCmd));
        dst->count = dst->capacity = src->count;
    }
    return (SkiaPath)dst;
}

void skia_path_destroy(SkiaPath path) {
    GdiPathImpl* p = (GdiPathImpl*)path;
    if (!p) return;
    free(p->cmds);
    free(p);
}

void skia_path_reset(SkiaPath path) {
    GdiPathImpl* p = (GdiPathImpl*)path;
    if (p) p->count = 0;
}

void skia_path_move_to(SkiaPath path, double x, double y) {
    GdiPathImpl* p = (GdiPathImpl*)path;
    if (p) path_push(p, PATH_MOVE, x, y, 0, 0, 0, 0);
}

void skia_path_line_to(SkiaPath path, double x, double y) {
    GdiPathImpl* p = (GdiPathImpl*)path;
    if (p) path_push(p, PATH_LINE, x, y, 0, 0, 0, 0);
}

void skia_path_quad_to(SkiaPath path, double cx, double cy, double x, double y) {
    GdiPathImpl* p = (GdiPathImpl*)path;
    if (p) path_push(p, PATH_QUAD, cx, cy, x, y, 0, 0);
}

void skia_path_cubic_to(SkiaPath path, double c1x, double c1y,
                         double c2x, double c2y, double x, double y) {
    GdiPathImpl* p = (GdiPathImpl*)path;
    if (p) path_push(p, PATH_CUBIC, c1x, c1y, c2x, c2y, x, y);
}

void skia_path_arc_to(SkiaPath path, float rx, float ry, float angle,
                       int large_arc, int sweep, float x, float y) {
    /* Approximate arc as line to endpoint */
    (void)rx; (void)ry; (void)angle; (void)large_arc; (void)sweep;
    skia_path_line_to(path, x, y);
}

void skia_path_close(SkiaPath path) {
    GdiPathImpl* p = (GdiPathImpl*)path;
    if (p) path_push(p, PATH_CLOSE, 0, 0, 0, 0, 0, 0);
}

void skia_path_add_rect(SkiaPath path, double x, double y, double w, double h) {
    skia_path_move_to(path, x, y);
    skia_path_line_to(path, x + w, y);
    skia_path_line_to(path, x + w, y + h);
    skia_path_line_to(path, x, y + h);
    skia_path_close(path);
}

void skia_path_add_rrect(SkiaPath path, double x, double y, double w, double h,
                          double rx, double ry) {
    /* Approximate rounded rect as regular rect in path */
    (void)rx; (void)ry;
    skia_path_add_rect(path, x, y, w, h);
}

void skia_path_add_circle(SkiaPath path, double cx, double cy, double r) {
    /* Approximate circle with 4 cubic bezier segments */
    float k = 0.5522847498f * r;
    skia_path_move_to(path, cx + r, cy);
    skia_path_cubic_to(path, cx + r, cy + k, cx + k, cy + r, cx, cy + r);
    skia_path_cubic_to(path, cx - k, cy + r, cx - r, cy + k, cx - r, cy);
    skia_path_cubic_to(path, cx - r, cy - k, cx - k, cy - r, cx, cy - r);
    skia_path_cubic_to(path, cx + k, cy - r, cx + r, cy - k, cx + r, cy);
    skia_path_close(path);
}

void skia_path_add_oval(SkiaPath path, float x, float y, float w, float h) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float rx = w * 0.5f, ry = h * 0.5f;
    float kx = 0.5522847498f * rx, ky = 0.5522847498f * ry;
    skia_path_move_to(path, cx + rx, cy);
    skia_path_cubic_to(path, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    skia_path_cubic_to(path, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    skia_path_cubic_to(path, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    skia_path_cubic_to(path, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    skia_path_close(path);
}

int skia_path_contains(SkiaPath path, float x, float y) {
    (void)path; (void)x; (void)y;
    return 0; /* stub */
}

/* ========================================================================
 * Font & Text
 * ======================================================================== */

SkiaFont skia_font_create(const char* family, double size) {
    GdiFontImpl* f = (GdiFontImpl*)calloc(1, sizeof(GdiFontImpl));
    if (!f) return NULL;
    strncpy(f->family, family ? family : "Segoe UI", 63);
    f->size = size;
    recreate_gdi_font(f);
    return (SkiaFont)f;
}

SkiaFont skia_font_create_from_file(const char* path, double size) {
    /* GDI can load fonts with AddFontResourceEx but keep it simple */
    (void)path;
    return skia_font_create("Segoe UI", size);
}

void skia_font_set_size(SkiaFont font, double size) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    if (!f) return;
    f->size = size;
    recreate_gdi_font(f);
}

void skia_font_set_bold(SkiaFont font, int bold) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    if (!f) return;
    f->bold = bold;
    recreate_gdi_font(f);
}

void skia_font_set_italic(SkiaFont font, int italic) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    if (!f) return;
    f->italic = italic;
    recreate_gdi_font(f);
}

void skia_font_destroy(SkiaFont font) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    if (!f) return;
    if (f->hfont) DeleteObject(f->hfont);
    free(f);
}

double skia_font_measure_text(SkiaFont font, const char* text) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    if (!f || !text || !text[0]) return 0.0f;
    HDC hdc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, f->hfont);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
    return (float)sz.cx;
}

double skia_font_get_height(SkiaFont font) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    return f ? (float)f->tm.tmHeight : 0.0f;
}

float skia_font_get_ascent(SkiaFont font) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    return f ? -(float)f->tm.tmAscent : 0.0f; /* Skia convention: negative */
}

float skia_font_get_descent(SkiaFont font) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    return f ? (float)f->tm.tmDescent : 0.0f;
}

float skia_font_get_leading(SkiaFont font) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    return f ? (float)f->tm.tmExternalLeading : 0.0f;
}

void skia_font_get_char_widths(SkiaFont font, const char* text, float* widths) {
    GdiFontImpl* f = (GdiFontImpl*)font;
    if (!f || !text || !widths) return;
    int len = (int)strlen(text);
    HDC hdc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, f->hfont);
    for (int i = 0; i < len; i++) {
        SIZE sz;
        GetTextExtentPoint32A(hdc, text + i, 1, &sz);
        widths[i] = (float)sz.cx;
    }
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
}

SkiaTextBlob skia_text_blob_make(const char* text, SkiaFont font) {
    GdiTextBlobImpl* tb = (GdiTextBlobImpl*)calloc(1, sizeof(GdiTextBlobImpl));
    if (!tb) return NULL;
    tb->text = _strdup(text ? text : "");
    tb->font = font;
    return (SkiaTextBlob)tb;
}

void skia_text_blob_destroy(SkiaTextBlob blob) {
    GdiTextBlobImpl* tb = (GdiTextBlobImpl*)blob;
    if (!tb) return;
    free(tb->text);
    free(tb);
}

/* ========================================================================
 * Image — BMP loading via Win32 + DIB pixel access
 * ======================================================================== */

SkiaImage skia_image_load_from_file(const char* path) {
    if (!path) return NULL;

    /* Load BMP/ICO/CUR via Win32 LoadImage */
    HBITMAP hbm = (HBITMAP)LoadImageA(NULL, path, IMAGE_BITMAP, 0, 0,
                                       LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!hbm) return NULL;

    BITMAP bm;
    GetObject(hbm, sizeof(bm), &bm);

    GdiImageImpl* img = (GdiImageImpl*)calloc(1, sizeof(GdiImageImpl));
    img->width = bm.bmWidth;
    img->height = bm.bmHeight;
    img->pixels = (uint32_t*)malloc((size_t)(bm.bmWidth * bm.bmHeight) * 4);

    /* Copy pixels into our ARGB buffer via GetDIBits */
    HDC hdc = GetDC(NULL);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight; /* Top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(hdc, hbm, 0, bm.bmHeight, img->pixels, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hdc);
    DeleteObject(hbm);

    /* Set alpha to 0xFF for opaque (BMP has no alpha channel) */
    int count = img->width * img->height;
    for (int i = 0; i < count; i++) {
        img->pixels[i] |= 0xFF000000;
    }

    return (SkiaImage)img;
}

SkiaImage skia_image_load_from_memory(const void* data, int size) {
    (void)data; (void)size;
    return NULL; /* Memory loading requires parsing BMP header; not yet implemented */
}

void skia_image_destroy(SkiaImage img) {
    GdiImageImpl* im = (GdiImageImpl*)img;
    if (!im) return;
    free(im->pixels);
    free(im);
}

int skia_image_get_width(SkiaImage img) {
    GdiImageImpl* im = (GdiImageImpl*)img;
    return im ? im->width : 0;
}

int skia_image_get_height(SkiaImage img) {
    GdiImageImpl* im = (GdiImageImpl*)img;
    return im ? im->height : 0;
}

/* ========================================================================
 * Shader — Gradients
 * ======================================================================== */

SkiaShader skia_shader_linear_gradient(float x0, float y0, float x1, float y1,
                                        const uint32_t* colors,
                                        const float* positions, int count) {
    GdiShaderImpl* s = (GdiShaderImpl*)calloc(1, sizeof(GdiShaderImpl));
    if (!s) return NULL;
    s->type = 0;
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1;
    s->count = count;
    s->colors = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    memcpy(s->colors, colors, (size_t)count * sizeof(uint32_t));
    if (positions) {
        s->positions = (float*)malloc((size_t)count * sizeof(float));
        memcpy(s->positions, positions, (size_t)count * sizeof(float));
    }
    return (SkiaShader)s;
}

SkiaShader skia_shader_radial_gradient(float cx, float cy, float radius,
                                        const uint32_t* colors,
                                        const float* positions, int count) {
    GdiShaderImpl* s = (GdiShaderImpl*)calloc(1, sizeof(GdiShaderImpl));
    if (!s) return NULL;
    s->type = 1;
    s->x0 = cx; s->y0 = cy; s->radius = radius;
    s->count = count;
    s->colors = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    memcpy(s->colors, colors, (size_t)count * sizeof(uint32_t));
    if (positions) {
        s->positions = (float*)malloc((size_t)count * sizeof(float));
        memcpy(s->positions, positions, (size_t)count * sizeof(float));
    }
    return (SkiaShader)s;
}

SkiaShader skia_shader_sweep_gradient(float cx, float cy,
                                       const uint32_t* colors,
                                       const float* positions, int count) {
    (void)cx; (void)cy; (void)colors; (void)positions; (void)count;
    return NULL; /* stub */
}

void skia_shader_destroy(SkiaShader shader) {
    GdiShaderImpl* s = (GdiShaderImpl*)shader;
    if (!s) return;
    free(s->colors);
    free(s->positions);
    free(s);
}

#endif /* _WIN32 */
