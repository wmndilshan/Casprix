/*
 * Scene Graph — Retained-mode rendering tree for Casperix UI
 *
 * Hierarchical node tree with:
 *   - Transform propagation (translate, scale, rotate)
 *   - Dirty region tracking (SG_DIRTY_LAYOUT | SG_DIRTY_PAINT)
 *   - Style properties (background, border, shadow, gradient)
 *   - Layout hints (flex row/column, alignment)
 *   - Hit testing for event dispatch
 *   - Skia-based rendering via pre-order DFS traversal
 */

#ifndef SCENE_GRAPH_H
#define SCENE_GRAPH_H

#include "skia_c.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Node Types
 * ======================================================================== */

typedef enum {
    SG_NODE_CONTAINER,    /* Layout container (no visual of its own) */
    SG_NODE_RECT,         /* Colored/styled rectangle */
    SG_NODE_TEXT,         /* Text label */
    SG_NODE_IMAGE,        /* Bitmap image */
    SG_NODE_PATH,         /* Custom vector path */
    SG_NODE_CLIP,         /* Clipping region */
    SG_NODE_CANVAS,       /* Custom draw callback */
} SGNodeType;

/* ========================================================================
 * Geometry Types
 * ======================================================================== */

typedef struct {
    float x, y, w, h;
} SGRect;

typedef struct {
    float w, h;
} SGSize;

typedef struct {
    float tx, ty;         /* Translation */
    float sx, sy;         /* Scale (1.0 = identity) */
    float rotation;       /* Degrees */
} SGTransform;

/* ========================================================================
 * Style Properties
 * ======================================================================== */

typedef struct {
    /* Fill */
    uint32_t background;      /* ARGB fill color */
    SkiaShader gradient;      /* Optional gradient (overrides background) */

    /* Border */
    uint32_t border_color;
    float border_width;
    float border_radius;      /* Corner radius (all corners equal) */

    /* Spacing */
    float padding[4];         /* top, right, bottom, left */
    float margin[4];          /* top, right, bottom, left */

    /* Effects */
    float opacity;            /* 0.0 - 1.0 */
    int elevation;            /* Shadow depth (0 = none) */
    float shadow_offset_x;
    float shadow_offset_y;
    float shadow_blur;
    uint32_t shadow_color;
} SGStyle;

/* ========================================================================
 * Layout Configuration
 * ======================================================================== */

/* Layout direction */
#define SG_LAYOUT_NONE    0   /* Manual positioning */
#define SG_LAYOUT_ROW     1   /* Horizontal flex */
#define SG_LAYOUT_COLUMN  2   /* Vertical flex */
#define SG_LAYOUT_STACK   3   /* Overlapping children */
#define SG_LAYOUT_WRAP    4   /* Flex with line wrapping */

/* Alignment */
#define SG_ALIGN_START    0
#define SG_ALIGN_CENTER   1
#define SG_ALIGN_END      2
#define SG_ALIGN_STRETCH  3

/* Justify content (main axis distribution) */
#define SG_JUSTIFY_START    0
#define SG_JUSTIFY_CENTER   1
#define SG_JUSTIFY_END      2
#define SG_JUSTIFY_BETWEEN  3
#define SG_JUSTIFY_AROUND   4
#define SG_JUSTIFY_EVENLY   5

/* Text alignment */
#define SG_TEXT_ALIGN_LEFT   0
#define SG_TEXT_ALIGN_CENTER 1
#define SG_TEXT_ALIGN_RIGHT  2

/* Image fit mode */
#define SG_IMAGE_FIT_FILL    0
#define SG_IMAGE_FIT_CONTAIN 1
#define SG_IMAGE_FIT_COVER   2

/* ========================================================================
 * Dirty Flags
 * ======================================================================== */

