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
#include <stddef.h>

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
 * Accessibility role — drives the Android class name / TalkBack behaviour
 * reported by the a11y bridge (runtime/android/a11y_bridge.c). Set on a node
 * with sg_node_set_a11y_role(); widget constructors set it automatically.
 * ======================================================================== */
typedef enum {
    SG_A11Y_ROLE_NONE = 0,  /* not surfaced as an actionable/readable node */
    SG_A11Y_ROLE_TEXT,      /* android.widget.TextView */
    SG_A11Y_ROLE_HEADER,    /* android.widget.TextView + heading flag */
    SG_A11Y_ROLE_BUTTON,    /* android.widget.Button */
    SG_A11Y_ROLE_EDIT_TEXT, /* android.widget.EditText */
    SG_A11Y_ROLE_IMAGE,     /* android.widget.ImageView */
    SG_A11Y_ROLE_CHECKBOX,  /* android.widget.CheckBox */
} SGA11yRole;

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
typedef struct SGScene SGScene;
typedef struct SGArena SGArena;
typedef struct SGStyleRef SGStyleRef;

typedef void (*SGEventHandler)(SGNode* node, int event_type, void* event_data, void* user_data);

typedef struct {
    int event_type;
    SGEventHandler handler;
    void* user_data;
} SGHandlerSlot;

typedef void (*SGNodeCleanupFn)(SGNode* node);

/* ========================================================================
 * Modern Lifecycle + Arena API
 * ======================================================================== */

typedef struct {
    float min_w;
    float max_w;
    float min_h;
    float max_h;
} SGMeasureConstraints;

struct SGArena {
    uint8_t* base;
    size_t   capacity;
    size_t   offset;
};

typedef struct SGWidgetVTable {
    void (*init)(SGScene* scene, SGNode* node);
    void (*measure_layout)(SGScene* scene, SGNode* node,
                           const SGMeasureConstraints* constraints,
                           float* out_w, float* out_h);
    void (*paint)(SGScene* scene, SGNode* node, SkiaCanvas canvas, SGRect clip);
    void (*destroy)(SGScene* scene, SGNode* node);
} SGWidgetVTable;

typedef struct {
    SGNode* root;
    SGRect  dirty_rects[1024];
    uint32_t dirty_count;
    uint32_t frame_id;
} SGFrameState;

struct SGScene {
    SGArena arena;
    SGFrameState frame;
    SGNode* root;

    /* Reusable paints to avoid per-frame allocations. */
    SkiaPaint fill_paint;
    SkiaPaint stroke_paint;
    SkiaPaint text_paint;
    SkiaPaint shadow_paint;
};

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
            int wrap;             /* 0 = single line (default), 1 = word-wrap to bounded width */
            int max_lines;        /* 0 = unlimited (default); >0 truncates to N lines (no ellipsis in v1) */
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

    uint32_t layout_version;      /* Bumped when this node is layout-dirtied */
    int      dirty_rect_valid;
    SGRect   dirty_rect;          /* Unioned invalid rect in local space */

    /* State */
    uint32_t flags;               /* SG_DIRTY_* | SG_VISIBLE | ... */
    int z_index;                  /* Rendering order (higher = on top) */

    /* Events */
    SGHandlerSlot handlers[SG_MAX_HANDLERS];
    int handler_count;

    /* User data pointer (for widget state, etc.) */
    void* user_data;
    SGNodeCleanupFn cleanup;

    /* Accessibility (v1b). All additive; zero-initialised, so a node with no
     * a11y intent behaves exactly as before. */
    char*      a11y_label;    /* explicit content-description; NULL -> derive
                                from a child SG_NODE_TEXT */
    SGA11yRole a11y_role;     /* SG_A11Y_ROLE_* (default NONE) */
    int        a11y_hidden;   /* 1 -> node (but not its children) is excluded
                                from the a11y tree; decorative-only nodes */

    /* Optional modern lifecycle hooks (kept additive for compatibility). */
    const SGWidgetVTable* lifecycle;
    uint32_t              state_flags;
    SGStyleRef*           style_ref;
    SGScene*              scene_owner;
};

enum {
    SG_STATE_HOVER    = 1u << 0,
    SG_STATE_ACTIVE   = 1u << 1,
    SG_STATE_FOCUSED  = 1u << 2,
    SG_STATE_DISABLED = 1u << 3,
    SG_STATE_CHECKED  = 1u << 4
};

/* ========================================================================
 * Node Creation / Destruction
 * ======================================================================== */

SGNode* sg_node_create(SGNodeType type);
SGNode* sg_node_create_in_scene(SGScene* scene, SGNodeType type);
void    sg_node_destroy(SGNode* node);              /* Release one owner/reference */
void    sg_node_destroy_tree(SGNode* node);         /* Release root of subtree */
void    sg_node_destroy_single(SGNode* node);       /* Destroy only this node */
void    sg_node_retain(SGNode* node);
void    sg_node_set_cleanup(SGNode* node, SGNodeCleanupFn cleanup);

/* ========================================================================
 * Tree Operations
 * ======================================================================== */

