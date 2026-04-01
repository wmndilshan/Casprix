/*
 * Scene Graph Implementation
 *
 * Tree management, dirty tracking, rendering traversal, and hit testing.
 */

#include "scene_graph.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Global State
 * ======================================================================== */

static uint32_t g_next_node_id = 1;

uint32_t sg_next_id(void) {
    return g_next_node_id++;
}

/* ========================================================================
 * Node Creation / Destruction
 * ======================================================================== */

SGNode* sg_node_create(SGNodeType type) {
    SGNode* node = (SGNode*)calloc(1, sizeof(SGNode));
    if (!node) return NULL;

    node->type = type;
    node->id = sg_next_id();

    /* Default transform: identity */
    node->transform.sx = 1.0f;
    node->transform.sy = 1.0f;

    /* Default style */
    sg_style_init(&node->style);

    /* Default state: visible */
    node->flags = SG_VISIBLE | SG_DIRTY_LAYOUT | SG_DIRTY_PAINT;

    /* Default layout */
    node->flex_basis = -1.0f;  /* Auto */
    node->max_width = -1.0f;
    node->max_height = -1.0f;
    node->layout_version   = 0;
    node->dirty_rect_valid = 0;
    node->dirty_rect       = (SGRect){ 0, 0, 0, 0 };

    return node;
}

void sg_node_destroy(SGNode* node) {
    if (!node) return;

    /* Recursively destroy children */
    SGNode* child = node->first_child;
    while (child) {
        SGNode* next = child->next_sibling;
        sg_node_destroy(child);
        child = next;
    }

    sg_node_destroy_single(node);
}

void sg_node_destroy_single(SGNode* node) {
    if (!node) return;

    /* Free type-specific data */
    switch (node->type) {
        case SG_NODE_TEXT:
            if (node->data.text.text) free(node->data.text.text);
            if (node->data.text.font) skia_font_destroy(node->data.text.font);
            break;
        case SG_NODE_IMAGE:
            if (node->data.image.image) skia_image_destroy(node->data.image.image);
            break;
        case SG_NODE_PATH:
            if (node->data.path.path) skia_path_destroy(node->data.path.path);
            break;
        default:
            break;
    }

    /* Free gradient shader if set */
    if (node->style.gradient) skia_shader_destroy(node->style.gradient);

    free(node);
}

/* ========================================================================
 * Tree Operations
 * ======================================================================== */

void sg_node_add_child(SGNode* parent, SGNode* child) {
    if (!parent || !child) return;
    if (child->parent) sg_node_remove_from_parent(child);

    child->parent = parent;
    child->next_sibling = NULL;
    child->prev_sibling = parent->last_child;

    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
    parent->child_count++;

    sg_node_mark_layout_dirty(parent);
}