#define SG_DIRTY_LAYOUT   0x01  /* Needs re-layout */
#define SG_DIRTY_PAINT    0x02  /* Needs repaint */
#define SG_DIRTY_CHILDREN 0x04  /* A child is dirty */
#define SG_VISIBLE        0x08  /* Node is visible */
#define SG_INTERACTIVE    0x10  /* Node receives events */
#define SG_CLIP_CHILDREN  0x20  /* Clip children to bounds */

/* ========================================================================
 * Event Handler Slots
 * ======================================================================== */

#define SG_MAX_HANDLERS 8

typedef struct SGNode SGNode;

typedef void (*SGEventHandler)(SGNode* node, int event_type, void* event_data, void* user_data);

typedef struct {
    int event_type;
    SGEventHandler handler;
    void* user_data;
} SGHandlerSlot;

typedef void (*SGNodeCleanupFn)(SGNode* node);

/* ========================================================================
 * Scene Graph Node
 * ======================================================================== */

#define SG_NODE_OWNER_RUNTIME     0x0001u
#define SG_NODE_OWNER_PARENT      0x0002u
#define SG_NODE_OWNER_APP         0x0004u
#define SG_NODE_OWNS_FONT         0x0010u
#define SG_NODE_OWNS_IMAGE        0x0020u
#define SG_NODE_OWNS_PATH         0x0040u
#define SG_NODE_OWNS_GRADIENT     0x0080u

struct SGNode {
    SGNodeType type;
    uint32_t id;                  /* Unique node ID */
    uint32_t ref_count;           /* Shared lifetime across wrappers/parents/app */
    uint32_t ownership_flags;     /* SG_NODE_OWNER_* | SG_NODE_OWNS_* */

    /* Geometry */
    SGRect bounds;                /* Computed bounds (set by layout) */
    SGTransform transform;        /* Local transform */
    SGStyle style;                /* Visual styling */

    /* Tree structure (first-child / next-sibling) */
    SGNode* parent;
    SGNode* first_child;
    SGNode* last_child;
    SGNode* next_sibling;
    SGNode* prev_sibling;
    int child_count;

    /* Type-specific data */
    union {
        struct {
            char* text;
            SkiaFont font;
            uint32_t color;
            int align;            /* SG_TEXT_ALIGN_* */
        } text;

        struct {
            SkiaImage image;
            int fit;              /* SG_IMAGE_FIT_* */
        } image;

        struct {
            SkiaPath path;
            uint32_t fill_color;
            uint32_t stroke_color;
            float stroke_width;
        } path;

        struct {
            void (*draw_fn)(SkiaCanvas canvas, SGRect bounds, void* ctx);
            void* ctx;
        } canvas;
    } data;

    /* Layout */
    int layout_type;              /* SG_LAYOUT_* */
    int justify;                  /* SG_JUSTIFY_* (main axis) */
    int align_items;              /* SG_ALIGN_* (cross axis) */
    int align_self;               /* SG_ALIGN_* (override parent) */
    float flex_grow;              /* Flex grow factor */
    float flex_shrink;            /* Flex shrink factor */
    float flex_basis;             /* Base size (-1 = auto) */
    float spacing;                /* Gap between children */
    float min_width, min_height;  /* Minimum size constraints */
    float max_width, max_height;  /* Maximum size constraints (-1 = none) */

    /* State */
    uint32_t flags;               /* SG_DIRTY_* | SG_VISIBLE | ... */
    int z_index;                  /* Rendering order (higher = on top) */

    /* Events */
    SGHandlerSlot handlers[SG_MAX_HANDLERS];
    int handler_count;

    /* User data pointer (for widget state, etc.) */
    void* user_data;
    SGNodeCleanupFn cleanup;
};

/* ========================================================================
 * Node Creation / Destruction
 * ======================================================================== */

SGNode* sg_node_create(SGNodeType type);
void    sg_node_destroy(SGNode* node);              /* Release one owner/reference */
void    sg_node_destroy_tree(SGNode* node);         /* Release root of subtree */
void    sg_node_destroy_single(SGNode* node);       /* Destroy only this node */
void    sg_node_retain(SGNode* node);
void    sg_node_set_cleanup(SGNode* node, SGNodeCleanupFn cleanup);

