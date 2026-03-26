/*
 * Skia C Binding Implementation
 *
 * Wraps Skia C++ API into the C interface defined in skia_c.h.
 * Compiled as C++ and linked into the runtime.
 */

#include "skia_c.h"

#include "include/core/SkSurface.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkImage.h"
#include "include/core/SkData.h"
#include "include/core/SkStream.h"
#include "include/core/SkRRect.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkString.h"
#include "include/effects/SkGradientShader.h"

/* Platform-specific font manager */
#ifdef _WIN32
#include "include/ports/SkTypeface_win.h"
#endif

#include <cstring>
#include <cstdio>

/* ========================================================================
 * Platform Font Manager
 * ======================================================================== */

static sk_sp<SkFontMgr> get_font_mgr() {
    static sk_sp<SkFontMgr> mgr;
    if (!mgr) {
#ifdef _WIN32
        mgr = SkFontMgr_New_GDI();
#else
        mgr = SkFontMgr::RefEmpty();
#endif
    }
    return mgr;
}

/* ========================================================================
 * Helper Macros — cast opaque handles to Skia types
 * ======================================================================== */

#define AS_CANVAS(h)   reinterpret_cast<SkCanvas*>(h)
#define AS_PAINT(h)    reinterpret_cast<SkPaint*>(h)
#define AS_PATH(h)     reinterpret_cast<SkPath*>(h)
#define AS_SURFACE(h)  reinterpret_cast<SkSurface*>(h)
#define AS_FONT(h)     reinterpret_cast<SkFont*>(h)
#define AS_IMAGE(h)    reinterpret_cast<SkImage*>(h)
#define AS_SHADER(h)   reinterpret_cast<SkShader*>(h)
#define AS_TEXTBLOB(h) reinterpret_cast<SkTextBlob*>(h)

/* ========================================================================
 * Color
 * ======================================================================== */

extern "C" uint32_t skia_color_rgba(int r, int g, int b, int a) {
    return SkColorSetARGB(a, r, g, b);
}

extern "C" uint32_t skia_color_rgb(int r, int g, int b) {
    return SkColorSetARGB(255, r, g, b);
}

/* ========================================================================
 * Surface
 * ======================================================================== */

extern "C" SkiaSurface skia_surface_make_raster(int width, int height) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return nullptr;
    return reinterpret_cast<void*>(surface.release());
}

extern "C" SkiaSurface skia_surface_make_raster_direct(int width, int height,
                                                        void* pixels, int row_bytes) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, row_bytes);
    if (!surface) return nullptr;
    return reinterpret_cast<void*>(surface.release());
}

extern "C" void skia_surface_destroy(SkiaSurface surface) {
    if (surface) AS_SURFACE(surface)->unref();
}

extern "C" SkiaCanvas skia_surface_get_canvas(SkiaSurface surface) {
    if (!surface) return nullptr;
    return reinterpret_cast<void*>(AS_SURFACE(surface)->getCanvas());
}

extern "C" void* skia_surface_peek_pixels(SkiaSurface surface, int* row_bytes) {
    if (!surface) return nullptr;
    SkPixmap pixmap;
    if (!AS_SURFACE(surface)->peekPixels(&pixmap)) return nullptr;
    if (row_bytes) *row_bytes = (int)pixmap.rowBytes();
    return const_cast<void*>(pixmap.addr());
}

extern "C" SkiaImage skia_surface_snapshot(SkiaSurface surface) {
    if (!surface) return nullptr;
    sk_sp<SkImage> img = AS_SURFACE(surface)->makeImageSnapshot();
    return reinterpret_cast<void*>(img.release());
}

extern "C" int skia_surface_get_width(SkiaSurface surface) {
    return surface ? AS_SURFACE(surface)->width() : 0;
}

extern "C" int skia_surface_get_height(SkiaSurface surface) {
    return surface ? AS_SURFACE(surface)->height() : 0;
}

/* ========================================================================
 * Canvas
 * ======================================================================== */

extern "C" void skia_canvas_clear(SkiaCanvas c, uint32_t color) {
    if (c) AS_CANVAS(c)->clear(color);
}

