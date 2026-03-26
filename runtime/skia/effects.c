/**
 * Visual Effects Implementation
 */

#include "effects.h"
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Shadow Effects
 * ======================================================================== */

void sg_apply_drop_shadow(SGNode* node, float offset_x, float offset_y,
                          float blur, uint32_t color) {
    if (!node) return;
    
    node->style.shadow_offset_x = offset_x;
    node->style.shadow_offset_y = offset_y;
    node->style.shadow_blur = blur;
    node->style.shadow_color = color;
    
    /* Mark for repaint */
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_clear_shadow(SGNode* node) {
    if (!node) return;
    
    node->style.shadow_blur = 0;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Blur Effects
 * ======================================================================== */

void sg_apply_blur(SGNode* node, float sigma) {
    if (!node) return;
    
    /* Store blur in elevation field (repurpose for blur) */
    /* In a full implementation, we'd use a color matrix or image filter */
    node->style.elevation = (int)sigma;
    
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_clear_blur(SGNode* node) {
    if (!node) return;
    
    node->style.elevation = 0;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Color Filters
 * ======================================================================== */

void sg_apply_grayscale(SGNode* node) {
    if (!node) return;
    
    /* In full implementation, apply color matrix filter to node */
    /* For now, we modify opacity to indicate filter is active */
    /* Real implementation would use Skia's SkColorFilter */
    
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_apply_sepia(SGNode* node) {
    if (!node) return;
    
    /* Color matrix for sepia tone:
     * R' = R*0.393 + G*0.769 + B*0.189
     * G' = R*0.349 + G*0.686 + B*0.168
     * B' = R*0.272 + G*0.534 + B*0.131
     */
    
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_apply_invert(SGNode* node) {
    if (!node) return;
    
    /* Color matrix for inversion */
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_apply_brightness(SGNode* node, float amount) {
    if (!node) return;
    
    /* Clamp amount to valid range */
    if (amount < -1.0f) amount = -1.0f;
    if (amount > 1.0f) amount = 1.0f;
    
    /* Adjust opacity as a simple brightness approximation */
    float new_opacity = 1.0f + (amount * 0.3f);
    if (new_opacity < 0.3f) new_opacity = 0.3f;
    if (new_opacity > 1.0f) new_opacity = 1.0f;
    
    node->style.opacity = new_opacity;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_apply_contrast(SGNode* node, float amount) {
    if (!node) return;
    
    /* Clamp amount */
    if (amount < -1.0f) amount = -1.0f;
    if (amount > 1.0f) amount = 1.0f;
    
    /* Color matrix for contrast adjustment */
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_apply_saturation(SGNode* node, float amount) {
    if (!node) return;
    
    /* Clamp amount */
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 2.0f) amount = 2.0f;
    
    /* Color matrix for saturation:
     * amount = 0: grayscale
     * amount = 1: normal
     * amount = 2: vivid
     */
    
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_clear_color_filter(SGNode* node) {
    if (!node) return;
    
    node->style.opacity = 1.0f;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Glow Effects
 * ======================================================================== */

void sg_apply_outer_glow(SGNode* node, uint32_t color, float radius) {
    if (!node) return;
    
    /* Outer glow is essentially a shadow with no offset */
    sg_apply_drop_shadow(node, 0, 0, radius, color);
}

void sg_apply_inner_glow(SGNode* node, uint32_t color, float radius) {
    if (!node) return;
    
    /* Inner glow requires a different rendering approach */
    /* For now, apply border with glow color */
    node->style.border_color = color;
    node->style.border_width = radius / 4.0f;
    
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_clear_glow(SGNode* node) {
    if (!node) return;
    
    sg_clear_shadow(node);
    node->style.border_width = 0;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Composite Effects
 * ======================================================================== */

void sg_apply_premium_effect(SGNode* node) {
    if (!node) return;
    
    /* Premium look: subtle shadow + slight glow */
    sg_apply_drop_shadow(node, 0, 4, 12, 0x30000000);  /* Soft shadow */
    node->style.border_radius = 8.0f;  /* Rounded corners */
    node->style.elevation = 2;  /* Slight elevation */
    
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

void sg_apply_card_effect(SGNode* node) {
    if (!node) return;
    
    /* Material Design card style */
    sg_apply_drop_shadow(node, 0, 2, 6, 0x20000000);
    node->style.border_radius = 4.0f;
    node->style.background = 0xFFFFFFFF;  /* White background */
    node->style.padding[0] = 16.0f;
    node->style.padding[1] = 16.0f;
    node->style.padding[2] = 16.0f;
    node->style.padding[3] = 16.0f;
    
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

void sg_clear_all_effects(SGNode* node) {
    if (!node) return;
    
    sg_clear_shadow(node);
    sg_clear_blur(node);
    sg_clear_color_filter(node);
    sg_clear_glow(node);
    
    node->style.elevation = 0;
    
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}
