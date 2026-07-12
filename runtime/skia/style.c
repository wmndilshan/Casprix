/*
 * Styling System — Shadow, gradient, and color rendering
 */

#include "style.h"
#include "skia_c.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sg_color_dim(uint32_t c, int delta) {
    int a = (int)((c >> 24) & 0xFF);
    int r = (int)((c >> 16) & 0xFF) + delta;
    int g = (int)((c >> 8) & 0xFF) + delta;
    int b = (int)(c & 0xFF) + delta;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void sg_theme_material3_default(SGTheme* out_theme) {
    if (!out_theme) return;
    memset(out_theme, 0, sizeof(*out_theme));
    out_theme->colors[SG_TOKEN_PRIMARY] = 0xFF6750A4;
    out_theme->colors[SG_TOKEN_ON_PRIMARY] = 0xFFFFFFFF;
    out_theme->colors[SG_TOKEN_SURFACE] = 0xFFFCFCFF;
    out_theme->colors[SG_TOKEN_ON_SURFACE] = 0xFF1C1B1F;
    out_theme->colors[SG_TOKEN_SURFACE_CONTAINER] = 0xFFF3EDF7;
    out_theme->colors[SG_TOKEN_SURFACE_CONTAINER_HIGH] = 0xFFECE6F0;
    out_theme->colors[SG_TOKEN_OUTLINE] = 0xFF79747E;
    out_theme->colors[SG_TOKEN_OUTLINE_VARIANT] = 0xFFCAC4D0;
    out_theme->colors[SG_TOKEN_ERROR] = 0xFFB3261E;

    out_theme->spacing_scale[0] = 2;
    out_theme->spacing_scale[1] = 4;
    out_theme->spacing_scale[2] = 6;
    out_theme->spacing_scale[3] = 8;
    out_theme->spacing_scale[4] = 12;
    out_theme->spacing_scale[5] = 16;
    out_theme->spacing_scale[6] = 24;
    out_theme->spacing_scale[7] = 32;
}

void sg_style_data_from_theme(SGStyleData* out, const SGTheme* theme) {
    if (!out || !theme) return;
    memset(out, 0, sizeof(*out));
    out->bg_color = theme->colors[SG_TOKEN_SURFACE];
    out->fg_color = theme->colors[SG_TOKEN_ON_SURFACE];
    out->border_color = theme->colors[SG_TOKEN_OUTLINE_VARIANT];
    out->border_width = 1.0f;
    out->radius = 12.0f;
    out->pad_top = 8.0f;
    out->pad_right = 12.0f;
    out->pad_bottom = 8.0f;
    out->pad_left = 12.0f;
    out->font = NULL;
    out->shadow_offset_y = 0.0f;
    out->shadow_blur = 0.0f;
    out->shadow_color = 0x00000000;
}

void sg_style_ref_init(SGStyleRef* ref, const SGStyleRef* parent) {
    if (!ref) return;
    memset(ref, 0, sizeof(*ref));
    ref->parent = parent;
}

void sg_style_ref_set_bg(SGStyleRef* ref, uint32_t color) {
    if (!ref) return;
    ref->data.bg_color = color;
    ref->set_mask |= SG_STYLE_SET_BG;
}

void sg_style_ref_set_fg(SGStyleRef* ref, uint32_t color) {
    if (!ref) return;
    ref->data.fg_color = color;
    ref->set_mask |= SG_STYLE_SET_FG;
}

void sg_style_ref_set_border(SGStyleRef* ref, uint32_t color, float width) {
    if (!ref) return;
    ref->data.border_color = color;
    ref->data.border_width = width;
    ref->set_mask |= SG_STYLE_SET_BORDER;
}

void sg_style_ref_set_radius(SGStyleRef* ref, float radius) {
    if (!ref) return;
    ref->data.radius = radius;
    ref->set_mask |= SG_STYLE_SET_RADIUS;
}

void sg_style_ref_set_padding(SGStyleRef* ref, float top, float right, float bottom, float left) {
    if (!ref) return;
    ref->data.pad_top = top;
    ref->data.pad_right = right;
    ref->data.pad_bottom = bottom;
    ref->data.pad_left = left;
    ref->set_mask |= SG_STYLE_SET_PADDING;
}

