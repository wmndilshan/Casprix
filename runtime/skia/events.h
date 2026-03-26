/*
 * Event System — Mouse, keyboard, focus dispatch for Casperix UI
 *
 * Pipeline: OS Event → Window → Hit Test → Capture Phase → Bubble Phase
 *
 * Events flow through the scene graph in two phases:
 *   1. Capture: root → target (for intercepting)
 *   2. Bubble:  target → root (for handling)
 *
 * Focus management via Tab key cycling.
 * Mouse enter/leave tracking via hover pointer.
 */

#ifndef EVENTS_H
#define EVENTS_H

#include "scene_graph.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Event Types
 * ======================================================================== */

typedef enum {
    /* Mouse events */
    SG_EVENT_MOUSE_DOWN    = 1,
    SG_EVENT_MOUSE_UP      = 2,
    SG_EVENT_MOUSE_MOVE    = 3,
    SG_EVENT_MOUSE_ENTER   = 4,
    SG_EVENT_MOUSE_LEAVE   = 5,
    SG_EVENT_MOUSE_SCROLL  = 6,
    SG_EVENT_CLICK         = 7,
    SG_EVENT_DOUBLE_CLICK  = 8,

    /* Keyboard events */
    SG_EVENT_KEY_DOWN      = 10,
    SG_EVENT_KEY_UP        = 11,
    SG_EVENT_TEXT_INPUT    = 12,

    /* Focus events */
    SG_EVENT_FOCUS_IN      = 20,
    SG_EVENT_FOCUS_OUT     = 21,

    /* Window events */
    SG_EVENT_RESIZE        = 30,
    SG_EVENT_CLOSE         = 31,
} SGEventType;

/* Mouse buttons */
#define SG_BUTTON_LEFT    0
#define SG_BUTTON_RIGHT   1
#define SG_BUTTON_MIDDLE  2

/* Keyboard modifier flags */
#define SG_MOD_SHIFT   0x01
#define SG_MOD_CTRL    0x02
#define SG_MOD_ALT     0x04
#define SG_MOD_SUPER   0x08

/* Common virtual key codes (matching Win32 VK_ values) */
#define SG_KEY_BACKSPACE 0x08
#define SG_KEY_TAB       0x09
#define SG_KEY_RETURN    0x0D
#define SG_KEY_ESCAPE    0x1B
#define SG_KEY_SPACE     0x20
#define SG_KEY_LEFT      0x25
#define SG_KEY_UP        0x26
#define SG_KEY_RIGHT     0x27
#define SG_KEY_DOWN      0x28
#define SG_KEY_DELETE    0x2E
#define SG_KEY_HOME      0x24
#define SG_KEY_END       0x23
#define SG_KEY_A         0x41
#define SG_KEY_C         0x43
#define SG_KEY_V         0x56
#define SG_KEY_X         0x58
#define SG_KEY_Z         0x5A

/* ========================================================================
 * Event Data
 * ======================================================================== */

typedef struct {
    SGEventType type;
    SGNode* target;           /* Node that originally received the event */
    SGNode* current;          /* Node currently handling (during bubble) */
    int consumed;             /* Set to 1 to stop propagation */
    int mods;                 /* SG_MOD_* flags */
    double timestamp;         /* Event time in seconds */

    union {
        struct {
            float x, y;          /* Position in window coordinates */
            float local_x, local_y; /* Position relative to target node */
            int button;          /* SG_BUTTON_* */
            int clicks;          /* Click count (1=single, 2=double) */
        } mouse;

        struct {
            float dx, dy;        /* Scroll delta */
            float x, y;          /* Mouse position at scroll */
        } scroll;

        struct {
            int keycode;         /* Virtual key code */
            int scancode;        /* Hardware scan code */
            int repeat;          /* Is this a key repeat? */
        } key;

        struct {
            char text[8];        /* UTF-8 encoded character */
        } text_input;

        struct {
            int width, height;   /* New window dimensions */
        } resize;
    } data;
} SGEvent;

