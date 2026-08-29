/*
 * Scene Graph Implementation
 *
 * Tree management, dirty tracking, rendering traversal, and hit testing.
 */

#include "scene_graph.h"
#include "widgets.h"
#include "style.h"
#include "text.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ========================================================================
 * Global State
 * ======================================================================== */

static uint32_t g_next_node_id = 1;

static void sg_node_release(SGNode* node);
static void sg_node_finalize(SGNode* node);
static void sg_node_release_children(SGNode* node);

static void* sg_arena_alloc_internal(SGArena* arena, size_t size, size_t align) {
    uintptr_t base_addr;
    uintptr_t aligned;
    size_t next;

    if (!arena || !arena->base || size == 0) return NULL;
    if (align == 0) align = 1;
    if ((align & (align - 1)) != 0) return NULL;

    base_addr = (uintptr_t)arena->base + arena->offset;
    aligned = (base_addr + (align - 1)) & ~(uintptr_t)(align - 1);
    next = (size_t)(aligned - (uintptr_t)arena->base) + size;
    if (next > arena->capacity) return NULL;

    arena->offset = next;
    return (void*)aligned;
}

void* sg_scene_alloc(SGScene* scene, size_t size, size_t align) {
    if (!scene) return NULL;
    return sg_arena_alloc_internal(&scene->arena, size, align);
}

SGScene* sg_scene_create(void* arena_memory, size_t arena_capacity) {
    SGScene* scene;
    if (!arena_memory || arena_capacity == 0) return NULL;

    scene = (SGScene*)calloc(1, sizeof(SGScene));
    if (!scene) return NULL;

    scene->arena.base = (uint8_t*)arena_memory;
    scene->arena.capacity = arena_capacity;
    scene->arena.offset = 0;
    scene->frame.root = NULL;
    scene->frame.dirty_count = 0;
    scene->frame.frame_id = 0;

    scene->fill_paint = skia_paint_create();
    scene->stroke_paint = skia_paint_create();
    scene->text_paint = skia_paint_create();
    scene->shadow_paint = skia_paint_create();
    if (scene->fill_paint) skia_paint_set_style(scene->fill_paint, 0);
    if (scene->stroke_paint) skia_paint_set_style(scene->stroke_paint, 1);
    if (scene->text_paint) skia_paint_set_style(scene->text_paint, 0);
    if (scene->shadow_paint) skia_paint_set_style(scene->shadow_paint, 0);

    return scene;
}

void sg_scene_register_root(SGScene* scene, SGNode* root) {
    if (!scene) return;
    scene->root = root;
    scene->frame.root = root;
}

void sg_scene_begin_frame(SGScene* scene) {
    if (!scene) return;
    scene->frame.frame_id++;
    scene->frame.dirty_count = 0;
}

void sg_scene_mark_dirty_rect(SGScene* scene, SGRect rect) {
    if (!scene) return;
    if (scene->frame.dirty_count < (uint32_t)(sizeof(scene->frame.dirty_rects) / sizeof(scene->frame.dirty_rects[0]))) {
        scene->frame.dirty_rects[scene->frame.dirty_count++] = rect;
    }
}

void sg_scene_mark_node_dirty(SGNode* node) {
    if (!node) return;
    sg_mark_dirty_rect(node, node->bounds);
    if (node->scene_owner) {
        sg_scene_mark_dirty_rect(node->scene_owner, node->bounds);
    }
}

void sg_scene_destroy(SGScene* scene) {
    if (!scene) return;
    if (scene->root) {
        sg_node_destroy_tree(scene->root);
        scene->root = NULL;
    }
    if (scene->fill_paint) skia_paint_destroy(scene->fill_paint);
    if (scene->stroke_paint) skia_paint_destroy(scene->stroke_paint);
    if (scene->text_paint) skia_paint_destroy(scene->text_paint);
    if (scene->shadow_paint) skia_paint_destroy(scene->shadow_paint);
    free(scene);
}

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
    node->ref_count = 1;
    node->ownership_flags = SG_NODE_OWNER_RUNTIME;

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