void sg_node_insert_before(SGNode* parent, SGNode* child, SGNode* before) {
    if (!parent || !child) return;
    if (!before) {
        sg_node_add_child(parent, child);
        return;
    }

    if (child->parent) sg_node_remove_from_parent(child);

    child->parent = parent;
    child->next_sibling = before;
    child->prev_sibling = before->prev_sibling;

    if (before->prev_sibling) {
        before->prev_sibling->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    before->prev_sibling = child;
    parent->child_count++;

    sg_node_mark_layout_dirty(parent);
}

void sg_node_insert_after(SGNode* parent, SGNode* child, SGNode* after) {
    if (!parent || !child) return;
    if (!after) {
        /* Insert at beginning */
        sg_node_insert_before(parent, child, parent->first_child);
        return;
    }

    if (child->parent) sg_node_remove_from_parent(child);

    child->parent = parent;
    child->prev_sibling = after;
    child->next_sibling = after->next_sibling;

    if (after->next_sibling) {
        after->next_sibling->prev_sibling = child;
    } else {
        parent->last_child = child;
    }
    after->next_sibling = child;
    parent->child_count++;

    sg_node_mark_layout_dirty(parent);
}

void sg_node_remove_child(SGNode* parent, SGNode* child) {
    if (!parent || !child || child->parent != parent) return;

    if (child->prev_sibling) {
        child->prev_sibling->next_sibling = child->next_sibling;
    } else {
        parent->first_child = child->next_sibling;
    }

    if (child->next_sibling) {
        child->next_sibling->prev_sibling = child->prev_sibling;
    } else {
        parent->last_child = child->prev_sibling;
    }

    child->parent = NULL;
    child->next_sibling = NULL;
    child->prev_sibling = NULL;
    parent->child_count--;

    sg_node_mark_layout_dirty(parent);
}

void sg_node_remove_from_parent(SGNode* node) {
    if (node && node->parent) {
        sg_node_remove_child(node->parent, node);
    }
}

SGNode* sg_node_first_child(SGNode* node) { return node ? node->first_child : NULL; }
SGNode* sg_node_last_child(SGNode* node) { return node ? node->last_child : NULL; }
SGNode* sg_node_next(SGNode* node) { return node ? node->next_sibling : NULL; }
SGNode* sg_node_prev(SGNode* node) { return node ? node->prev_sibling : NULL; }
int sg_node_child_count(SGNode* node) { return node ? node->child_count : 0; }

/* ========================================================================
 * Dirty Tracking
 * ======================================================================== */

void sg_node_mark_dirty(SGNode* node, uint32_t flags) {
    if (!node) return;
    node->flags |= flags;

    /* Propagate SG_DIRTY_CHILDREN up to root */
    SGNode* p = node->parent;
    while (p) {
        if (p->flags & SG_DIRTY_CHILDREN) break;  /* Already marked */
        p->flags |= SG_DIRTY_CHILDREN;
        p = p->parent;
    }
}

void sg_node_mark_layout_dirty(SGNode* node) {
    if (node) node->layout_version++;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

void sg_node_mark_paint_dirty(SGNode* node) {
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void sg_node_clear_dirty(SGNode* node) {
    if (!node) return;
    node->flags &= ~(SG_DIRTY_LAYOUT | SG_DIRTY_PAINT | SG_DIRTY_CHILDREN);
    node->dirty_rect_valid = 0;
}

int sg_node_is_dirty(SGNode* node, uint32_t flags) {
    return node ? (node->flags & flags) != 0 : 0;
}

/* ------------------------------------------------------------------------
 * Dirty-region helpers & selective repaint
 * ------------------------------------------------------------------------ */

static uint32_t g_sg_paint_node_count  = 0;

uint32_t sg_debug_paint_node_count(void) { return g_sg_paint_node_count; }
void sg_debug_reset_paint_node_count(void) { g_sg_paint_node_count = 0; }

void sg_mark_dirty(SGNode* node) {
    sg_node_mark_paint_dirty(node);
}

void sg_mark_dirty_rect(SGNode* node, SGRect rect) {
    if (!node) return;
    if (!node->dirty_rect_valid) {
        node->dirty_rect       = rect;
        node->dirty_rect_valid = 1;
    } else {
        float x1 = node->dirty_rect.x;
        float y1 = node->dirty_rect.y;
        float x2 = node->dirty_rect.x + node->dirty_rect.w;
        float y2 = node->dirty_rect.y + node->dirty_rect.h;
        float rx1 = rect.x;
        float ry1 = rect.y;
        float rx2 = rect.x + rect.w;
        float ry2 = rect.y + rect.h;
        float nx1 = x1 < rx1 ? x1 : rx1;
        float ny1 = y1 < ry1 ? y1 : ry1;
        float nx2 = x2 > rx2 ? x2 : rx2;
        float ny2 = y2 > ry2 ? y2 : ry2;
        node->dirty_rect.x = nx1;
        node->dirty_rect.y = ny1;
        node->dirty_rect.w = nx2 - nx1;
        node->dirty_rect.h = ny2 - ny1;
    }
    sg_node_mark_paint_dirty(node);
}

int sg_is_subtree_dirty(const SGNode* node) {
    if (!node) return 0;
    if (node->flags & (SG_DIRTY_LAYOUT | SG_DIRTY_PAINT | SG_DIRTY_CHILDREN)) return 1;
    for (SGNode* c = node->first_child; c; c = c->next_sibling) {
        if (sg_is_subtree_dirty(c)) return 1;
    }
    return 0;
}

void sg_clear_dirty(SGNode* node) {
    sg_node_clear_dirty(node);
}

void sg_clear_dirty_recursive(SGNode* root) {
    if (!root) return;
    sg_node_clear_dirty(root);
    for (SGNode* c = root->first_child; c; c = c->next_sibling) {
        sg_clear_dirty_recursive(c);
    }
}

/* ========================================================================
 * Style Helpers
 * ======================================================================== */

void sg_style_init(SGStyle* style) {
    memset(style, 0, sizeof(SGStyle));
    style->opacity = 1.0f;
    style->shadow_color = 0x40000000;  /* Semi-transparent black */
}

void sg_style_set_background(SGNode* node, uint32_t color) {
    if (!node) return;
    node->style.background = color;
    sg_node_mark_paint_dirty(node);
}

void sg_style_set_border(SGNode* node, uint32_t color, double width, double radius) {
    if (!node) return;
    node->style.border_color = color;
    node->style.border_width = width;
    node->style.border_radius = radius;
    sg_node_mark_paint_dirty(node);
}

void sg_style_set_padding(SGNode* node, double top, double right, double bottom, double left) {
    if (!node) return;
    node->style.padding[0] = top;
    node->style.padding[1] = right;
    node->style.padding[2] = bottom;
    node->style.padding[3] = left;
    sg_node_mark_layout_dirty(node);
}

void sg_style_set_padding_uniform(SGNode* node, double padding) {
    sg_style_set_padding(node, padding, padding, padding, padding);
}

void sg_style_set_margin(SGNode* node, double top, double right, double bottom, double left) {
    if (!node) return;
    node->style.margin[0] = top;
    node->style.margin[1] = right;
    node->style.margin[2] = bottom;
    node->style.margin[3] = left;
    sg_node_mark_layout_dirty(node);
}

void sg_style_set_margin_uniform(SGNode* node, double margin) {
    sg_style_set_margin(node, margin, margin, margin, margin);
}

void sg_style_set_shadow(SGNode* node, double ox, double oy, double blur, uint32_t color) {
    if (!node) return;
    node->style.elevation = 1;
    node->style.shadow_offset_x = ox;
    node->style.shadow_offset_y = oy;
    node->style.shadow_blur = blur;
    node->style.shadow_color = color;
    sg_node_mark_paint_dirty(node);
}

void sg_style_set_opacity(SGNode* node, double opacity) {
    if (!node) return;
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    node->style.opacity = opacity;
    sg_node_mark_paint_dirty(node);
}

void sg_style_set_gradient(SGNode* node, SkiaShader gradient) {
    if (!node) return;
    if (node->style.gradient) skia_shader_destroy(node->style.gradient);
    node->style.gradient = gradient;
    sg_node_mark_paint_dirty(node);
}

void sg_node_set_visible(SGNode* node, int visible) {
    if (!node) return;
    if (visible) node->flags |= SG_VISIBLE;
    else         node->flags &= ~SG_VISIBLE;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

void sg_node_set_min_size(SGNode* node, double min_w, double min_h) {
    if (!node) return;
    node->min_width  = min_w;
    node->min_height = min_h;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT);
}

void sg_node_set_max_size(SGNode* node, double max_w, double max_h) {
    if (!node) return;
    node->max_width  = max_w;
    node->max_height = max_h;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT);
}

void sg_node_set_flex_grow(SGNode* node, double grow) {
    if (!node) return;
    node->flex_grow = grow;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT);
}

void sg_node_set_align_self(SGNode* node, int align) {
    if (!node) return;
    node->align_self = align;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT);
}

