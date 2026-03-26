/*
 * Layout Engine — Flexbox-like layout computation for Casperix UI
 *
 * Two-pass algorithm:
 *   1. Measure pass (bottom-up): determine intrinsic sizes
 *   2. Arrange pass (top-down): assign positions within available space
 *
 * Supports flex row/column, stack, wrapping, alignment, and spacing.
 * Only re-layouts dirty subtrees (nodes marked SG_DIRTY_LAYOUT).
 */

#ifndef LAYOUT_H
#define LAYOUT_H

#include "scene_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Layout API
 * ======================================================================== */

/* Measure intrinsic size of a node and its subtree.
 * Returns preferred size within the given constraints. */
SGSize sg_layout_measure(SGNode* node, SGConstraints constraints);

/* Arrange a node and its children within the given available rect.
 * Sets node->bounds for all nodes in the subtree. */
void sg_layout_arrange(SGNode* node, SGRect available);

/* Convenience: Measure + arrange from root.
 * Equivalent to measure then arrange with the window rect. */
void sg_layout_compute(SGNode* root, float window_width, float window_height);

/* ========================================================================
 * Intrinsic Size Queries
 * ======================================================================== */

/* Get intrinsic (content) size of a leaf node (text, image).
 * Returns {0,0} for containers — they size from children. */
SGSize sg_layout_intrinsic_size(SGNode* node);

/* Get content area (bounds minus padding) */
SGRect sg_layout_content_rect(SGNode* node);

/* Get outer area (bounds plus margin) */
SGRect sg_layout_outer_rect(SGNode* node);

/* ========================================================================
 * Utility
 * ======================================================================== */

/* Clamp a float to [min, max] */
float sg_clampf(float val, float min_val, float max_val);

/* Check if a node needs re-layout */
int sg_layout_needs_update(SGNode* node);

/* Clear layout dirty flags in entire subtree */
void sg_layout_clear_dirty(SGNode* node);

#ifdef __cplusplus
}
#endif

#endif /* LAYOUT_H */