SGNode* sg_node_create_in_scene(SGScene* scene, SGNodeType type) {
    SGNode* node;
    if (!scene) return sg_node_create(type);

    node = (SGNode*)sg_scene_alloc(scene, sizeof(SGNode), 64);
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));

    node->type = type;
    node->id = sg_next_id();
    node->ref_count = 1;
    node->ownership_flags = SG_NODE_OWNER_RUNTIME;
    node->transform.sx = 1.0f;
    node->transform.sy = 1.0f;
    sg_style_init(&node->style);
    node->flags = SG_VISIBLE | SG_DIRTY_LAYOUT | SG_DIRTY_PAINT;
    node->flex_basis = -1.0f;
    node->max_width = -1.0f;
    node->max_height = -1.0f;
    node->dirty_rect = (SGRect){ 0, 0, 0, 0 };
    node->scene_owner = scene;
    return node;
}

int sg_node_set_lifecycle(SGNode* node, const SGWidgetVTable* lifecycle) {
    if (!node) return -1;
    node->lifecycle = lifecycle;
    return 0;
}

void sg_node_set_style_ref(SGNode* node, SGStyleRef* style_ref) {
    if (!node) return;
    node->style_ref = style_ref;
    sg_node_mark_paint_dirty(node);
}

void sg_node_run_init(SGNode* node) {
    if (!node || !node->lifecycle || !node->lifecycle->init) return;
    node->lifecycle->init(node->scene_owner, node);
}

void sg_node_run_destroy(SGNode* node) {
    if (!node || !node->lifecycle || !node->lifecycle->destroy) return;
    node->lifecycle->destroy(node->scene_owner, node);
}

void sg_node_run_measure(SGNode* node, const SGMeasureConstraints* constraints,
                         float* out_w, float* out_h) {
    if (!out_w || !out_h) return;
    *out_w = 0.0f;
    *out_h = 0.0f;
    if (!node || !node->lifecycle || !node->lifecycle->measure_layout) return;
    node->lifecycle->measure_layout(node->scene_owner, node, constraints, out_w, out_h);
}

void sg_node_run_paint(SGNode* node, SkiaCanvas canvas, SGRect clip) {
    if (!node || !node->lifecycle || !node->lifecycle->paint) return;
    node->lifecycle->paint(node->scene_owner, node, canvas, clip);
}

void sg_node_destroy(SGNode* node) {
    sg_node_release(node);
}

void sg_node_destroy_tree(SGNode* node) {
    sg_node_release(node);
}

void sg_node_retain(SGNode* node) {
    if (!node) return;
    node->ref_count++;
}

void sg_node_set_cleanup(SGNode* node, SGNodeCleanupFn cleanup) {
    if (!node) return;
    node->cleanup = cleanup;
}

void sg_node_destroy_single(SGNode* node) {
    if (!node) return;

    if (node->lifecycle && node->lifecycle->destroy) {
        node->lifecycle->destroy(node->scene_owner, node);
    }

    if (node->cleanup) {
        node->cleanup(node);
        node->cleanup = NULL;
    } else if (!node->lifecycle) {
        widget_cleanup(node);
    }

    /* Free type-specific data */
    switch (node->type) {
        case SG_NODE_TEXT:
            if (node->data.text.text && !node->scene_owner) free(node->data.text.text);
            if ((node->ownership_flags & SG_NODE_OWNS_FONT) && node->data.text.font) {
                skia_font_destroy(node->data.text.font);
            }
            break;
        case SG_NODE_IMAGE:
            if ((node->ownership_flags & SG_NODE_OWNS_IMAGE) && node->data.image.image) {
                skia_image_destroy(node->data.image.image);
            }
            break;
        case SG_NODE_PATH:
            if ((node->ownership_flags & SG_NODE_OWNS_PATH) && node->data.path.path) {
                skia_path_destroy(node->data.path.path);
            }
            break;
        default:
            break;
    }

    /* Free gradient shader if set */
    if ((node->ownership_flags & SG_NODE_OWNS_GRADIENT) && node->style.gradient) {
        skia_shader_destroy(node->style.gradient);
    }

    if (node->a11y_label) {
        free(node->a11y_label);
        node->a11y_label = NULL;
    }

    if (!node->scene_owner) {
        free(node);
    }
}