void sg_node_set_align_items(SGNode* node, int align) {
    if (!node) return;
    node->align_items = align;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT);
}

/* ========================================================================
 * Rendering — Private Helpers
 * ======================================================================== */

/* Reusable paint objects (avoid per-frame allocation) */
static SkiaPaint g_fill_paint = NULL;
static SkiaPaint g_stroke_paint = NULL;
static SkiaPaint g_shadow_paint = NULL;
static SkiaPaint g_text_paint = NULL;

static void sg_ensure_paints(void) {
    if (!g_fill_paint) {
        g_fill_paint = skia_paint_create();
        skia_paint_set_style(g_fill_paint, 0);  /* Fill */
    }
    if (!g_stroke_paint) {
        g_stroke_paint = skia_paint_create();
        skia_paint_set_style(g_stroke_paint, 1);  /* Stroke */
    }
    if (!g_shadow_paint) {
        g_shadow_paint = skia_paint_create();
        skia_paint_set_style(g_shadow_paint, 0);
    }
    if (!g_text_paint) {
        g_text_paint = skia_paint_create();
        skia_paint_set_style(g_text_paint, 0);
    }
}

/* Draw shadow for a node */
static void sg_render_shadow(SkiaCanvas canvas, SGNode* node) {
    SGStyle* s = &node->style;
    if (s->elevation <= 0 && s->shadow_blur <= 0.0f) return;

    float blur = s->shadow_blur > 0 ? s->shadow_blur : (float)s->elevation * 2.0f;
    float ox = s->shadow_offset_x;
    float oy = s->shadow_offset_y > 0 ? s->shadow_offset_y : (float)s->elevation * 1.0f;

    skia_paint_set_color(g_shadow_paint, s->shadow_color);
    skia_paint_set_blur(g_shadow_paint, blur * 0.5f);

    SGRect b = node->bounds;
    if (s->border_radius > 0) {
        skia_canvas_draw_rrect(canvas,
            b.x + ox, b.y + oy, b.w, b.h,
            s->border_radius, s->border_radius,
            g_shadow_paint);
    } else {
        skia_canvas_draw_rect(canvas,
            b.x + ox, b.y + oy, b.w, b.h,
            g_shadow_paint);
    }

    skia_paint_clear_blur(g_shadow_paint);
}