void sg_style_ref_set_font(SGStyleRef* ref, SkiaFont font) {
    if (!ref) return;
    ref->data.font = font;
    ref->set_mask |= SG_STYLE_SET_FONT;
}

void sg_style_ref_set_shadow(SGStyleRef* ref, float offset_y, float blur, uint32_t color) {
    if (!ref) return;
    ref->data.shadow_offset_y = offset_y;
    ref->data.shadow_blur = blur;
    ref->data.shadow_color = color;
    ref->set_mask |= SG_STYLE_SET_SHADOW;
}

void sg_style_resolve(const SGStyleRef* ref, uint32_t state_flags, SGResolvedStyle* out) {
    uint32_t needed;
    const SGStyleRef* it;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    needed = SG_STYLE_SET_BG | SG_STYLE_SET_FG | SG_STYLE_SET_BORDER |
             SG_STYLE_SET_RADIUS | SG_STYLE_SET_PADDING |
             SG_STYLE_SET_FONT | SG_STYLE_SET_SHADOW;

    for (it = ref; it && needed != 0; it = it->parent) {
        uint32_t take = it->set_mask & needed;
        if (!take) continue;

        if (take & SG_STYLE_SET_BG) out->data.bg_color = it->data.bg_color;
        if (take & SG_STYLE_SET_FG) out->data.fg_color = it->data.fg_color;
        if (take & SG_STYLE_SET_BORDER) {
            out->data.border_color = it->data.border_color;
            out->data.border_width = it->data.border_width;
        }
        if (take & SG_STYLE_SET_RADIUS) out->data.radius = it->data.radius;
        if (take & SG_STYLE_SET_PADDING) {
            out->data.pad_top = it->data.pad_top;
            out->data.pad_right = it->data.pad_right;
            out->data.pad_bottom = it->data.pad_bottom;
            out->data.pad_left = it->data.pad_left;
        }
        if (take & SG_STYLE_SET_FONT) out->data.font = it->data.font;
        if (take & SG_STYLE_SET_SHADOW) {
            out->data.shadow_offset_y = it->data.shadow_offset_y;
            out->data.shadow_blur = it->data.shadow_blur;
            out->data.shadow_color = it->data.shadow_color;
        }

        needed &= ~take;
    }

    if (state_flags & SG_STATE_DISABLED) {
        out->data.bg_color = 0xFFE6E8EB;
        out->data.fg_color = 0xFF8A9099;
    } else if (state_flags & SG_STATE_ACTIVE) {
        out->data.bg_color = sg_color_dim(out->data.bg_color, -18);
    } else if (state_flags & SG_STATE_HOVER) {
        out->data.bg_color = sg_color_dim(out->data.bg_color, 10);
    }

    out->resolved_state_flags = state_flags;
}

static void sg_style_mark_changed(SGNode* node, int affects_layout) {
    if (!node) return;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT | (affects_layout ? SG_DIRTY_LAYOUT : 0));
}

/* ========================================================================
 * Color Utilities
 * ======================================================================== */

uint32_t sg_color_rgba(int r, int g, int b, int a) {
    return ((uint32_t)(a & 0xFF) << 24) |
           ((uint32_t)(r & 0xFF) << 16) |
           ((uint32_t)(g & 0xFF) << 8)  |
           ((uint32_t)(b & 0xFF));
}

uint32_t sg_color_rgb(int r, int g, int b) {
    return sg_color_rgba(r, g, b, 255);
}