static void sg_node_release_children(SGNode* node) {
    SGNode* child;
    SGNode* next;

    if (!node) return;

    child = node->first_child;
    node->first_child = NULL;
    node->last_child = NULL;
    node->child_count = 0;

    while (child) {
        next = child->next_sibling;
        child->parent = NULL;
        child->prev_sibling = NULL;
        child->next_sibling = NULL;
        child->ownership_flags &= ~SG_NODE_OWNER_PARENT;
        sg_node_release(child);
        child = next;
    }
}

static void sg_node_finalize(SGNode* node) {
    if (!node) return;
    sg_node_release_children(node);
    sg_node_destroy_single(node);
}

static void sg_node_release(SGNode* node) {
    if (!node) return;
    if (node->ref_count == 0) return;
    node->ref_count--;
    if (node->ref_count == 0) {
        sg_node_finalize(node);
    }
}

/* ========================================================================
 * Tree Operations
 * ======================================================================== */

/* Upper bound on the depth we will walk when checking ancestry. A well-formed
 * UI tree is nowhere near this deep; hitting it means the parent chain is
 * already corrupt, so we conservatively treat that as "would cycle". */
#define SG_MAX_TREE_DEPTH 4096

int sg_node_is_ancestor(const SGNode* maybe_ancestor, const SGNode* node) {
    if (!maybe_ancestor || !node) return 0;
    const SGNode* p = node;
    int guard = 0;
    while (p) {
        if (p == maybe_ancestor) return 1;
        if (++guard > SG_MAX_TREE_DEPTH) return 1; /* corrupt chain — refuse */
        p = p->parent;
    }
    return 0;
}

/* Reject an attach that would make 'child' an ancestor of itself (directly or
 * transitively). Returns 1 if the attach is safe, 0 if it must be rejected. */
static int sg_attach_is_safe(SGNode* parent, SGNode* child) {
    if (!parent || !child) return 0;
    if (parent == child) return 0;
    if (sg_node_is_ancestor(child, parent)) return 0;
    return 1;
}

int sg_node_add_child(SGNode* parent, SGNode* child) {
    if (!sg_attach_is_safe(parent, child)) return -1;
    if (child->parent) sg_node_remove_from_parent(child);

    sg_node_retain(child);
    child->parent = parent;
    child->next_sibling = NULL;
    child->prev_sibling = parent->last_child;
    child->ownership_flags |= SG_NODE_OWNER_PARENT;

    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
    parent->child_count++;

    sg_node_mark_layout_dirty(parent);
    sg_a11y_notify_structural_change();
    return 0;
}