/* Draw node background (fill + border) */
static void sg_render_background(SkiaCanvas canvas, SGNode* node) {
    SGStyle* s = &node->style;
    SGRect b = node->bounds;

    /* Background fill */
    if ((s->background & 0xFF000000) != 0 || s->gradient) {
        if (s->gradient) {
            skia_paint_set_shader(g_fill_paint, s->gradient);
        } else {
            skia_paint_set_color(g_fill_paint, s->background);
            skia_paint_set_shader(g_fill_paint, NULL);
        }

        if (s->border_radius > 0) {
            skia_canvas_draw_rrect(canvas, b.x, b.y, b.w, b.h,
                s->border_radius, s->border_radius, g_fill_paint);
        } else {
            skia_canvas_draw_rect(canvas, b.x, b.y, b.w, b.h, g_fill_paint);
        }

        skia_paint_set_shader(g_fill_paint, NULL);
    }

    /* Border */
    if (s->border_width > 0 && (s->border_color & 0xFF000000) != 0) {
        skia_paint_set_color(g_stroke_paint, s->border_color);
        skia_paint_set_stroke_width(g_stroke_paint, s->border_width);

        if (s->border_radius > 0) {
            skia_canvas_draw_rrect(canvas, b.x, b.y, b.w, b.h,
                s->border_radius, s->border_radius, g_stroke_paint);
        } else {
            skia_canvas_draw_rect(canvas, b.x, b.y, b.w, b.h, g_stroke_paint);
        }
    }
}

