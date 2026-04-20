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

/* Modern default palette */
#define SG_COLOR_PRIMARY         0xFF3B82F6
#define SG_COLOR_PRIMARY_DARK    0xFF2563EB
#define SG_COLOR_PRIMARY_SOFT    0xFFEFF6FF
#define SG_COLOR_ACCENT          0xFF14B8A6
#define SG_COLOR_SUCCESS         0xFF10B981
#define SG_COLOR_ERROR           0xFFEF4444
#define SG_COLOR_WARNING         0xFFF59E0B
#define SG_COLOR_INFO            0xFF0EA5E9

/* Neutral colors */
#define SG_COLOR_BACKGROUND      0xFFF3F6FA
#define SG_COLOR_SURFACE         0xFFFFFFFF
#define SG_COLOR_SURFACE_ALT     0xFFF8FBFE
#define SG_COLOR_TEXT            0xFF1F2937
#define SG_COLOR_TEXT_SECONDARY  0xFF6B7280
#define SG_COLOR_TEXT_SOFT       0xFF94A3B8
#define SG_COLOR_TEXT_ON_DARK    0xFFF8FAFC
#define SG_COLOR_DIVIDER         0xFFD6DEE8
#define SG_COLOR_BORDER          0xFFD6DEE8
#define SG_COLOR_BORDER_STRONG   0xFFB8C5D6
#define SG_COLOR_DISABLED        0xFF94A3B8
#define SG_COLOR_DISABLED_SURFACE 0xFFE2E8F0
#define SG_COLOR_FOCUS           0xFF60A5FA
#define SG_COLOR_SIDEBAR         0xFF1F2937
#define SG_COLOR_SIDEBAR_ALT     0xFF111827

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

/* Application root shell */
void sg_style_app_root(SGNode* node);

/* Panel surface: bright card-like container */
void sg_style_panel(SGNode* node);

/* Alternate panel surface for subtle contrast */
void sg_style_panel_alt(SGNode* node);

/* Toolbar surface */
void sg_style_toolbar(SGNode* node);

/* Dark navigation/sidebar surface */
void sg_style_sidebar(SGNode* node);

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
