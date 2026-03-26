/**
 * Visual Effects System for Casperix UI
 * 
 * Apply post-processing effects to scene graph nodes:
 *  - Drop shadows
 *  - Blur effects
 *  - Color filters (grayscale, sepia, brightness, contrast)
 *  - Glow effects
 */

#ifndef CASPERIX_EFFECTS_H
#define CASPERIX_EFFECTS_H

#include "scene_graph.h"
#include "skia_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Shadow Effects
 * ======================================================================== */

/**
 * Apply drop shadow to a node
 * @param node Target node
 * @param offset_x Shadow horizontal offset
 * @param offset_y Shadow vertical offset  
 * @param blur Shadow blur radius
 * @param color Shadow color (ARGB)
 */
void sg_apply_drop_shadow(SGNode* node, float offset_x, float offset_y,
                          float blur, uint32_t color);

/**
 * Remove shadow effect
 */
void sg_clear_shadow(SGNode* node);

/* ========================================================================
 * Blur Effects
 * ======================================================================== */

/**
 * Apply Gaussian blur to node
 * @param node Target node
 * @param sigma Blur radius (0 = no blur, 10 = heavy blur)
 */
void sg_apply_blur(SGNode* node, float sigma);

/**
 * Clear blur effect
 */
void sg_clear_blur(SGNode* node);

/* ========================================================================
 * Color Filters
 * ======================================================================== */

/**
 * Apply grayscale filter
 * @param node Target node
 */
void sg_apply_grayscale(SGNode* node);

/**
 * Apply sepia tone filter
 * @param node Target node
 */
void sg_apply_sepia(SGNode* node);

/**
 * Apply color inversion
 * @param node Target node
 */
void sg_apply_invert(SGNode* node);

/**
 * Adjust brightness
 * @param node Target node
 * @param amount Brightness adjustment (-1.0 = darker, 0 = normal, +1.0 = brighter)
 */
void sg_apply_brightness(SGNode* node, float amount);

/**
 * Adjust contrast
 * @param node Target node
 * @param amount Contrast adjustment (-1.0 = low, 0 = normal, +1.0 = high)
 */
void sg_apply_contrast(SGNode* node, float amount);

/**
 * Adjust saturation
 * @param node Target node
 * @param amount Saturation (0 = grayscale, 1 = normal, 2 = vivid)
 */
void sg_apply_saturation(SGNode* node, float amount);

/**
 * Clear all color filters
 */
void sg_clear_color_filter(SGNode* node);

/* ========================================================================
 * Glow Effects
 * ======================================================================== */

/**
 * Apply outer glow effect
 * @param node Target node
 * @param color Glow color
 * @param radius Glow radius
 */
void sg_apply_outer_glow(SGNode* node, uint32_t color, float radius);

/**
 * Apply inner glow effect
 * @param node Target node
 * @param color Glow color
 * @param radius Glow radius
 */
void sg_apply_inner_glow(SGNode* node, uint32_t color, float radius);

/**
 * Clear glow effects
 */
void sg_clear_glow(SGNode* node);

/* ========================================================================
 * Composite Effects
 * ======================================================================== */

/**
 * Apply multiple effects at once
 * Creates a "premium" look with shadow + subtle glow
 */
void sg_apply_premium_effect(SGNode* node);

/**
 * Apply "card" effect (common for UI cards)
 * Subtle shadow with border radius
 */
void sg_apply_card_effect(SGNode* node);

/**
 * Clear all effects from node
 */
void sg_clear_all_effects(SGNode* node);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_EFFECTS_H */
