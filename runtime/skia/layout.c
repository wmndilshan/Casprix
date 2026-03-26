/*
 * Layout Engine — Flexbox-like layout computation
 *
 * Implements a two-pass layout:
 *   Pass 1 (Measure): Bottom-up intrinsic size calculation
 *   Pass 2 (Arrange): Top-down position assignment with flex distribution
 */

#include "layout.h"
#include "skia_c.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================
 * Utilities
 * ======================================================================== */

float sg_clampf(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

/* Get total horizontal padding */
static float pad_h(SGNode* node) {
    return node->style.padding[1] + node->style.padding[3]; /* right + left */
}

/* Get total vertical padding */
static float pad_v(SGNode* node) {
    return node->style.padding[0] + node->style.padding[2]; /* top + bottom */
}

/* Get total horizontal margin */
static float margin_h(SGNode* node) {
    return node->style.margin[1] + node->style.margin[3];
}

/* Get total vertical margin */
static float margin_v(SGNode* node) {
    return node->style.margin[0] + node->style.margin[2];
}

/* Apply min/max size constraints to a node's computed size */
static SGSize apply_size_constraints(SGNode* node, SGSize size) {
    if (node->min_width > 0)
        size.w = maxf(size.w, node->min_width);
    if (node->min_height > 0)
        size.h = maxf(size.h, node->min_height);
    if (node->max_width > 0)
        size.w = minf(size.w, node->max_width);
    if (node->max_height > 0)
        size.h = minf(size.h, node->max_height);
    return size;
}

/* ========================================================================
 * Intrinsic Size
 * ======================================================================== */

SGSize sg_layout_intrinsic_size(SGNode* node) {
    SGSize size = { 0, 0 };
    if (!node) return size;

    switch (node->type) {
        case SG_NODE_TEXT:
            if (node->data.text.text && node->data.text.font) {
                size.w = skia_font_measure_text(node->data.text.font,
                                                 node->data.text.text);
                size.h = skia_font_get_height(node->data.text.font);
            }
            break;

        case SG_NODE_IMAGE:
            if (node->data.image.image) {
                size.w = (float)skia_image_get_width(node->data.image.image);
                size.h = (float)skia_image_get_height(node->data.image.image);
            }
            break;

        default:
            /* Containers and other nodes have no intrinsic size */
            break;
    }

    return size;
}

/* ========================================================================
 * Measure Pass (bottom-up)
 * ======================================================================== */

SGSize sg_layout_measure(SGNode* node, SGConstraints constraints) {
    if (!node) return (SGSize){ 0, 0 };
    if (!(node->flags & SG_VISIBLE)) return (SGSize){ 0, 0 };

    /* If node has explicit flex_basis, use that as starting size on main axis */
    SGSize size = { 0, 0 };

    /* Leaf nodes: use intrinsic size */
    if (!node->first_child) {
        size = sg_layout_intrinsic_size(node);
        size.w += pad_h(node);
        size.h += pad_v(node);
        size = apply_size_constraints(node, size);
        return size;
    }

    /* Container: aggregate children based on layout type */
    float content_w = 0, content_h = 0;
    int child_idx = 0;

    for (SGNode* child = node->first_child; child; child = child->next_sibling) {
        if (!(child->flags & SG_VISIBLE)) continue;

        SGSize child_size = sg_layout_measure(child, constraints);
        float cw = child_size.w + margin_h(child);
        float ch = child_size.h + margin_v(child);

        switch (node->layout_type) {
            case SG_LAYOUT_ROW:
                content_w += cw;
                if (child_idx > 0) content_w += node->spacing;
                content_h = maxf(content_h, ch);
                break;

            case SG_LAYOUT_COLUMN:
                content_h += ch;
                if (child_idx > 0) content_h += node->spacing;
                content_w = maxf(content_w, cw);
                break;

            case SG_LAYOUT_STACK:
                content_w = maxf(content_w, cw);
                content_h = maxf(content_h, ch);
                break;

            default: /* SG_LAYOUT_NONE — children positioned manually */
                content_w = maxf(content_w, child->bounds.x + cw);
                content_h = maxf(content_h, child->bounds.y + ch);
                break;
        }
        child_idx++;
    }

    size.w = content_w + pad_h(node);
    size.h = content_h + pad_v(node);
    size = apply_size_constraints(node, size);

    /* Clamp to constraints */
    size.w = sg_clampf(size.w, constraints.min_width, constraints.max_width);
    size.h = sg_clampf(size.h, constraints.min_height, constraints.max_height);

    return size;
}

/* ========================================================================
 * Arrange Pass (top-down)
 * ======================================================================== */

/* Temporary per-child data for flex computation */
typedef struct {
    SGNode* node;
    float base_size;       /* Measured size on main axis */
    float cross_size;      /* Measured size on cross axis */
    float final_size;      /* After flex grow/shrink */
    float flex_grow;
    float flex_shrink;
    float main_margin;     /* Total margin on main axis */
    float cross_margin;    /* Total margin on cross axis */
} FlexItem;

/* Flex layout for row or column */
static void arrange_flex(SGNode* parent, SGRect content, int is_row) {
    /* Count visible children and measure them */
    int n = 0;
    for (SGNode* c = parent->first_child; c; c = c->next_sibling) {
        if (c->flags & SG_VISIBLE) n++;
    }
    if (n == 0) return;

    /* Stack-allocate flex items (use heap for large counts) */
    FlexItem* items;
    FlexItem stack_items[32];
    if (n <= 32) {
        items = stack_items;
    } else {
        items = (FlexItem*)malloc(n * sizeof(FlexItem));
        if (!items) return;
    }

    /* Measure children */
    float total_base = 0;
    float total_grow = 0;
    float total_shrink = 0;
    float total_spacing = (n > 1) ? parent->spacing * (n - 1) : 0;
    int idx = 0;

    SGConstraints child_constraints = SG_CONSTRAINTS_NONE;

    for (SGNode* c = parent->first_child; c; c = c->next_sibling) {
        if (!(c->flags & SG_VISIBLE)) continue;

        SGSize measured = sg_layout_measure(c, child_constraints);
        FlexItem* item = &items[idx];
        item->node = c;
        item->flex_grow = c->flex_grow;
        item->flex_shrink = (c->flex_shrink > 0) ? c->flex_shrink : 1.0f;

        if (is_row) {
            item->base_size = (c->flex_basis > 0) ? c->flex_basis : measured.w;
            item->cross_size = measured.h;
            item->main_margin = margin_h(c);
            item->cross_margin = margin_v(c);
        } else {
            item->base_size = (c->flex_basis > 0) ? c->flex_basis : measured.h;
            item->cross_size = measured.w;
            item->main_margin = margin_v(c);
            item->cross_margin = margin_h(c);
        }

        total_base += item->base_size + item->main_margin;
        total_grow += item->flex_grow;
        total_shrink += item->flex_shrink;
        item->final_size = item->base_size;
        idx++;
    }

    /* Available space on main axis */
    float main_available = is_row ? content.w : content.h;
    float remaining = main_available - total_base - total_spacing;

    /* Distribute remaining space via flex grow/shrink */
    if (remaining > 0 && total_grow > 0) {
        /* Grow */
        for (int i = 0; i < n; i++) {
            if (items[i].flex_grow > 0) {
                items[i].final_size += remaining * (items[i].flex_grow / total_grow);
            }
        }
    } else if (remaining < 0 && total_shrink > 0) {
        /* Shrink */
        float deficit = -remaining;
        for (int i = 0; i < n; i++) {
            if (items[i].flex_shrink > 0) {
                items[i].final_size -= deficit * (items[i].flex_shrink / total_shrink);
                if (items[i].final_size < 0) items[i].final_size = 0;
            }
        }
    }

    /* Justify content — compute starting offset and gap */
    float cursor;
    float gap = parent->spacing;

    /* Recalculate total after flex distribution */
    float total_final = 0;
    for (int i = 0; i < n; i++) {
        total_final += items[i].final_size + items[i].main_margin;
    }
    total_final += total_spacing;
    float justify_remaining = main_available - total_final;

    switch (parent->justify) {
        case SG_JUSTIFY_CENTER:
            cursor = justify_remaining / 2.0f;
            break;
        case SG_JUSTIFY_END:
            cursor = justify_remaining;
            break;
        case SG_JUSTIFY_BETWEEN:
            cursor = 0;
            if (n > 1) gap = parent->spacing + justify_remaining / (n - 1);
            break;
        case SG_JUSTIFY_AROUND:
            if (n > 0) {
                float space = justify_remaining / n;
                cursor = space / 2.0f;
                gap = parent->spacing + space;
            } else {
                cursor = 0;
            }
            break;
        case SG_JUSTIFY_EVENLY:
            if (n > 0) {
                float space = justify_remaining / (n + 1);
                cursor = space;
                gap = parent->spacing + space;
            } else {
                cursor = 0;
            }
            break;
        default: /* SG_JUSTIFY_START */
            cursor = 0;
            break;
    }

    /* Position each child */
    float cross_available = is_row ? content.h : content.w;

    for (int i = 0; i < n; i++) {
        FlexItem* item = &items[i];
        SGNode* child = item->node;

        /* Main axis margin */
        float m_start = is_row ? child->style.margin[3] : child->style.margin[0];
        float m_end   = is_row ? child->style.margin[1] : child->style.margin[2];

        cursor += m_start;

        /* Cross axis alignment */
        int align = child->align_self;
        if (align == SG_ALIGN_START && parent->align_items != SG_ALIGN_START) {
            align = parent->align_items;
        }

        float cross_size;
        float cross_pos;
        float c_m_start = is_row ? child->style.margin[0] : child->style.margin[3];
        float c_m_end   = is_row ? child->style.margin[2] : child->style.margin[1];
        float cross_avail = cross_available - c_m_start - c_m_end;

        switch (align) {
            case SG_ALIGN_CENTER:
                cross_size = minf(item->cross_size, cross_avail);
                cross_pos = c_m_start + (cross_avail - cross_size) / 2.0f;
                break;
            case SG_ALIGN_END:
                cross_size = minf(item->cross_size, cross_avail);
                cross_pos = c_m_start + cross_avail - cross_size;
                break;
            case SG_ALIGN_STRETCH:
                cross_size = cross_avail;
                cross_pos = c_m_start;
                break;
            default: /* SG_ALIGN_START */
                cross_size = minf(item->cross_size, cross_avail);
                cross_pos = c_m_start;
                break;
        }

        /* Set child bounds */
        if (is_row) {
            child->bounds.x = content.x + cursor;
            child->bounds.y = content.y + cross_pos;
            child->bounds.w = item->final_size;
            child->bounds.h = cross_size;
        } else {
            child->bounds.x = content.x + cross_pos;
            child->bounds.y = content.y + cursor;
            child->bounds.w = cross_size;
            child->bounds.h = item->final_size;
        }

        cursor += item->final_size + m_end + gap;

        /* Recursively arrange child */
        sg_layout_arrange(child, child->bounds);
    }

    if (n > 32) free(items);
}

/* Stack layout — each child fills the available rect */
static void arrange_stack(SGNode* parent, SGRect content) {
    for (SGNode* child = parent->first_child; child; child = child->next_sibling) {
        if (!(child->flags & SG_VISIBLE)) continue;

        child->bounds.x = content.x + child->style.margin[3];
        child->bounds.y = content.y + child->style.margin[0];
        child->bounds.w = content.w - margin_h(child);
        child->bounds.h = content.h - margin_v(child);

        sg_layout_arrange(child, child->bounds);
    }
}

/* None layout — children keep their manually set positions, offset by content origin */
static void arrange_none(SGNode* parent, SGRect content) {
    for (SGNode* child = parent->first_child; child; child = child->next_sibling) {
        if (!(child->flags & SG_VISIBLE)) continue;

        /* If child has no explicit size, measure it */
        if (child->bounds.w <= 0 || child->bounds.h <= 0) {
            SGSize measured = sg_layout_measure(child, SG_CONSTRAINTS_NONE);
            if (child->bounds.w <= 0) child->bounds.w = measured.w;
            if (child->bounds.h <= 0) child->bounds.h = measured.h;
        }

        /* Treat translate as a stable local offset for free-positioned
         * children so relayout does not keep accumulating parent origin. */
        child->bounds.x = content.x + child->transform.tx + child->style.margin[3];
        child->bounds.y = content.y + child->transform.ty + child->style.margin[0];

        sg_layout_arrange(child, child->bounds);
    }
}

void sg_layout_arrange(SGNode* node, SGRect available) {
    if (!node) return;
    if (!(node->flags & SG_VISIBLE)) return;

    /* Set own bounds to available rect (may be overridden by parent) */
    node->bounds = available;

    /* Compute content rect (bounds minus padding) */
    SGRect content;
    content.x = available.x + node->style.padding[3]; /* left */
    content.y = available.y + node->style.padding[0]; /* top */
    content.w = available.w - pad_h(node);
    content.h = available.h - pad_v(node);

    if (content.w < 0) content.w = 0;
    if (content.h < 0) content.h = 0;

    /* No children? Nothing to layout. */
    if (!node->first_child) return;

    switch (node->layout_type) {
        case SG_LAYOUT_ROW:
            arrange_flex(node, content, 1);
            break;
        case SG_LAYOUT_COLUMN:
            arrange_flex(node, content, 0);
            break;
        case SG_LAYOUT_STACK:
            arrange_stack(node, content);
            break;
        case SG_LAYOUT_WRAP:
            /* TODO: flex wrap — fall back to row for now */
            arrange_flex(node, content, 1);
            break;
        default:
            arrange_none(node, content);
            break;
    }
}

/* ========================================================================
 * Convenience API
 * ======================================================================== */

void sg_layout_compute(SGNode* root, float window_width, float window_height) {
    if (!root) return;

    SGConstraints constraints = {
        .min_width = 0,
        .max_width = window_width,
        .min_height = 0,
        .max_height = window_height
    };

    /* Measure pass */
    sg_layout_measure(root, constraints);

    /* Arrange pass */
    SGRect window_rect = { 0, 0, window_width, window_height };
    sg_layout_arrange(root, window_rect);

    /* Clear dirty flags */
    sg_layout_clear_dirty(root);
}

SGRect sg_layout_content_rect(SGNode* node) {
    SGRect r = { 0, 0, 0, 0 };
    if (!node) return r;
    r.x = node->bounds.x + node->style.padding[3];
    r.y = node->bounds.y + node->style.padding[0];
    r.w = node->bounds.w - pad_h(node);
    r.h = node->bounds.h - pad_v(node);
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

SGRect sg_layout_outer_rect(SGNode* node) {
    SGRect r = { 0, 0, 0, 0 };
    if (!node) return r;
    r.x = node->bounds.x - node->style.margin[3];
    r.y = node->bounds.y - node->style.margin[0];
    r.w = node->bounds.w + margin_h(node);
    r.h = node->bounds.h + margin_v(node);
    return r;
}

int sg_layout_needs_update(SGNode* node) {
    if (!node) return 0;
    return (node->flags & (SG_DIRTY_LAYOUT | SG_DIRTY_CHILDREN)) != 0;
}

void sg_layout_clear_dirty(SGNode* node) {
    if (!node) return;
    node->flags &= ~(SG_DIRTY_LAYOUT | SG_DIRTY_CHILDREN);
    for (SGNode* c = node->first_child; c; c = c->next_sibling) {
        sg_layout_clear_dirty(c);
    }
}