/* ========================================================================
 * Tree Operations
 * ======================================================================== */

void    sg_node_add_child(SGNode* parent, SGNode* child);
void    sg_node_insert_before(SGNode* parent, SGNode* child, SGNode* before);
void    sg_node_insert_after(SGNode* parent, SGNode* child, SGNode* after);
void    sg_node_remove_child(SGNode* parent, SGNode* child);
void    sg_node_remove_from_parent(SGNode* node);

/* Child iteration */
SGNode* sg_node_first_child(SGNode* node);
SGNode* sg_node_last_child(SGNode* node);
SGNode* sg_node_next(SGNode* node);
SGNode* sg_node_prev(SGNode* node);
int     sg_node_child_count(SGNode* node);

/* ========================================================================
 * Dirty Tracking
 * ======================================================================== */

void sg_node_mark_dirty(SGNode* node, uint32_t flags);
void sg_node_mark_layout_dirty(SGNode* node);
void sg_node_mark_paint_dirty(SGNode* node);
void sg_node_clear_dirty(SGNode* node);
int  sg_node_is_dirty(SGNode* node, uint32_t flags);

/* ========================================================================
 * Style Helpers
 * ======================================================================== */

void sg_style_init(SGStyle* style);  /* Set defaults */
void sg_style_set_background(SGNode* node, uint32_t color);
void sg_style_set_border(SGNode* node, uint32_t color, double width, double radius);
void sg_style_set_padding(SGNode* node, double top, double right, double bottom, double left);
void sg_style_set_padding_uniform(SGNode* node, double padding);
void sg_style_set_margin(SGNode* node, double top, double right, double bottom, double left);
void sg_style_set_margin_uniform(SGNode* node, double margin);
void sg_style_set_shadow(SGNode* node, double ox, double oy, double blur, uint32_t color);
void sg_style_set_opacity(SGNode* node, double opacity);
void sg_style_set_gradient(SGNode* node, SkiaShader gradient);

/* ========================================================================
 * Rendering
 * ======================================================================== */

/* Render entire scene graph to canvas */
void sg_render(SGNode* root, SkiaCanvas canvas);

/* Render only dirty nodes (partial redraw) */
void sg_render_dirty(SGNode* root, SkiaCanvas canvas);

/* ========================================================================
 * Hit Testing
 * ======================================================================== */

/* Find deepest interactive node at (x,y) in window coordinates */
SGNode* sg_hit_test(SGNode* root, float x, float y);

/* Check if a point is inside a node's bounds */
int sg_point_in_node(SGNode* node, float x, float y);

/* ========================================================================
 * Events
 * ======================================================================== */

void sg_node_on(SGNode* node, int event_type, SGEventHandler handler, void* user_data);
void sg_node_off(SGNode* node, int event_type);
void sg_node_emit(SGNode* node, int event_type, void* event_data);

/* ========================================================================
 * Layout Constraints (used by layout.h)
 * ======================================================================== */

typedef struct {
    float min_width, max_width;
    float min_height, max_height;
} SGConstraints;

/* Unconstrained defaults */
#define SG_CONSTRAINTS_NONE ((SGConstraints){ 0, 1e6f, 0, 1e6f })

/* ========================================================================
 * Node ID Generator
 * ======================================================================== */

uint32_t sg_next_id(void);

/* ========================================================================
 * Node Helpers
 * ======================================================================== */

void sg_node_set_visible(SGNode* node, int visible);
void sg_node_set_min_size(SGNode* node, double min_w, double min_h);
void sg_node_set_max_size(SGNode* node, double max_w, double max_h);
void sg_node_set_flex_grow(SGNode* node, double grow);
void sg_node_set_align_self(SGNode* node, int align);
void sg_node_set_align_items(SGNode* node, int align);

#ifdef __cplusplus
}
#endif

#endif /* SCENE_GRAPH_H */