/* Attach 'child' as the last child of 'parent'.
 * Returns 0 on success, -1 if the attachment was rejected because it would
 * create a cycle (child is 'parent' itself or an ancestor of 'parent') or the
 * arguments are NULL. Existing callers that ignore the return value are
 * unaffected. */
int     sg_node_add_child(SGNode* parent, SGNode* child);

/* Same rejection contract as sg_node_add_child (return 0 / -1). */
int     sg_node_insert_before(SGNode* parent, SGNode* child, SGNode* before);
int     sg_node_insert_after(SGNode* parent, SGNode* child, SGNode* after);
void    sg_node_remove_child(SGNode* parent, SGNode* child);
void    sg_node_remove_from_parent(SGNode* node);

/* True if 'maybe_ancestor' is 'node' or lies on 'node's parent chain. Used by
 * the tree-op cycle guard; also useful for callers doing their own checks. */
int     sg_node_is_ancestor(const SGNode* maybe_ancestor, const SGNode* node);

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

/* Aliases / helpers for dirty-region workflow */
void     sg_mark_dirty(SGNode* node);
void     sg_mark_dirty_rect(SGNode* node, SGRect rect);
int      sg_is_subtree_dirty(const SGNode* node);
void     sg_clear_dirty(SGNode* node);
void     sg_clear_dirty_recursive(SGNode* root);

/* Debug: number of nodes that executed paint body in last sg_render_dirty_only */
uint32_t sg_debug_paint_node_count(void);
void     sg_debug_reset_paint_node_count(void);

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
 * Modern Scene Lifecycle Helpers
 * ======================================================================== */

SGScene* sg_scene_create(void* arena_memory, size_t arena_capacity);
void     sg_scene_destroy(SGScene* scene);
void     sg_scene_begin_frame(SGScene* scene);
void     sg_scene_register_root(SGScene* scene, SGNode* root);

void* sg_scene_alloc(SGScene* scene, size_t size, size_t align);
int   sg_node_set_lifecycle(SGNode* node, const SGWidgetVTable* lifecycle);
void  sg_node_set_style_ref(SGNode* node, SGStyleRef* style_ref);
void  sg_node_run_init(SGNode* node);
void  sg_node_run_destroy(SGNode* node);
void  sg_node_run_measure(SGNode* node, const SGMeasureConstraints* constraints,
                          float* out_w, float* out_h);
void  sg_node_run_paint(SGNode* node, SkiaCanvas canvas, SGRect clip);

void sg_scene_mark_dirty_rect(SGScene* scene, SGRect rect);
void sg_scene_mark_node_dirty(SGNode* node);

/* ========================================================================
 * Rendering
 * ======================================================================== */

/* Render entire scene graph to canvas */
void sg_render(SGNode* root, SkiaCanvas canvas);

/* Render only dirty nodes (partial redraw) */
void sg_render_dirty(SGNode* root, SkiaCanvas canvas);

/* Selective repaint: counts painted nodes via sg_debug_paint_node_count */
void sg_render_dirty_only(SGNode* root, SkiaCanvas canvas);

/* ========================================================================
 * Hit Testing
 * ======================================================================== */

/* Find deepest interactive node at (x,y) in window coordinates */
SGNode* sg_hit_test(SGNode* root, float x, float y);

/* Convert a window-space point into coordinates relative to node bounds. */
int sg_node_world_to_local(SGNode* node, float world_x, float world_y,
                           float* local_x, float* local_y);

/* Check if a point is inside a node's bounds */
int sg_point_in_node(SGNode* node, float x, float y);

/* ========================================================================
 * Events
 * ======================================================================== */

void sg_node_on(SGNode* node, int event_type, SGEventHandler handler, void* user_data);
void sg_node_off(SGNode* node, int event_type);
void sg_node_emit(SGNode* node, int event_type, void* event_data);

/* ========================================================================
 * Accessibility (v1b) — node annotations + structural-change notification
 * ======================================================================== */

/* Explicit accessibility label. Copied; pass NULL to clear (falls back to a
 * child text node). Fires a structural-change notification. */
void       sg_node_set_a11y_label(SGNode* node, const char* label);
const char* sg_node_get_a11y_label(const SGNode* node);

void       sg_node_set_a11y_role(SGNode* node, SGA11yRole role);
SGA11yRole sg_node_get_a11y_role(const SGNode* node);

/* Hide just this node (children still surface). Fires a structural-change
 * notification. */
void       sg_node_set_a11y_hidden(SGNode* node, int hidden);
int        sg_node_get_a11y_hidden(const SGNode* node);

/* Structural-change hook. The a11y bridge registers a callback here; it is
 * invoked (at most once per coalescing window — see the .c) when the tree
 * shape or a label changes, NOT on paint/layout dirty. Passing NULL clears. */
typedef void (*SGA11yStructuralChangeFn)(void* user_data);
void sg_a11y_set_structural_change_cb(SGA11yStructuralChangeFn cb, void* user_data);

/* Manually signal a structural change (e.g. after a batch of edits). Safe to
 * call with no callback registered. */
void sg_a11y_notify_structural_change(void);

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
