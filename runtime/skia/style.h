/*
 * Styling System — CSS-like visual properties for Casperix UI
 *
 * Provides convenience functions for applying visual styles to scene graph
 * nodes: backgrounds, borders, shadows, gradients, opacity.
 *
 * Shadow rendering uses Skia's blur mask filter.
 * Gradient rendering creates Skia shader objects.
 */

#ifndef STYLE_H
#define STYLE_H

#include "scene_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Predefined Color Palette
 * ======================================================================== */

/* Material Design colors */
#define SG_COLOR_PRIMARY      0xFF4285F4   /* Google Blue */
#define SG_COLOR_PRIMARY_DARK 0xFF3367D6
#define SG_COLOR_ACCENT       0xFFFBBC05   /* Google Yellow */
#define SG_COLOR_SUCCESS      0xFF34A853   /* Google Green */
#define SG_COLOR_ERROR        0xFFEA4335   /* Google Red */
#define SG_COLOR_WARNING      0xFFFF9800
#define SG_COLOR_INFO         0xFF2196F3

/* Neutral colors */
#define SG_COLOR_BACKGROUND   0xFFF5F5F5
#define SG_COLOR_SURFACE      0xFFFFFFFF
#define SG_COLOR_TEXT         0xFF212121
#define SG_COLOR_TEXT_SECONDARY 0xFF757575
#define SG_COLOR_DIVIDER      0xFFBDBDBD
#define SG_COLOR_DISABLED     0xFF9E9E9E

/* ========================================================================
 * Color Utilities
 * ======================================================================== */

/* Create ARGB from components (0-255) */
uint32_t sg_color_rgba(int r, int g, int b, int a);
uint32_t sg_color_rgb(int r, int g, int b);

/* Modify color */
uint32_t sg_color_lighten(uint32_t color, int amount);
uint32_t sg_color_darken(uint32_t color, int amount);
uint32_t sg_color_with_alpha(uint32_t color, int alpha);

/* Interpolate between two colors (t = 0.0 to 1.0) */
uint32_t sg_color_lerp(uint32_t a, uint32_t b, double t);

/* ========================================================================
 * Shadow Rendering
 * ======================================================================== */

/* Render a drop shadow beneath a rounded rect.
 * Call before rendering the node's background. */
void sg_render_shadow(SkiaCanvas canvas, SGRect bounds, float radius,
                       float offset_x, float offset_y, float blur_sigma,
                       uint32_t shadow_color);

/* Material Design elevation shadows (0-24) */
void sg_render_elevation(SkiaCanvas canvas, SGRect bounds, float radius,
                          int elevation);

/* ========================================================================
 * Gradient Helpers
 * ======================================================================== */

/* Create a linear gradient shader at an angle (degrees).
 * 0=left-to-right, 90=top-to-bottom, etc. */
SkiaShader sg_gradient_linear(SGRect bounds, float angle_degrees,
                               const uint32_t* colors, const float* stops, int count);

/* Create a radial gradient from center of bounds */
SkiaShader sg_gradient_radial(SGRect bounds,
                               const uint32_t* colors, const float* stops, int count);

/* Two-color convenience gradient */
SkiaShader sg_gradient_two_color(SGRect bounds, float angle,
                                  uint32_t color1, uint32_t color2);

/* ========================================================================
 * Themed Styles (apply to nodes)
 * ======================================================================== */

/* Card: white background, rounded corners, shadow */
void sg_style_card(SGNode* node);

/* Outlined: border only, no fill */
void sg_style_outlined(SGNode* node, uint32_t border_color, double width);

/* Elevated: background + shadow based on elevation level */
void sg_style_elevated(SGNode* node, int elevation);

/* Pill: maximum rounded corners */
void sg_style_pill(SGNode* node);

/* ========================================================================
 * Rendering Helpers (used internally by scene_graph.c)
 * ======================================================================== */

/* Render a rounded rect with background, gradient, and border */
void sg_render_styled_rect(SkiaCanvas canvas, SGRect bounds, SGStyle* style);

#ifdef __cplusplus
}
#endif

#endif /* STYLE_H */