int sg_node_insert_before(SGNode* parent, SGNode* child, SGNode* before) {
    if (!sg_attach_is_safe(parent, child)) return -1;
    if (!before) {
        return sg_node_add_child(parent, child);
    }

    if (child->parent) sg_node_remove_from_parent(child);

    sg_node_retain(child);
    child->parent = parent;
    child->next_sibling = before;
    child->prev_sibling = before->prev_sibling;
    child->ownership_flags |= SG_NODE_OWNER_PARENT;

    if (before->prev_sibling) {
        before->prev_sibling->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    before->prev_sibling = child;
    parent->child_count++;

    sg_node_mark_layout_dirty(parent);
    sg_a11y_notify_structural_change();
    return 0;
}

int sg_node_insert_after(SGNode* parent, SGNode* child, SGNode* after) {
    if (!sg_attach_is_safe(parent, child)) return -1;
    if (!after) {
        /* Insert at beginning */
        return sg_node_insert_before(parent, child, parent->first_child);
    }

    if (child->parent) sg_node_remove_from_parent(child);

    sg_node_retain(child);
    child->parent = parent;
    child->prev_sibling = after;
    child->next_sibling = after->next_sibling;
    child->ownership_flags |= SG_NODE_OWNER_PARENT;

    if (after->next_sibling) {
        after->next_sibling->prev_sibling = child;
    } else {
        parent->last_child = child;
    }
    after->next_sibling = child;
    parent->child_count++;

    sg_node_mark_layout_dirty(parent);
    sg_a11y_notify_structural_change();
    return 0;
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
    child->ownership_flags &= ~SG_NODE_OWNER_PARENT;
    parent->child_count--;
    sg_node_release(child);

    sg_node_mark_layout_dirty(parent);
    sg_a11y_notify_structural_change();
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
    if ((node->ownership_flags & SG_NODE_OWNS_GRADIENT) && node->style.gradient) {
        skia_shader_destroy(node->style.gradient);
    }
    node->style.gradient = gradient;
    if (gradient) node->ownership_flags |= SG_NODE_OWNS_GRADIENT;
    else node->ownership_flags &= ~SG_NODE_OWNS_GRADIENT;
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
static void sg_render_node_shadow(SkiaCanvas canvas, SGNode* node) {
    SGStyle style_copy = node->style;
    SGStyle* s = &style_copy;
    if (node->style_ref) {
        SGResolvedStyle resolved;
        sg_style_resolve(node->style_ref, node->state_flags, &resolved);
        s->shadow_offset_y = resolved.data.shadow_offset_y;
        s->shadow_blur = resolved.data.shadow_blur;
        s->shadow_color = resolved.data.shadow_color;
        s->border_radius = resolved.data.radius;
    }
    if (s->elevation <= 0 && s->shadow_blur <= 0.0f) return;

    float blur = s->shadow_blur > 0 ? s->shadow_blur : (float)s->elevation * 2.0f;
    float ox = s->shadow_offset_x;
    float oy = s->shadow_offset_y > 0 ? s->shadow_offset_y : (float)s->elevation * 1.0f;

    SGRect b = node->bounds;
    if (s->shadow_blur <= 0.0f) {
        /* GDI-safe fallback: fake elevation via layered translucent shapes. */
        static const float kGrow[3] = { 0.0f, 1.0f, 2.0f };
        static const float kYOffset[3] = { 1.0f, 2.0f, 4.0f };
        static const uint8_t kAlpha[3] = { 40, 24, 12 };
        int passes = (s->elevation >= 2) ? 3 : ((s->elevation > 0) ? 2 : 1);
        for (int i = 0; i < passes; i++) {
            uint32_t rgb = s->shadow_color & 0x00FFFFFFu;
            uint32_t argb = ((uint32_t)kAlpha[i] << 24) | rgb;
            float grow = kGrow[i];
            float rx = s->border_radius > 0 ? s->border_radius + grow : 0.0f;
            skia_paint_set_color(g_shadow_paint, argb);
            if (s->border_radius > 0) {
                skia_canvas_draw_rrect(canvas,
                    b.x + ox - grow, b.y + oy + kYOffset[i], b.w + grow * 2.0f, b.h + grow * 2.0f,
                    rx, rx, g_shadow_paint);
            } else {
                skia_canvas_draw_rect(canvas,
                    b.x + ox - grow, b.y + oy + kYOffset[i], b.w + grow * 2.0f, b.h + grow * 2.0f,
                    g_shadow_paint);
            }
        }
    } else {
        skia_paint_set_color(g_shadow_paint, s->shadow_color);
        skia_paint_set_blur(g_shadow_paint, blur * 0.5f);
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
}

/* Draw node background (fill + border) */
static void sg_render_background(SkiaCanvas canvas, SGNode* node) {
    SGStyle style_copy = node->style;
    SGStyle* s = &style_copy;
    SGRect b = node->bounds;
    if (node->style_ref) {
        SGResolvedStyle resolved;
        sg_style_resolve(node->style_ref, node->state_flags, &resolved);
        s->background = resolved.data.bg_color;
        s->border_color = resolved.data.border_color;
        s->border_width = resolved.data.border_width;
        s->border_radius = resolved.data.radius;
        s->padding[0] = resolved.data.pad_top;
        s->padding[1] = resolved.data.pad_right;
        s->padding[2] = resolved.data.pad_bottom;
        s->padding[3] = resolved.data.pad_left;
        s->shadow_offset_y = resolved.data.shadow_offset_y;
        s->shadow_blur = resolved.data.shadow_blur;
        s->shadow_color = resolved.data.shadow_color;
    }

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

    if (node->lifecycle && node->lifecycle->paint) {
        node->lifecycle->paint(node->scene_owner, node, canvas, b);
        return;
    }

    switch (node->type) {
        case SG_NODE_TEXT: {
            if (widget_render_override(canvas, node)) break;
            if (!node->data.text.text || !node->data.text.font) break;
            skia_paint_set_color(g_text_paint, node->data.text.color);

            int has_nl = (strchr(node->data.text.text, '\n') != NULL);

            /* Fast path — single line, no wrap, no hard breaks. Byte-for-byte
             * identical to the pre-wrap behaviour. */
            if (!node->data.text.wrap && !has_nl) {
                float text_w = skia_font_measure_text(node->data.text.font, node->data.text.text);
                float font_h = skia_font_get_height(node->data.text.font);
                float ascent = -skia_font_get_ascent(node->data.text.font);

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

            /* Multi-line path — hard '\n' always splits; word-wrap only when
             * wrap is enabled and there is a bounded width to wrap against. */
            {
                float pad_l = node->style.padding[3];
                float pad_r = node->style.padding[1];
                float pad_t = node->style.padding[0];
                float wrap_w = 0.0f;
                if (node->data.text.wrap) {
                    wrap_w = b.w - pad_l - pad_r;
                    if (wrap_w < 1.0f) wrap_w = 1.0f;
                }

                TextLayout layout;
                text_layout_compute(&layout, node->data.text.text,
                                    node->data.text.font, wrap_w);

                int max_lines = layout.line_count;
                if (node->data.text.max_lines > 0 &&
                    max_lines > node->data.text.max_lines) {
                    max_lines = node->data.text.max_lines;
                }

                float ascent = -skia_font_get_ascent(node->data.text.font);
                float cy = b.y + pad_t + ascent;
                char line_buf[4096];

                for (int i = 0; i < max_lines; i++) {
                    int start = layout.line_starts[i];
                    int len = layout.line_lengths[i];
                    if (len > 0) {
                        if (len >= (int)sizeof(line_buf)) len = (int)sizeof(line_buf) - 1;
                        memcpy(line_buf, node->data.text.text + start, (size_t)len);
                        line_buf[len] = '\0';

                        float tx = b.x + pad_l;
                        switch (node->data.text.align) {
                            case SG_TEXT_ALIGN_CENTER:
                                tx = b.x + (b.w - layout.line_widths[i]) * 0.5f;
                                break;
                            case SG_TEXT_ALIGN_RIGHT:
                                tx = b.x + b.w - layout.line_widths[i] - pad_r;
                                break;
                            default:
                                break;
                        }
                        skia_canvas_draw_text(canvas, line_buf, tx, cy,
                                              node->data.text.font, g_text_paint);
                    }
                    cy += layout.line_height;
                }
            }
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

static WidgetType sg_node_widget_type(const SGNode* node) {
    if (!node || !node->user_data) return WIDGET_NONE;
    return *(const WidgetType*)node->user_data;
}

static int sg_build_path_to_node(const SGNode* node, const SGNode** path, int max_depth) {
    int depth = 0;
    const SGNode* current = node;

    while (current && depth < max_depth) {
        path[depth++] = current;
        current = current->parent;
    }

    for (int i = 0; i < depth / 2; i++) {
        const SGNode* tmp = path[i];
        path[i] = path[depth - 1 - i];
        path[depth - 1 - i] = tmp;
    }

    return depth;
}

static int sg_world_to_node_space(const SGNode* node, float world_x, float world_y,
                                  float* node_x, float* node_y) {
    const SGNode* path[64];
    int depth;
    float x = world_x;
    float y = world_y;
    const float epsilon = 1e-6f;

    if (!node || !node_x || !node_y) return 0;

    depth = sg_build_path_to_node(node, path, 64);
    if (depth <= 0) return 0;

    for (int i = 0; i < depth; i++) {
        const SGNode* current = path[i];
        const SGTransform* t = &current->transform;

        if (fabsf(t->tx) > epsilon || fabsf(t->ty) > epsilon) {
            x -= t->tx;
            y -= t->ty;
        }

        if (fabsf(t->sx - 1.0f) > epsilon || fabsf(t->sy - 1.0f) > epsilon) {
            if (fabsf(t->sx) <= epsilon || fabsf(t->sy) <= epsilon) {
                return 0;
            }
            x /= t->sx;
            y /= t->sy;
        }

        if (fabsf(t->rotation) > epsilon) {
            const float radians = -t->rotation * (3.14159265358979323846f / 180.0f);
            const float cx = current->bounds.x + current->bounds.w * 0.5f;
            const float cy = current->bounds.y + current->bounds.h * 0.5f;
            const float dx = x - cx;
            const float dy = y - cy;
            const float cosine = cosf(radians);
            const float sine = sinf(radians);
            x = cx + dx * cosine - dy * sine;
            y = cy + dx * sine + dy * cosine;
        }
    }

    *node_x = x;
    *node_y = y;
    return 1;
}

static SGRect sg_node_child_clip_rect(const SGNode* node) {
    SGRect clip = { 0, 0, 0, 0 };

    if (!node) return clip;

    clip = node->bounds;
    if (sg_node_widget_type(node) == WIDGET_SCROLL_VIEW) {
        clip.x += node->style.padding[3];
        clip.y += node->style.padding[0];
        clip.w -= node->style.padding[1] + node->style.padding[3];
        clip.h -= node->style.padding[0] + node->style.padding[2];
        if (clip.w < 0.0f) clip.w = 0.0f;
        if (clip.h < 0.0f) clip.h = 0.0f;
    }

    return clip;
}

static int sg_point_in_rect(float x, float y, SGRect rect) {
    return x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
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
        if (sg_node_widget_type(node) == WIDGET_SCROLL_VIEW) {
            b.x += node->style.padding[3];
            b.y += node->style.padding[0];
            b.w -= node->style.padding[1] + node->style.padding[3];
            b.h -= node->style.padding[0] + node->style.padding[2];
            if (b.w < 0) b.w = 0;
            if (b.h < 0) b.h = 0;
        }
        if (node->style.border_radius > 0) {
            skia_canvas_clip_rrect(canvas, b.x, b.y, b.w, b.h,
                node->style.border_radius, node->style.border_radius);
        } else {
            skia_canvas_clip_rect(canvas, b.x, b.y, b.w, b.h);
        }
    }

    if (!dirty_only || (node->flags & SG_DIRTY_PAINT)) {
        if (dirty_only) g_sg_paint_node_count++;
        sg_render_node_shadow(canvas, node);
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
    float local_x = 0.0f;
    float local_y = 0.0f;

    if (!node) return 0;
    if (!sg_node_world_to_local(node, x, y, &local_x, &local_y)) return 0;

    return local_x >= 0.0f && local_x < node->bounds.w &&
           local_y >= 0.0f && local_y < node->bounds.h;
}

int sg_node_world_to_local(SGNode* node, float world_x, float world_y,
                           float* local_x, float* local_y) {
    float node_x = 0.0f;
    float node_y = 0.0f;

    if (!node || !local_x || !local_y) return 0;
    if (!sg_world_to_node_space(node, world_x, world_y, &node_x, &node_y)) return 0;

    *local_x = node_x - node->bounds.x;
    *local_y = node_y - node->bounds.y;
    return 1;
}

static SGNode* sg_hit_test_node(SGNode* node, float x, float y) {
    float node_x = 0.0f;
    float node_y = 0.0f;
    int inside_child_clip = 1;

    if (!node || !(node->flags & SG_VISIBLE)) return NULL;
    if (!sg_world_to_node_space(node, x, y, &node_x, &node_y)) return NULL;

    if (node->flags & SG_CLIP_CHILDREN) {
        inside_child_clip = sg_point_in_rect(node_x, node_y, sg_node_child_clip_rect(node));
    }

    /* Test children in reverse order (last child = top of z-stack) */
    if (inside_child_clip) {
        SGNode* child = node->last_child;
        while (child) {
            SGNode* hit = sg_hit_test_node(child, x, y);
            if (hit) return hit;
            child = child->prev_sibling;
        }
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

/* Bound on nested event dispatch. A handler is free to emit further events
 * (e.g. a button click that opens a dialog); it is NOT allowed to recurse
 * unboundedly. Past this depth we stop dispatching rather than blow the
 * C stack. */
#define SG_MAX_EMIT_DEPTH 64
static int g_sg_emit_depth = 0;

void sg_node_emit(SGNode* node, int event_type, void* event_data) {
    if (!node) return;

    if (g_sg_emit_depth >= SG_MAX_EMIT_DEPTH) {
        fprintf(stderr,
                "[scene_graph] event dispatch depth limit (%d) reached; "
                "dropping event %d on node %u (runaway re-entrant handler?)\n",
                SG_MAX_EMIT_DEPTH, event_type, node->id);
        return;
    }

    for (int i = 0; i < node->handler_count; i++) {
        if (node->handlers[i].event_type != event_type) continue;

        SGEventHandler fn = node->handlers[i].handler;
        void* ud = node->handlers[i].user_data;

        /* Error boundary (what C can actually check):
         *   - a NULL / cleared handler slot is skipped, not called;
         *   - re-entrant dispatch is depth-bounded above.
         * What C CANNOT catch here without signal handlers: a genuine trap
         * (bad-pointer deref, unhandled Casprix `throw`) inside compiled
         * callback code. That still terminates the process by design — a
         * SIGSEGV boundary would mask real bugs, and this codebase has no
         * such infrastructure (ARC/ownership violations also abort()). */
        if (!fn) {
            fprintf(stderr,
                    "[scene_graph] null handler for event %d on node %u; "
                    "ignoring\n", event_type, node->id);
            return;
        }

        g_sg_emit_depth++;
        fn(node, event_type, event_data, ud);
        g_sg_emit_depth--;
        return;
    }
}

/* ========================================================================
 * Accessibility (v1b)
 * ======================================================================== */

static SGA11yStructuralChangeFn g_a11y_change_cb = NULL;
static void*                    g_a11y_change_ud = NULL;

/* Coalesce: only the FIRST notification since the last time the consumer was
 * called actually invokes the callback. The Java side already debounces
 * content-changed events, and the a11y bridge re-reads the whole tree on the
 * next query, so one edge per burst is enough and avoids event spam. */
static int g_a11y_change_pending = 0;

void sg_a11y_set_structural_change_cb(SGA11yStructuralChangeFn cb, void* user_data) {
    g_a11y_change_cb = cb;
    g_a11y_change_ud = user_data;
    g_a11y_change_pending = 0;
}

void sg_a11y_notify_structural_change(void) {
    if (!g_a11y_change_cb) return;
    if (g_a11y_change_pending) return;   /* already signalled this burst */
    g_a11y_change_pending = 1;
    g_a11y_change_cb(g_a11y_change_ud);
    /* The callback (JNI up-call) has consumed the edge; re-arm for the next. */
    g_a11y_change_pending = 0;
}

void sg_node_set_a11y_label(SGNode* node, const char* label) {
    if (!node) return;
    if (node->a11y_label) {
        free(node->a11y_label);
        node->a11y_label = NULL;
    }
    if (label) {
        size_t n = strlen(label) + 1;
        node->a11y_label = (char*)malloc(n);
        if (node->a11y_label) memcpy(node->a11y_label, label, n);
    }
    sg_a11y_notify_structural_change();
}

const char* sg_node_get_a11y_label(const SGNode* node) {
    return node ? node->a11y_label : NULL;
}

void sg_node_set_a11y_role(SGNode* node, SGA11yRole role) {
    if (!node) return;
    if (node->a11y_role != role) {
        node->a11y_role = role;
        sg_a11y_notify_structural_change();
    }
}

SGA11yRole sg_node_get_a11y_role(const SGNode* node) {
    return node ? node->a11y_role : SG_A11Y_ROLE_NONE;
}

void sg_node_set_a11y_hidden(SGNode* node, int hidden) {
    if (!node) return;
    hidden = hidden ? 1 : 0;
    if (node->a11y_hidden != hidden) {
        node->a11y_hidden = hidden;
        sg_a11y_notify_structural_change();
    }
}

int sg_node_get_a11y_hidden(const SGNode* node) {
    return node ? node->a11y_hidden : 0;
}