/* Render type-specific content */
static void sg_render_content(SkiaCanvas canvas, SGNode* node) {
    SGRect b = node->bounds;

    switch (node->type) {
        case SG_NODE_TEXT: {
            if (!node->data.text.text || !node->data.text.font) break;
            skia_paint_set_color(g_text_paint, node->data.text.color);

            float text_w = skia_font_measure_text(node->data.text.font, node->data.text.text);
            float font_h = skia_font_get_height(node->data.text.font);
            float ascent = -skia_font_get_ascent(node->data.text.font);

            /* Calculate text position based on alignment */
            float tx = b.x + node->style.padding[3];
            float ty = b.y + node->style.padding[0] + ascent;

            float avail_w = b.w - node->style.padding[1] - node->style.padding[3];
            switch (node->data.text.align) {
                case SG_TEXT_ALIGN_CENTER:
                    tx = b.x + (b.w - text_w) * 0.5f;
                    break;
                case SG_TEXT_ALIGN_RIGHT:
                    tx = b.x + b.w - text_w - node->style.padding[1];
                    break;
                default:
                    break;
            }
            (void)avail_w;
            (void)font_h;

            skia_canvas_draw_text(canvas, node->data.text.text,
                                   tx, ty, node->data.text.font, g_text_paint);
            break;
        }

        case SG_NODE_IMAGE: {
            if (!node->data.image.image) break;
            int img_w = skia_image_get_width(node->data.image.image);
            int img_h = skia_image_get_height(node->data.image.image);
            float pad_t = node->style.padding[0], pad_r = node->style.padding[1];
            float pad_b = node->style.padding[2], pad_l = node->style.padding[3];
            float dst_w = b.w - pad_l - pad_r;
            float dst_h = b.h - pad_t - pad_b;

            switch (node->data.image.fit) {
                case SG_IMAGE_FIT_FILL:
                    skia_canvas_draw_image_rect(canvas, node->data.image.image,
                        0, 0, (float)img_w, (float)img_h,
                        b.x + pad_l, b.y + pad_t, dst_w, dst_h, NULL);
                    break;
                case SG_IMAGE_FIT_CONTAIN: {
                    float scale_x = dst_w / img_w;
                    float scale_y = dst_h / img_h;
                    float scale = scale_x < scale_y ? scale_x : scale_y;
                    float fw = img_w * scale, fh = img_h * scale;
                    float fx = b.x + pad_l + (dst_w - fw) * 0.5f;
                    float fy = b.y + pad_t + (dst_h - fh) * 0.5f;
                    skia_canvas_draw_image_rect(canvas, node->data.image.image,
                        0, 0, (float)img_w, (float)img_h, fx, fy, fw, fh, NULL);
                    break;
                }
                case SG_IMAGE_FIT_COVER: {
                    float scale_x = dst_w / img_w;
                    float scale_y = dst_h / img_h;
                    float scale = scale_x > scale_y ? scale_x : scale_y;
                    float sw = dst_w / scale, sh = dst_h / scale;
                    float sx = (img_w - sw) * 0.5f, sy = (img_h - sh) * 0.5f;
                    skia_canvas_draw_image_rect(canvas, node->data.image.image,
                        sx, sy, sw, sh,
                        b.x + pad_l, b.y + pad_t, dst_w, dst_h, NULL);
                    break;
                }
            }
            break;
        }

        case SG_NODE_PATH: {
            if (!node->data.path.path) break;
            /* Fill */
            if ((node->data.path.fill_color & 0xFF000000) != 0) {
                skia_paint_set_color(g_fill_paint, node->data.path.fill_color);
                skia_canvas_draw_path(canvas, node->data.path.path, g_fill_paint);
            }
            /* Stroke */
            if ((node->data.path.stroke_color & 0xFF000000) != 0) {
                skia_paint_set_color(g_stroke_paint, node->data.path.stroke_color);
                skia_paint_set_stroke_width(g_stroke_paint, node->data.path.stroke_width);
                skia_canvas_draw_path(canvas, node->data.path.path, g_stroke_paint);
            }
            break;
        }

        case SG_NODE_CANVAS: {
            if (node->data.canvas.draw_fn) {
                node->data.canvas.draw_fn(canvas, b, node->data.canvas.ctx);
            }
            break;
        }

        case SG_NODE_CONTAINER:
        case SG_NODE_RECT:
        case SG_NODE_CLIP:
            /* Background already drawn, no additional content */
            break;
    }
}

/* ========================================================================
 * Rendering — Public API
 * ======================================================================== */