/* ========================================================================
 * Event Manager
 * ======================================================================== */

typedef struct {
    SGNode* root;             /* Scene graph root */

    /* State tracking */
    SGNode* hovered;          /* Currently hovered node */
    SGNode* focused;          /* Currently focused node */
    SGNode* pressed;          /* Node receiving mouse down (for drag) */
    SGNode* captured;         /* Mouse capture (receives all mouse events) */

    /* Mouse state */
    float mouse_x, mouse_y;  /* Current mouse position */
    int mouse_buttons;        /* Bitmask of held buttons */

    /* Double-click detection */
    double last_click_time;
    float last_click_x, last_click_y;
    int click_count;

    /* Focus tab order list (rebuilt on layout) */
    SGNode** tab_order;
    int tab_count;
    int tab_capacity;
} SGEventManager;

/* ========================================================================
 * Event Manager API
 * ======================================================================== */

SGEventManager* sg_event_manager_create(SGNode* root);
void            sg_event_manager_destroy(SGEventManager* mgr);
void            sg_event_manager_set_root(SGEventManager* mgr, SGNode* root);

/* ========================================================================
 * Event Dispatch
 * ======================================================================== */

/* Main dispatch entry — call this with OS events translated to SGEvent */
void sg_dispatch_event(SGEventManager* mgr, SGEvent* event);

/* Specific event dispatchers (called from window event handlers) */
void sg_dispatch_mouse_move(SGEventManager* mgr, float x, float y, int mods);
void sg_dispatch_mouse_down(SGEventManager* mgr, float x, float y, int button, int mods);
void sg_dispatch_mouse_up(SGEventManager* mgr, float x, float y, int button, int mods);
void sg_dispatch_mouse_scroll(SGEventManager* mgr, float x, float y, float dx, float dy, int mods);
void sg_dispatch_key_down(SGEventManager* mgr, int keycode, int scancode, int mods);
void sg_dispatch_key_up(SGEventManager* mgr, int keycode, int scancode, int mods);
void sg_dispatch_text_input(SGEventManager* mgr, const char* text);
void sg_dispatch_resize(SGEventManager* mgr, int width, int height);

/* ========================================================================
 * Focus Management
 * ======================================================================== */

/* Set focus to a specific node (sends FOCUS_OUT to old, FOCUS_IN to new) */
void sg_focus_set(SGEventManager* mgr, SGNode* node);

/* Clear focus */
void sg_focus_clear(SGEventManager* mgr);

/* Move focus to next/previous interactive node */
void sg_focus_next(SGEventManager* mgr);
void sg_focus_prev(SGEventManager* mgr);

/* Rebuild tab order from scene graph */
void sg_focus_rebuild_tab_order(SGEventManager* mgr);

/* Get currently focused node */
SGNode* sg_focus_get(SGEventManager* mgr);

/* ========================================================================
 * Mouse Capture
 * ======================================================================== */

/* Set mouse capture — all mouse events go to this node until released */
void sg_capture_set(SGEventManager* mgr, SGNode* node);
void sg_capture_release(SGEventManager* mgr);

/* ========================================================================
 * Cursor
 * ======================================================================== */

typedef enum {
    SG_CURSOR_ARROW,
    SG_CURSOR_HAND,
    SG_CURSOR_TEXT,
    SG_CURSOR_CROSSHAIR,
    SG_CURSOR_RESIZE_NS,
    SG_CURSOR_RESIZE_EW,
    SG_CURSOR_RESIZE_NWSE,
    SG_CURSOR_RESIZE_NESW,
    SG_CURSOR_MOVE,
    SG_CURSOR_NOT_ALLOWED,
} SGCursor;

/* Set the window cursor (platform-specific implementation) */
void sg_set_cursor(SGCursor cursor);

#ifdef __cplusplus
}
#endif

#endif /* EVENTS_H */