extern "C" void skia_canvas_draw_rect(SkiaCanvas c, double x, double y,
                                       double w, double h, SkiaPaint p) {
    if (!c || !p) return;
    AS_CANVAS(c)->drawRect(SkRect::MakeXYWH(x, y, w, h), *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_rrect(SkiaCanvas c, double x, double y,
                                        double w, double h, double rx, double ry,
                                        SkiaPaint p) {
    if (!c || !p) return;
    SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry);
    AS_CANVAS(c)->drawRRect(rrect, *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_circle(SkiaCanvas c, double cx, double cy,
                                         double r, SkiaPaint p) {
    if (!c || !p) return;
    AS_CANVAS(c)->drawCircle(cx, cy, r, *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_oval(SkiaCanvas c, double x, double y,
                                       double w, double h, SkiaPaint p) {
    if (!c || !p) return;
    AS_CANVAS(c)->drawOval(SkRect::MakeXYWH(x, y, w, h), *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_line(SkiaCanvas c, double x1, double y1,
                                       double x2, double y2, SkiaPaint p) {
    if (!c || !p) return;
    AS_CANVAS(c)->drawLine(x1, y1, x2, y2, *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_path(SkiaCanvas c, SkiaPath path, SkiaPaint p) {
    if (!c || !path || !p) return;
    AS_CANVAS(c)->drawPath(*AS_PATH(path), *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_text(SkiaCanvas c, const char* text,
                                       double x, double y, SkiaFont font,
                                       SkiaPaint p) {
    if (!c || !text || !font || !p) return;
    size_t len = strlen(text);
    AS_CANVAS(c)->drawSimpleText(text, len, SkTextEncoding::kUTF8,
                                  x, y, *AS_FONT(font), *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_text_blob(SkiaCanvas c, SkiaTextBlob blob,
                                            float x, float y, SkiaPaint p) {
    if (!c || !blob || !p) return;
    AS_CANVAS(c)->drawTextBlob(sk_ref_sp(AS_TEXTBLOB(blob)), x, y, *AS_PAINT(p));
}

extern "C" void skia_canvas_draw_image(SkiaCanvas c, SkiaImage img,
                                        double x, double y, SkiaPaint p) {
    if (!c || !img) return;
    SkSamplingOptions sampling(SkFilterMode::kLinear);
    AS_CANVAS(c)->drawImage(sk_ref_sp(AS_IMAGE(img)), x, y, sampling,
                             p ? AS_PAINT(p) : nullptr);
}

extern "C" void skia_canvas_draw_image_rect(SkiaCanvas c, SkiaImage img,
                                             float sx, float sy, float sw, float sh,
                                             float dx, float dy, float dw, float dh,
                                             SkiaPaint p) {
    if (!c || !img) return;
    SkRect src = SkRect::MakeXYWH(sx, sy, sw, sh);
    SkRect dst = SkRect::MakeXYWH(dx, dy, dw, dh);
    SkSamplingOptions sampling(SkFilterMode::kLinear);
    AS_CANVAS(c)->drawImageRect(sk_ref_sp(AS_IMAGE(img)), src, dst,
                                 sampling, p ? AS_PAINT(p) : nullptr,
                                 SkCanvas::kStrict_SrcRectConstraint);
}

extern "C" void skia_canvas_save(SkiaCanvas c) {
    if (c) AS_CANVAS(c)->save();
}

extern "C" void skia_canvas_restore(SkiaCanvas c) {
    if (c) AS_CANVAS(c)->restore();
}

extern "C" int skia_canvas_get_save_count(SkiaCanvas c) {
    return c ? AS_CANVAS(c)->getSaveCount() : 0;
}

extern "C" void skia_canvas_restore_to_count(SkiaCanvas c, int count) {
    if (c) AS_CANVAS(c)->restoreToCount(count);
}

extern "C" void skia_canvas_translate(SkiaCanvas c, double dx, double dy) {
    if (c) AS_CANVAS(c)->translate(dx, dy);
}

extern "C" void skia_canvas_scale(SkiaCanvas c, double sx, double sy) {
    if (c) AS_CANVAS(c)->scale(sx, sy);
}

extern "C" void skia_canvas_rotate(SkiaCanvas c, double degrees) {
    if (c) AS_CANVAS(c)->rotate(degrees);
}

extern "C" void skia_canvas_clip_rect(SkiaCanvas c, double x, double y,
                                       double w, double h) {
    if (c) AS_CANVAS(c)->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

extern "C" void skia_canvas_clip_rrect(SkiaCanvas c, float x, float y,
                                        float w, float h, float rx, float ry) {
    if (!c) return;
    SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry);
    AS_CANVAS(c)->clipRRect(rrect);
}

extern "C" void skia_canvas_clip_path(SkiaCanvas c, SkiaPath path) {
    if (c && path) AS_CANVAS(c)->clipPath(*AS_PATH(path));
}

/* ========================================================================
 * Paint
 * ======================================================================== */

extern "C" SkiaPaint skia_paint_create(void) {
    SkPaint* p = new SkPaint();
    p->setAntiAlias(true);
    return reinterpret_cast<void*>(p);
}

extern "C" SkiaPaint skia_paint_clone(SkiaPaint p) {
    if (!p) return nullptr;
    return reinterpret_cast<void*>(new SkPaint(*AS_PAINT(p)));
}

extern "C" void skia_paint_destroy(SkiaPaint p) {
    delete AS_PAINT(p);
}

extern "C" void skia_paint_set_color(SkiaPaint p, uint32_t argb) {
    if (p) AS_PAINT(p)->setColor(argb);
}

extern "C" void skia_paint_set_alpha(SkiaPaint p, int alpha) {
    if (p) AS_PAINT(p)->setAlpha(alpha);
}

extern "C" void skia_paint_set_antialias(SkiaPaint p, int aa) {
    if (p) AS_PAINT(p)->setAntiAlias(aa != 0);
}

extern "C" void skia_paint_set_style(SkiaPaint p, int style) {
    if (!p) return;
    switch (style) {
        case 0: AS_PAINT(p)->setStyle(SkPaint::kFill_Style); break;
        case 1: AS_PAINT(p)->setStyle(SkPaint::kStroke_Style); break;
        case 2: AS_PAINT(p)->setStyle(SkPaint::kStrokeAndFill_Style); break;
    }
}

extern "C" void skia_paint_set_stroke_width(SkiaPaint p, double w) {
    if (p) AS_PAINT(p)->setStrokeWidth(w);
}

extern "C" void skia_paint_set_stroke_cap(SkiaPaint p, int cap) {
    if (!p) return;
    switch (cap) {
        case 0: AS_PAINT(p)->setStrokeCap(SkPaint::kButt_Cap); break;
        case 1: AS_PAINT(p)->setStrokeCap(SkPaint::kRound_Cap); break;
        case 2: AS_PAINT(p)->setStrokeCap(SkPaint::kSquare_Cap); break;
    }
}

extern "C" void skia_paint_set_stroke_join(SkiaPaint p, int join) {
    if (!p) return;
    switch (join) {
        case 0: AS_PAINT(p)->setStrokeJoin(SkPaint::kMiter_Join); break;
        case 1: AS_PAINT(p)->setStrokeJoin(SkPaint::kRound_Join); break;
        case 2: AS_PAINT(p)->setStrokeJoin(SkPaint::kBevel_Join); break;
    }
}

extern "C" void skia_paint_set_shader(SkiaPaint p, SkiaShader shader) {
    if (!p) return;
    if (shader) {
        AS_PAINT(p)->setShader(sk_ref_sp(AS_SHADER(shader)));
    } else {
        AS_PAINT(p)->setShader(nullptr);
    }
}

extern "C" void skia_paint_set_blur(SkiaPaint p, float sigma) {
    if (!p) return;
    AS_PAINT(p)->setMaskFilter(SkMaskFilter::MakeBlur(
        SkBlurStyle::kNormal_SkBlurStyle, sigma));
}

extern "C" void skia_paint_clear_blur(SkiaPaint p) {
    if (p) AS_PAINT(p)->setMaskFilter(nullptr);
}

/* ========================================================================
 * Path
 * ======================================================================== */

extern "C" SkiaPath skia_path_create(void) {
    return reinterpret_cast<void*>(new SkPath());
}

extern "C" SkiaPath skia_path_clone(SkiaPath path) {
    if (!path) return nullptr;
    return reinterpret_cast<void*>(new SkPath(*AS_PATH(path)));
}

extern "C" void skia_path_destroy(SkiaPath path) {
    delete AS_PATH(path);
}

extern "C" void skia_path_reset(SkiaPath path) {
    if (path) AS_PATH(path)->reset();
}

extern "C" void skia_path_move_to(SkiaPath path, double x, double y) {
    if (path) AS_PATH(path)->moveTo(x, y);
}

extern "C" void skia_path_line_to(SkiaPath path, double x, double y) {
    if (path) AS_PATH(path)->lineTo(x, y);
}

extern "C" void skia_path_quad_to(SkiaPath path, double cx, double cy,
                                   double x, double y) {
    if (path) AS_PATH(path)->quadTo(cx, cy, x, y);
}

extern "C" void skia_path_cubic_to(SkiaPath path, double c1x, double c1y,
                                    double c2x, double c2y, double x, double y) {
    if (path) AS_PATH(path)->cubicTo(c1x, c1y, c2x, c2y, x, y);
}

extern "C" void skia_path_arc_to(SkiaPath path, float rx, float ry, float angle,
                                  int large_arc, int sweep, float x, float y) {
    if (!path) return;
    AS_PATH(path)->arcTo(rx, ry, angle,
                          large_arc ? SkPath::kLarge_ArcSize : SkPath::kSmall_ArcSize,
                          sweep ? SkPathDirection::kCW : SkPathDirection::kCCW,
                          x, y);
}

extern "C" void skia_path_close(SkiaPath path) {
    if (path) AS_PATH(path)->close();
}

extern "C" void skia_path_add_rect(SkiaPath path, double x, double y,
                                    double w, double h) {
    if (path) AS_PATH(path)->addRect(SkRect::MakeXYWH(x, y, w, h));
}

extern "C" void skia_path_add_rrect(SkiaPath path, double x, double y,
                                     double w, double h, double rx, double ry) {
    if (!path) return;
    SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), rx, ry);
    AS_PATH(path)->addRRect(rrect);
}

extern "C" void skia_path_add_circle(SkiaPath path, double cx, double cy, double r) {
    if (path) AS_PATH(path)->addCircle(cx, cy, r);
}

extern "C" void skia_path_add_oval(SkiaPath path, float x, float y,
                                    float w, float h) {
    if (path) AS_PATH(path)->addOval(SkRect::MakeXYWH(x, y, w, h));
}

extern "C" int skia_path_contains(SkiaPath path, float x, float y) {
    return (path && AS_PATH(path)->contains(x, y)) ? 1 : 0;
}

/* ========================================================================
 * Font & Text
 * ======================================================================== */

extern "C" SkiaFont skia_font_create(const char* family, double size) {
    sk_sp<SkFontMgr> mgr = get_font_mgr();
    sk_sp<SkTypeface> typeface;

    if (family && family[0]) {
        typeface = mgr->matchFamilyStyle(family, SkFontStyle::Normal());
    }
    if (!typeface) {
        typeface = mgr->matchFamilyStyle("Arial", SkFontStyle::Normal());
    }
    if (!typeface) {
        typeface = SkTypeface::MakeDefault();
    }

    SkFont* font = new SkFont(typeface, size);
    font->setSubpixel(true);
    font->setEdging(SkFont::Edging::kSubpixelAntiAlias);
    return reinterpret_cast<void*>(font);
}

extern "C" SkiaFont skia_font_create_from_file(const char* path, double size) {
    sk_sp<SkFontMgr> mgr = get_font_mgr();
    sk_sp<SkTypeface> typeface = mgr->makeFromFile(path);
    if (!typeface) {
        fprintf(stderr, "Failed to load font from: %s\n", path);
        return skia_font_create(nullptr, size);  /* fallback to default */
    }
    SkFont* font = new SkFont(typeface, size);
    font->setSubpixel(true);
    font->setEdging(SkFont::Edging::kSubpixelAntiAlias);
    return reinterpret_cast<void*>(font);
}

extern "C" void skia_font_set_size(SkiaFont font, double size) {
    if (font) AS_FONT(font)->setSize(size);
}

extern "C" void skia_font_set_bold(SkiaFont font, int bold) {
    if (!font) return;
    SkFont* current_font = AS_FONT(font);
    sk_sp<SkTypeface> tf = AS_FONT(font)->refTypeface();
    if (!tf) return;
    sk_sp<SkFontMgr> mgr = get_font_mgr();
    SkFontStyle style = bold
        ? SkFontStyle(SkFontStyle::kBold_Weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant)
        : SkFontStyle::Normal();
    SkString family_name;
    tf->getFamilyName(&family_name);
    sk_sp<SkTypeface> new_tf = mgr->matchFamilyStyle(family_name.c_str(), style);
    if (new_tf) {
        SkFont replacement(new_tf, current_font->getSize(), current_font->getScaleX(), current_font->getSkewX());
        replacement.setSubpixel(current_font->isSubpixel());
        replacement.setEdging(current_font->getEdging());
        replacement.setHinting(current_font->getHinting());
        replacement.setLinearMetrics(current_font->isLinearMetrics());
        replacement.setEmbeddedBitmaps(current_font->isEmbeddedBitmaps());
        replacement.setForceAutoHinting(current_font->isForceAutoHinting());
        replacement.setEmbolden(current_font->isEmbolden());
        replacement.setBaselineSnap(current_font->isBaselineSnap());
        *current_font = replacement;
    }
}

extern "C" void skia_font_set_italic(SkiaFont font, int italic) {
    if (!font) return;
    SkFont* current_font = AS_FONT(font);
    sk_sp<SkTypeface> tf = AS_FONT(font)->refTypeface();
    if (!tf) return;
    sk_sp<SkFontMgr> mgr = get_font_mgr();
    SkFontStyle style = italic
        ? SkFontStyle(SkFontStyle::kNormal_Weight, SkFontStyle::kNormal_Width, SkFontStyle::kItalic_Slant)
        : SkFontStyle::Normal();
    SkString family_name;
    tf->getFamilyName(&family_name);
    sk_sp<SkTypeface> new_tf = mgr->matchFamilyStyle(family_name.c_str(), style);
    if (new_tf) {
        SkFont replacement(new_tf, current_font->getSize(), current_font->getScaleX(), current_font->getSkewX());
        replacement.setSubpixel(current_font->isSubpixel());
        replacement.setEdging(current_font->getEdging());
        replacement.setHinting(current_font->getHinting());
        replacement.setLinearMetrics(current_font->isLinearMetrics());
        replacement.setEmbeddedBitmaps(current_font->isEmbeddedBitmaps());
        replacement.setForceAutoHinting(current_font->isForceAutoHinting());
        replacement.setEmbolden(current_font->isEmbolden());
        replacement.setBaselineSnap(current_font->isBaselineSnap());
        *current_font = replacement;
    }
}

extern "C" void skia_font_destroy(SkiaFont font) {
    delete AS_FONT(font);
}

extern "C" double skia_font_measure_text(SkiaFont font, const char* text) {
    if (!font || !text) return 0.0f;
    return AS_FONT(font)->measureText(text, strlen(text), SkTextEncoding::kUTF8);
}

extern "C" double skia_font_get_height(SkiaFont font) {
    if (!font) return 0.0f;
    SkFontMetrics metrics;
    AS_FONT(font)->getMetrics(&metrics);
    return metrics.fDescent - metrics.fAscent + metrics.fLeading;
}

extern "C" float skia_font_get_ascent(SkiaFont font) {
    if (!font) return 0.0f;
    SkFontMetrics metrics;
    AS_FONT(font)->getMetrics(&metrics);
    return metrics.fAscent;
}

extern "C" float skia_font_get_descent(SkiaFont font) {
    if (!font) return 0.0f;
    SkFontMetrics metrics;
    AS_FONT(font)->getMetrics(&metrics);
    return metrics.fDescent;
}

extern "C" float skia_font_get_leading(SkiaFont font) {
    if (!font) return 0.0f;
    SkFontMetrics metrics;
    AS_FONT(font)->getMetrics(&metrics);
    return metrics.fLeading;
}

extern "C" void skia_font_get_char_widths(SkiaFont font, const char* text,
                                           float* widths) {
    if (!font || !text || !widths) return;
    /* Measure each UTF-8 byte as a glyph position.
     * For full correctness, this should use SkShaper, but for ASCII
     * this simple approach works well. */
    const char* p = text;
    int idx = 0;
    while (*p) {
        char ch[2] = { *p, 0 };
        widths[idx] = AS_FONT(font)->measureText(ch, 1, SkTextEncoding::kUTF8);
        p++;
        idx++;
    }
}

extern "C" SkiaTextBlob skia_text_blob_make(const char* text, SkiaFont font) {
    if (!text || !font) return nullptr;
    sk_sp<SkTextBlob> blob = SkTextBlob::MakeFromString(text, *AS_FONT(font));
    return reinterpret_cast<void*>(blob.release());
}

extern "C" void skia_text_blob_destroy(SkiaTextBlob blob) {
    if (blob) AS_TEXTBLOB(blob)->unref();
}

/* ========================================================================
 * Image
 * ======================================================================== */

extern "C" SkiaImage skia_image_load_from_file(const char* path) {
    sk_sp<SkData> data = SkData::MakeFromFileName(path);
    if (!data) {
        fprintf(stderr, "Failed to read image file: %s\n", path);
        return nullptr;
    }
    sk_sp<SkImage> img = SkImages::DeferredFromEncodedData(data);
    if (!img) {
        fprintf(stderr, "Failed to decode image: %s\n", path);
        return nullptr;
    }
    return reinterpret_cast<void*>(img.release());
}

extern "C" SkiaImage skia_image_load_from_memory(const void* data, int size) {
    sk_sp<SkData> sk_data = SkData::MakeWithoutCopy(data, size);
    sk_sp<SkImage> img = SkImages::DeferredFromEncodedData(sk_data);
    return img ? reinterpret_cast<void*>(img.release()) : nullptr;
}

extern "C" void skia_image_destroy(SkiaImage img) {
    if (img) AS_IMAGE(img)->unref();
}

extern "C" int skia_image_get_width(SkiaImage img) {
    return img ? AS_IMAGE(img)->width() : 0;
}

extern "C" int skia_image_get_height(SkiaImage img) {
    return img ? AS_IMAGE(img)->height() : 0;
}

/* ========================================================================
 * Shader (Gradients)
 * ======================================================================== */

extern "C" SkiaShader skia_shader_linear_gradient(float x0, float y0,
                                                   float x1, float y1,
                                                   const uint32_t* colors,
                                                   const float* positions,
                                                   int count) {
    SkPoint pts[2] = { {x0, y0}, {x1, y1} };
    /* Convert uint32_t ARGB to SkColor array (same format) */
    sk_sp<SkShader> shader = SkGradientShader::MakeLinear(
        pts,
        reinterpret_cast<const SkColor*>(colors),
        positions, count,
        SkTileMode::kClamp
    );
    return shader ? reinterpret_cast<void*>(shader.release()) : nullptr;
}

extern "C" SkiaShader skia_shader_radial_gradient(float cx, float cy,
                                                   float radius,
                                                   const uint32_t* colors,
                                                   const float* positions,
                                                   int count) {
    sk_sp<SkShader> shader = SkGradientShader::MakeRadial(
        SkPoint::Make(cx, cy), radius,
        reinterpret_cast<const SkColor*>(colors),
        positions, count,
        SkTileMode::kClamp
    );
    return shader ? reinterpret_cast<void*>(shader.release()) : nullptr;
}

extern "C" SkiaShader skia_shader_sweep_gradient(float cx, float cy,
                                                  const uint32_t* colors,
                                                  const float* positions,
                                                  int count) {
    sk_sp<SkShader> shader = SkGradientShader::MakeSweep(
        cx, cy,
        reinterpret_cast<const SkColor*>(colors),
        positions, count
    );
    return shader ? reinterpret_cast<void*>(shader.release()) : nullptr;
}

extern "C" void skia_shader_destroy(SkiaShader shader) {
    if (shader) AS_SHADER(shader)->unref();
}