static void sg_render_node(SGNode* node, SkiaCanvas canvas, int dirty_only) {
    if (!node || !(node->flags & SG_VISIBLE)) return;
    if (dirty_only && !sg_is_subtree_dirty(node)) return;

    sg_ensure_paints();

    skia_canvas_save(canvas);

    SGTransform* t = &node->transform;
    if (t->tx != 0.0f || t->ty != 0.0f) {
        skia_canvas_translate(canvas, t->tx, t->ty);
    }
    if (t->sx != 1.0f || t->sy != 1.0f) {
        skia_canvas_scale(canvas, t->sx, t->sy);
    }
    if (t->rotation != 0.0f) {
        float cx = node->bounds.x + node->bounds.w * 0.5f;
        float cy = node->bounds.y + node->bounds.h * 0.5f;
        skia_canvas_translate(canvas, cx, cy);
        skia_canvas_rotate(canvas, t->rotation);
        skia_canvas_translate(canvas, -cx, -cy);
    }

    if (node->flags & SG_CLIP_CHILDREN) {
        SGRect b = node->bounds;
        if (node->style.border_radius > 0) {
            skia_canvas_clip_rrect(canvas, b.x, b.y, b.w, b.h,
                node->style.border_radius, node->style.border_radius);
        } else {
            skia_canvas_clip_rect(canvas, b.x, b.y, b.w, b.h);
        }
    }

    if (!dirty_only || (node->flags & SG_DIRTY_PAINT)) {
        if (dirty_only) g_sg_paint_node_count++;
        sg_render_shadow(canvas, node);
        sg_render_background(canvas, node);
        sg_render_content(canvas, node);
    }

    SGNode* child = node->first_child;
    while (child) {
        sg_render_node(child, canvas, dirty_only);
        child = child->next_sibling;
    }

    skia_canvas_restore(canvas);

    if (!dirty_only)
        node->flags &= ~SG_DIRTY_PAINT;
}

void sg_render(SGNode* root, SkiaCanvas canvas) {
    if (!root || !canvas) return;
    sg_render_node(root, canvas, 0);
}

void sg_render_dirty(SGNode* root, SkiaCanvas canvas) {
    sg_render_dirty_only(root, canvas);
}

void sg_render_dirty_only(SGNode* root, SkiaCanvas canvas) {
    if (!root || !canvas) return;
    g_sg_paint_node_count = 0;
    sg_render_node(root, canvas, 1);
    sg_clear_dirty_recursive(root);
}

/* ========================================================================
 * Hit Testing
 * ======================================================================== */

int sg_point_in_node(SGNode* node, float x, float y) {
    if (!node) return 0;
    SGRect b = node->bounds;

    /* Apply parent transforms to get world-space bounds.
     * For now, use local bounds (assumes no transforms). */
    return (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h);
}

static SGNode* sg_hit_test_node(SGNode* node, float x, float y) {
    if (!node || !(node->flags & SG_VISIBLE)) return NULL;

    /* Test children in reverse order (last child = top of z-stack) */
    SGNode* child = node->last_child;
    while (child) {
        SGNode* hit = sg_hit_test_node(child, x, y);
        if (hit) return hit;
        child = child->prev_sibling;
    }

    /* Test this node if interactive */
    if ((node->flags & SG_INTERACTIVE) && sg_point_in_node(node, x, y)) {
        return node;
    }

    return NULL;
}

SGNode* sg_hit_test(SGNode* root, float x, float y) {
    return sg_hit_test_node(root, x, y);
}

/* ========================================================================
 * Events
 * ======================================================================== */

void sg_node_on(SGNode* node, int event_type, SGEventHandler handler, void* user_data) {
    if (!node || !handler || node->handler_count >= SG_MAX_HANDLERS) return;

    /* Check if handler already exists for this event type */
    for (int i = 0; i < node->handler_count; i++) {
        if (node->handlers[i].event_type == event_type) {
            node->handlers[i].handler = handler;
            node->handlers[i].user_data = user_data;
            return;
        }
    }

    SGHandlerSlot* slot = &node->handlers[node->handler_count++];
    slot->event_type = event_type;
    slot->handler = handler;
    slot->user_data = user_data;

    /* Auto-enable interactivity */
    node->flags |= SG_INTERACTIVE;
}

void sg_node_off(SGNode* node, int event_type) {
    if (!node) return;
    for (int i = 0; i < node->handler_count; i++) {
        if (node->handlers[i].event_type == event_type) {
            /* Shift remaining handlers down */
            for (int j = i; j < node->handler_count - 1; j++) {
                node->handlers[j] = node->handlers[j + 1];
            }
            node->handler_count--;
            return;
        }
    }
}

void sg_node_emit(SGNode* node, int event_type, void* event_data) {
    if (!node) return;
    for (int i = 0; i < node->handler_count; i++) {
        if (node->handlers[i].event_type == event_type) {
            node->handlers[i].handler(node, event_type, event_data,
                                       node->handlers[i].user_data);
            return;
        }
    }
}