uint32_t sg_color_lighten(uint32_t color, int amount) {
    int a = (color >> 24) & 0xFF;
    int r = ((color >> 16) & 0xFF) + amount; if (r > 255) r = 255;
    int g = ((color >>  8) & 0xFF) + amount; if (g > 255) g = 255;
    int b = ((color      ) & 0xFF) + amount; if (b > 255) b = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t sg_color_darken(uint32_t color, int amount) {
    int a = (color >> 24) & 0xFF;
    int r = ((color >> 16) & 0xFF) - amount; if (r < 0) r = 0;
    int g = ((color >>  8) & 0xFF) - amount; if (g < 0) g = 0;
    int b = ((color      ) & 0xFF) - amount; if (b < 0) b = 0;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t sg_color_with_alpha(uint32_t color, int alpha) {
    return (color & 0x00FFFFFF) | ((uint32_t)(alpha & 0xFF) << 24);
}

uint32_t sg_color_lerp(uint32_t a, uint32_t b, double t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;

    int aa = (a >> 24) & 0xFF, ab = (b >> 24) & 0xFF;
    int ra = (a >> 16) & 0xFF, rb = (b >> 16) & 0xFF;
    int ga = (a >>  8) & 0xFF, gb = (b >>  8) & 0xFF;
    int ba = (a      ) & 0xFF, bb = (b      ) & 0xFF;

    int ar = (int)(aa + (ab - aa) * t);
    int rr = (int)(ra + (rb - ra) * t);
    int gr = (int)(ga + (gb - ga) * t);
    int br = (int)(ba + (bb - ba) * t);

    return (ar << 24) | (rr << 16) | (gr << 8) | br;
}

/* ========================================================================
 * Shadow Rendering
 * ======================================================================== */

/* Reusable paint for shadow rendering */
static SkiaPaint s_shadow_paint = NULL;

static SkiaPaint get_shadow_paint(void) {
    if (!s_shadow_paint) {
        s_shadow_paint = skia_paint_create();
        skia_paint_set_antialias(s_shadow_paint, 1);
        skia_paint_set_style(s_shadow_paint, 0); /* fill */
    }
    return s_shadow_paint;
}

void sg_render_shadow(SkiaCanvas canvas, SGRect bounds, float radius,
                       float offset_x, float offset_y, float blur_sigma,
                       uint32_t shadow_color) {
    if (blur_sigma <= 0) return;

    SkiaPaint paint = get_shadow_paint();
    skia_paint_set_color(paint, shadow_color);
    skia_paint_set_blur(paint, blur_sigma);

    skia_canvas_draw_rrect(canvas,
                            bounds.x + offset_x,
                            bounds.y + offset_y,
                            bounds.w, bounds.h,
                            radius, radius, paint);

    skia_paint_clear_blur(paint);
}

void sg_render_elevation(SkiaCanvas canvas, SGRect bounds, float radius,
                          int elevation) {
    if (elevation <= 0) return;

    /* Material Design shadow approximation:
     * Two shadows — ambient (soft, large) + key (directional) */

    /* Ambient shadow */
    float ambient_blur = elevation * 1.5f;
    uint32_t ambient_alpha = 12 + elevation * 2;
    if (ambient_alpha > 60) ambient_alpha = 60;
    uint32_t ambient_color = (ambient_alpha << 24);

    sg_render_shadow(canvas, bounds, radius,
                      0, elevation * 0.5f, ambient_blur, ambient_color);

    /* Key shadow (directional, from top-left light source) */
    float key_blur = elevation * 0.8f;
    float key_offset_y = elevation * 1.0f;
    uint32_t key_alpha = 20 + elevation * 3;
    if (key_alpha > 80) key_alpha = 80;
    uint32_t key_color = (key_alpha << 24);

    sg_render_shadow(canvas, bounds, radius,
                      0, key_offset_y, key_blur, key_color);
}

/* ========================================================================
 * Gradient Helpers
 * ======================================================================== */

SkiaShader sg_gradient_linear(SGRect bounds, float angle_degrees,
                               const uint32_t* colors, const float* stops, int count) {
    float rad = angle_degrees * 3.14159265f / 180.0f;
    float cx = bounds.x + bounds.w / 2.0f;
    float cy = bounds.y + bounds.h / 2.0f;

    /* Diagonal length for gradient line */
    float diag = sqrtf(bounds.w * bounds.w + bounds.h * bounds.h) / 2.0f;

    float x0 = cx - cosf(rad) * diag;
    float y0 = cy - sinf(rad) * diag;
    float x1 = cx + cosf(rad) * diag;
    float y1 = cy + sinf(rad) * diag;

    return skia_shader_linear_gradient(x0, y0, x1, y1, colors, stops, count);
}

SkiaShader sg_gradient_radial(SGRect bounds,
                               const uint32_t* colors, const float* stops, int count) {
    float cx = bounds.x + bounds.w / 2.0f;
    float cy = bounds.y + bounds.h / 2.0f;
    float radius = sqrtf(bounds.w * bounds.w + bounds.h * bounds.h) / 2.0f;

    return skia_shader_radial_gradient(cx, cy, radius, colors, stops, count);
}

SkiaShader sg_gradient_two_color(SGRect bounds, float angle,
                                  uint32_t color1, uint32_t color2) {
    uint32_t colors[2] = { color1, color2 };
    float stops[2] = { 0.0f, 1.0f };
    return sg_gradient_linear(bounds, angle, colors, stops, 2);
}

/* ========================================================================
 * Themed Styles
 * ======================================================================== */

void sg_style_app_root(SGNode* node) {
    if (!node) return;
    node->style.background = SG_COLOR_BACKGROUND;
    node->style.padding[0] = 24.0f;
    node->style.padding[1] = 24.0f;
    node->style.padding[2] = 24.0f;
    node->style.padding[3] = 24.0f;
    node->style.border_width = 0.0f;
    node->style.shadow_blur = 0.0f;
    node->style.shadow_color = 0;
    sg_style_mark_changed(node, 1);
}

void sg_style_panel(SGNode* node) {
    if (!node) return;
    node->style.background = SG_COLOR_SURFACE;
    node->style.border_color = SG_COLOR_BORDER;
    node->style.border_width = 1.0f;
    node->style.border_radius = 18.0f;
    node->style.elevation = 0;
    node->style.shadow_offset_x = 0.0f;
    node->style.shadow_offset_y = 14.0f;
    node->style.shadow_blur = 36.0f;
    node->style.shadow_color = 0x0F111827;
    sg_style_mark_changed(node, 0);
}

void sg_style_panel_alt(SGNode* node) {
    if (!node) return;
    node->style.background = SG_COLOR_SURFACE_ALT;
    node->style.border_color = SG_COLOR_BORDER;
    node->style.border_width = 1.0f;
    node->style.border_radius = 18.0f;
    node->style.elevation = 0;
    node->style.shadow_offset_x = 0.0f;
    node->style.shadow_offset_y = 10.0f;
    node->style.shadow_blur = 22.0f;
    node->style.shadow_color = 0x0C111827;
    sg_style_mark_changed(node, 0);
}

void sg_style_toolbar(SGNode* node) {
    if (!node) return;
    node->style.background = SG_COLOR_SURFACE;
    node->style.border_color = SG_COLOR_BORDER;
    node->style.border_width = 1.0f;
    node->style.border_radius = 16.0f;
    node->style.padding[0] = 18.0f;
    node->style.padding[1] = 18.0f;
    node->style.padding[2] = 18.0f;
    node->style.padding[3] = 18.0f;
    node->style.elevation = 0;
    node->style.shadow_offset_x = 0.0f;
    node->style.shadow_offset_y = 10.0f;
    node->style.shadow_blur = 28.0f;
    node->style.shadow_color = 0x0F111827;
    sg_style_mark_changed(node, 1);
}

void sg_style_sidebar(SGNode* node) {
    if (!node) return;
    node->style.background = SG_COLOR_SIDEBAR;
    node->style.border_color = SG_COLOR_SIDEBAR_ALT;
    node->style.border_width = 1.0f;
    node->style.border_radius = 24.0f;
    node->style.padding[0] = 22.0f;
    node->style.padding[1] = 22.0f;
    node->style.padding[2] = 22.0f;
    node->style.padding[3] = 22.0f;
    node->style.elevation = 0;
    node->style.shadow_offset_x = 0.0f;
    node->style.shadow_offset_y = 18.0f;
    node->style.shadow_blur = 44.0f;
    node->style.shadow_color = 0x22111827;
    sg_style_mark_changed(node, 1);
}

void sg_style_card(SGNode* node) {
    sg_style_panel(node);
    if (!node) return;
    node->style.border_radius = 20.0f;
    node->style.padding[0] = 20.0f;
    node->style.padding[1] = 20.0f;
    node->style.padding[2] = 20.0f;
    node->style.padding[3] = 20.0f;
    node->style.shadow_offset_y = 12.0f;
    node->style.shadow_blur = 26.0f;
    sg_style_mark_changed(node, 1);
}

void sg_style_outlined(SGNode* node, uint32_t border_color, double width) {
    if (!node) return;
    node->style.background = SKIA_COLOR_TRANSPARENT;
    node->style.border_color = border_color;
    node->style.border_width = width;
    node->style.border_radius = 14.0f;
    sg_style_mark_changed(node, 0);
}

void sg_style_elevated(SGNode* node, int elevation) {
    if (!node) return;
    if (node->style.background == 0) {
        node->style.background = SG_COLOR_SURFACE;
    }
    node->style.border_color = SG_COLOR_BORDER;
    node->style.border_width = 1.0f;
    node->style.elevation = 0;
    node->style.shadow_offset_y = 6.0f + (float)elevation * 2.0f;
    node->style.shadow_blur = 18.0f + (float)elevation * 4.0f;
    node->style.shadow_color = 0x140F172A;
    if (node->style.border_radius < 16.0f) {
        node->style.border_radius = 16.0f;
    }
    sg_style_mark_changed(node, 0);
}

void sg_style_pill(SGNode* node) {
    if (!node) return;
    /* Set border radius to half the node height.
     * Since height may not be known yet, use a large value
     * that gets clamped during rendering. */
    node->style.border_radius = 9999.0f;
    sg_style_mark_changed(node, 0);
}

/* ========================================================================
 * Styled Rect Rendering
 * ======================================================================== */

/* Reusable paint objects */
static SkiaPaint s_fill_paint = NULL;
static SkiaPaint s_stroke_paint = NULL;

static void ensure_paints(void) {
    if (!s_fill_paint) {
        s_fill_paint = skia_paint_create();
        skia_paint_set_antialias(s_fill_paint, 1);
        skia_paint_set_style(s_fill_paint, 0); /* fill */
    }
    if (!s_stroke_paint) {
        s_stroke_paint = skia_paint_create();
        skia_paint_set_antialias(s_stroke_paint, 1);
        skia_paint_set_style(s_stroke_paint, 1); /* stroke */
    }
}

void sg_render_styled_rect(SkiaCanvas canvas, SGRect bounds, SGStyle* style) {
    if (!canvas || !style) return;
    ensure_paints();

    float r = style->border_radius;
    /* Clamp radius to half the smaller dimension */
    float max_r = bounds.w < bounds.h ? bounds.w / 2.0f : bounds.h / 2.0f;
    if (r > max_r) r = max_r;

    /* Shadow */
    if (style->elevation > 0 || style->shadow_blur > 0) {
        if (style->shadow_color != 0) {
            sg_render_shadow(canvas, bounds, r,
                              style->shadow_offset_x, style->shadow_offset_y,
                              style->shadow_blur, style->shadow_color);
        } else if (style->elevation > 0) {
            sg_render_elevation(canvas, bounds, r, style->elevation);
        }
    }

    /* Background fill */
    if (style->gradient) {
        skia_paint_set_shader(s_fill_paint, style->gradient);
        skia_canvas_draw_rrect(canvas, bounds.x, bounds.y, bounds.w, bounds.h,
                                r, r, s_fill_paint);
        skia_paint_set_shader(s_fill_paint, NULL);
    } else if ((style->background & 0xFF000000) != 0) {
        skia_paint_set_color(s_fill_paint, style->background);
        if (style->opacity < 1.0f && style->opacity > 0.0f) {
            skia_paint_set_alpha(s_fill_paint, (int)(style->opacity * 255));
        }
        skia_canvas_draw_rrect(canvas, bounds.x, bounds.y, bounds.w, bounds.h,
                                r, r, s_fill_paint);
        skia_paint_set_alpha(s_fill_paint, 255); /* reset */
    }

    /* Border */
    if (style->border_width > 0 && (style->border_color & 0xFF000000) != 0) {
        skia_paint_set_color(s_stroke_paint, style->border_color);
        skia_paint_set_stroke_width(s_stroke_paint, style->border_width);
        skia_canvas_draw_rrect(canvas, bounds.x, bounds.y, bounds.w, bounds.h,
                                r, r, s_stroke_paint);
    }
}
