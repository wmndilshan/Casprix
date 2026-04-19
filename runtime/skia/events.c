/*
 * Event System — Dispatch pipeline for Casperix UI
 *
 * OS events → hit test → capture/bubble through scene graph.
 * Tracks hover, focus, and pressed state.
 */

#include "events.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* ========================================================================
 * Double-click threshold
 * ======================================================================== */

#define DOUBLE_CLICK_TIME   0.4    /* seconds */
#define DOUBLE_CLICK_DIST   4.0f   /* pixels */

static SGEventManager* g_active_focus_manager = NULL;

static int sg_node_is_focusable(const SGNode* node) {
    if (!node) return 0;
    if (!(node->flags & SG_VISIBLE)) return 0;
    return (node->flags & SG_INTERACTIVE) != 0;
}

/* ========================================================================
 * Creation / Destruction
 * ======================================================================== */

SGEventManager* sg_event_manager_create(SGNode* root) {
    SGEventManager* mgr = (SGEventManager*)calloc(1, sizeof(SGEventManager));
    if (!mgr) return NULL;
    mgr->root = root;
    mgr->tab_capacity = 64;
    mgr->tab_order = (SGNode**)calloc(mgr->tab_capacity, sizeof(SGNode*));
    if (!g_active_focus_manager) {
        g_active_focus_manager = mgr;
    }
    return mgr;
}

void sg_event_manager_destroy(SGEventManager* mgr) {
    if (!mgr) return;
    if (g_active_focus_manager == mgr) {
        g_active_focus_manager = NULL;
    }
    free(mgr->tab_order);
    free(mgr);
}

void sg_event_manager_set_root(SGEventManager* mgr, SGNode* root) {
    if (!mgr) return;
    g_active_focus_manager = mgr;
    mgr->root = root;
    mgr->hovered = NULL;
    mgr->focused = NULL;
    mgr->pressed = NULL;
    mgr->captured = NULL;
    sg_focus_rebuild_tab_order(mgr);
}

/* ========================================================================
 * Internal: Build path from root to target
 * ======================================================================== */

/* Build ancestor path from root to node.
 * Returns path length. Path is root → ... → node. */
static int build_path(SGNode* node, SGNode** path, int max_depth) {
    int depth = 0;
    SGNode* n = node;

    /* Count depth */
    while (n && depth < max_depth) {
        path[depth++] = n;
        n = n->parent;
    }

    /* Reverse to get root → target order */
    for (int i = 0; i < depth / 2; i++) {
        SGNode* tmp = path[i];
        path[i] = path[depth - 1 - i];
        path[depth - 1 - i] = tmp;
    }

    return depth;
}

/* ========================================================================
 * Internal: Dispatch through capture + bubble phases
 * ======================================================================== */

static void dispatch_through_path(SGNode* target, SGEvent* event) {
    SGNode* path[64];
    int depth = build_path(target, path, 64);

    event->target = target;

    /* Capture phase: root → target */
    for (int i = 0; i < depth && !event->consumed; i++) {
        event->current = path[i];
        sg_node_emit(path[i], event->type, event);
    }

    /* If already consumed during capture, done.
     * If target was already emitted in capture, skip bubble duplicate.
     * Bubble phase: target-1 → root (target already handled in capture) */
    for (int i = depth - 2; i >= 0 && !event->consumed; i--) {
        event->current = path[i];
        sg_node_emit(path[i], event->type, event);
    }
}

/* ========================================================================
 * Mouse Move
 * ======================================================================== */

void sg_dispatch_mouse_move(SGEventManager* mgr, float x, float y, int mods) {
    if (!mgr || !mgr->root) return;

    mgr->mouse_x = x;
    mgr->mouse_y = y;

    /* Hit test to find what's under the cursor */
    SGNode* target = mgr->captured ? mgr->captured : sg_hit_test(mgr->root, x, y);

    /* Handle enter/leave */
    if (target != mgr->hovered) {
        /* Mouse leave old node */
        if (mgr->hovered) {
            SGEvent leave = { 0 };
            leave.type = SG_EVENT_MOUSE_LEAVE;
            leave.data.mouse.x = x;
            leave.data.mouse.y = y;
            leave.mods = mods;
            leave.target = mgr->hovered;
            leave.current = mgr->hovered;
            sg_node_emit(mgr->hovered, SG_EVENT_MOUSE_LEAVE, &leave);
        }

        /* Mouse enter new node */
        if (target) {
            SGEvent enter = { 0 };
            enter.type = SG_EVENT_MOUSE_ENTER;
            enter.data.mouse.x = x;
            enter.data.mouse.y = y;
            enter.mods = mods;
            enter.target = target;
            enter.current = target;
            sg_node_emit(target, SG_EVENT_MOUSE_ENTER, &enter);
        }

        mgr->hovered = target;
    }

    /* Mouse move event */
    if (target) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_MOUSE_MOVE;
        evt.data.mouse.x = x;
        evt.data.mouse.y = y;
        evt.data.mouse.local_x = x - target->bounds.x;
        evt.data.mouse.local_y = y - target->bounds.y;
        evt.mods = mods;
        dispatch_through_path(target, &evt);
    }
}

/* ========================================================================
 * Mouse Down / Up
 * ======================================================================== */

void sg_dispatch_mouse_down(SGEventManager* mgr, float x, float y, int button, int mods) {
    if (!mgr || !mgr->root) return;

    SGNode* target = mgr->captured ? mgr->captured : sg_hit_test(mgr->root, x, y);

    /* Update focus */
    sg_focus_set(mgr, target);

    mgr->pressed = target;
    mgr->mouse_buttons |= (1 << button);

    if (target) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_MOUSE_DOWN;
        evt.data.mouse.x = x;
        evt.data.mouse.y = y;
        evt.data.mouse.local_x = x - target->bounds.x;
        evt.data.mouse.local_y = y - target->bounds.y;
        evt.data.mouse.button = button;
        evt.mods = mods;
        dispatch_through_path(target, &evt);
    }
}

void sg_dispatch_mouse_up(SGEventManager* mgr, float x, float y, int button, int mods) {
    if (!mgr || !mgr->root) return;

    SGNode* target = mgr->captured ? mgr->captured : sg_hit_test(mgr->root, x, y);

    mgr->mouse_buttons &= ~(1 << button);

    if (target) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_MOUSE_UP;
        evt.data.mouse.x = x;
        evt.data.mouse.y = y;
        evt.data.mouse.local_x = x - target->bounds.x;
        evt.data.mouse.local_y = y - target->bounds.y;
        evt.data.mouse.button = button;
        evt.mods = mods;
        dispatch_through_path(target, &evt);

        /* Generate click if mouse up on the same node as mouse down */
        if (target == mgr->pressed && button == SG_BUTTON_LEFT) {
            /* Check for double click */
            double now = evt.timestamp;
            float dx = x - mgr->last_click_x;
            float dy = y - mgr->last_click_y;
            float dist = sqrtf(dx * dx + dy * dy);

            if (now - mgr->last_click_time < DOUBLE_CLICK_TIME &&
                dist < DOUBLE_CLICK_DIST) {
                mgr->click_count++;
            } else {
                mgr->click_count = 1;
            }
            mgr->last_click_time = now;
            mgr->last_click_x = x;
            mgr->last_click_y = y;

            SGEvent click = { 0 };
            click.type = (mgr->click_count >= 2) ? SG_EVENT_DOUBLE_CLICK : SG_EVENT_CLICK;
            click.data.mouse.x = x;
            click.data.mouse.y = y;
            click.data.mouse.local_x = x - target->bounds.x;
            click.data.mouse.local_y = y - target->bounds.y;
            click.data.mouse.button = button;
            click.data.mouse.clicks = mgr->click_count;
            click.mods = mods;
            dispatch_through_path(target, &click);
        }
    }

    mgr->pressed = NULL;
}

/* ========================================================================
 * Mouse Scroll
 * ======================================================================== */

void sg_dispatch_mouse_scroll(SGEventManager* mgr, float x, float y,
                               float dx, float dy, int mods) {
    if (!mgr || !mgr->root) return;

    SGNode* target = sg_hit_test(mgr->root, x, y);
    if (target) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_MOUSE_SCROLL;
        evt.data.scroll.x = x;
        evt.data.scroll.y = y;
        evt.data.scroll.dx = dx;
        evt.data.scroll.dy = dy;
        evt.mods = mods;
        dispatch_through_path(target, &evt);
    }
}

/* ========================================================================
 * Keyboard Events
 * ======================================================================== */

void sg_dispatch_key_down(SGEventManager* mgr, int keycode, int scancode, int mods) {
    if (!mgr) return;

    /* Tab key cycles focus */
    if (keycode == SG_KEY_TAB) {
        if (mods & SG_MOD_SHIFT) {
            sg_focus_prev(mgr);
        } else {
            sg_focus_next(mgr);
        }
        return;
    }

    SGNode* target = mgr->focused;
    if (target) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_KEY_DOWN;
        evt.data.key.keycode = keycode;
        evt.data.key.scancode = scancode;
        evt.mods = mods;
        dispatch_through_path(target, &evt);
    }
}

void sg_dispatch_key_up(SGEventManager* mgr, int keycode, int scancode, int mods) {
    if (!mgr) return;

    SGNode* target = mgr->focused;
    if (target) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_KEY_UP;
        evt.data.key.keycode = keycode;
        evt.data.key.scancode = scancode;
        evt.mods = mods;
        dispatch_through_path(target, &evt);
    }
}

void sg_dispatch_text_input(SGEventManager* mgr, const char* text) {
    if (!mgr || !text) return;

    SGNode* target = mgr->focused;
    if (target) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_TEXT_INPUT;
        /* Copy up to 7 bytes of UTF-8 */
        int len = (int)strlen(text);
        if (len > 7) len = 7;
        memcpy(evt.data.text_input.text, text, len);
        evt.data.text_input.text[len] = '\0';
        dispatch_through_path(target, &evt);
    }
}

/* ========================================================================
 * Window Resize
 * ======================================================================== */

void sg_dispatch_resize(SGEventManager* mgr, int width, int height) {
    if (!mgr || !mgr->root) return;

    SGEvent evt = { 0 };
    evt.type = SG_EVENT_RESIZE;
    evt.data.resize.width = width;
    evt.data.resize.height = height;
    evt.target = mgr->root;
    evt.current = mgr->root;
    sg_node_emit(mgr->root, SG_EVENT_RESIZE, &evt);

    /* Mark root as needing layout */
    sg_node_mark_dirty(mgr->root, SG_DIRTY_LAYOUT);
}

/* ========================================================================
 * Generic Dispatch
 * ======================================================================== */

void sg_dispatch_event(SGEventManager* mgr, SGEvent* event) {
    if (!mgr || !event) return;

    switch (event->type) {
        case SG_EVENT_MOUSE_MOVE:
            sg_dispatch_mouse_move(mgr, event->data.mouse.x, event->data.mouse.y,
                                    event->mods);
            break;
        case SG_EVENT_MOUSE_DOWN:
            sg_dispatch_mouse_down(mgr, event->data.mouse.x, event->data.mouse.y,
                                    event->data.mouse.button, event->mods);
            break;
        case SG_EVENT_MOUSE_UP:
            sg_dispatch_mouse_up(mgr, event->data.mouse.x, event->data.mouse.y,
                                  event->data.mouse.button, event->mods);
            break;
        case SG_EVENT_MOUSE_SCROLL:
            sg_dispatch_mouse_scroll(mgr, event->data.scroll.x, event->data.scroll.y,
                                      event->data.scroll.dx, event->data.scroll.dy,
                                      event->mods);
            break;
        case SG_EVENT_KEY_DOWN:
            sg_dispatch_key_down(mgr, event->data.key.keycode, event->data.key.scancode,
                                  event->mods);
            break;
        case SG_EVENT_KEY_UP:
            sg_dispatch_key_up(mgr, event->data.key.keycode, event->data.key.scancode,
                                event->mods);
            break;
        case SG_EVENT_TEXT_INPUT:
            sg_dispatch_text_input(mgr, event->data.text_input.text);
            break;
        case SG_EVENT_RESIZE:
            sg_dispatch_resize(mgr, event->data.resize.width, event->data.resize.height);
            break;
        default:
            break;
    }
}

/* ========================================================================
 * Focus Management
 * ======================================================================== */

void sg_focus_set(SGEventManager* mgr, SGNode* node) {
    if (!mgr) return;
    g_active_focus_manager = mgr;
    if (!sg_node_is_focusable(node)) {
        node = NULL;
    }
    if (mgr->focused == node) return;

    /* Send focus out to old node */
    if (mgr->focused) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_FOCUS_OUT;
        evt.target = mgr->focused;
        evt.current = mgr->focused;
        sg_node_emit(mgr->focused, SG_EVENT_FOCUS_OUT, &evt);
    }

    mgr->focused = node;

    /* Send focus in to new node */
    if (node) {
        SGEvent evt = { 0 };
        evt.type = SG_EVENT_FOCUS_IN;
        evt.target = node;
        evt.current = node;
        sg_node_emit(node, SG_EVENT_FOCUS_IN, &evt);
    }
}

void sg_focus_clear(SGEventManager* mgr) {
    sg_focus_set(mgr, NULL);
}

SGNode* sg_focus_get(SGEventManager* mgr) {
    return mgr ? mgr->focused : NULL;
}

SGEventManager* sg_focus_manager_get_active(void) {
    return g_active_focus_manager;
}

SGNode* sg_focus_get_global(void) {
    return g_active_focus_manager ? g_active_focus_manager->focused : NULL;
}

/* Recursive helper to collect focusable nodes in DFS order */
static void collect_focusable(SGNode* node, SGEventManager* mgr) {
    if (!node) return;
    if (!(node->flags & SG_VISIBLE)) return;

    if (node->flags & SG_INTERACTIVE) {
        if (mgr->tab_count >= mgr->tab_capacity) {
            mgr->tab_capacity *= 2;
            mgr->tab_order = (SGNode**)realloc(mgr->tab_order,
                                                mgr->tab_capacity * sizeof(SGNode*));
        }
        mgr->tab_order[mgr->tab_count++] = node;
    }

    for (SGNode* c = node->first_child; c; c = c->next_sibling) {
        collect_focusable(c, mgr);
    }
}

void sg_focus_rebuild_tab_order(SGEventManager* mgr) {
    if (!mgr) return;
    mgr->tab_count = 0;
    collect_focusable(mgr->root, mgr);
}

void sg_focus_next(SGEventManager* mgr) {
    if (!mgr || mgr->tab_count == 0) return;

    /* Find current index */
    int idx = -1;
    for (int i = 0; i < mgr->tab_count; i++) {
        if (mgr->tab_order[i] == mgr->focused) {
            idx = i;
            break;
        }
    }

    int next = (idx + 1) % mgr->tab_count;
    sg_focus_set(mgr, mgr->tab_order[next]);
}

void sg_focus_prev(SGEventManager* mgr) {
    if (!mgr || mgr->tab_count == 0) return;

    int idx = -1;
    for (int i = 0; i < mgr->tab_count; i++) {
        if (mgr->tab_order[i] == mgr->focused) {
            idx = i;
            break;
        }
    }

    int prev = (idx <= 0) ? mgr->tab_count - 1 : idx - 1;
    sg_focus_set(mgr, mgr->tab_order[prev]);
}

/* ========================================================================
 * Mouse Capture
 * ======================================================================== */

void sg_capture_set(SGEventManager* mgr, SGNode* node) {
    if (mgr) mgr->captured = node;
}

void sg_capture_release(SGEventManager* mgr) {
    if (mgr) mgr->captured = NULL;
}

/* ========================================================================
 * Cursor (Win32 implementation)
 * ======================================================================== */

void sg_set_cursor(SGCursor cursor) {
#ifdef _WIN32
    LPCSTR name;
    switch (cursor) {
        case SG_CURSOR_HAND:        name = IDC_HAND;     break;
        case SG_CURSOR_TEXT:        name = IDC_IBEAM;    break;
        case SG_CURSOR_CROSSHAIR:  name = IDC_CROSS;    break;
        case SG_CURSOR_RESIZE_NS:  name = IDC_SIZENS;   break;
        case SG_CURSOR_RESIZE_EW:  name = IDC_SIZEWE;   break;
        case SG_CURSOR_RESIZE_NWSE:name = IDC_SIZENWSE; break;
        case SG_CURSOR_RESIZE_NESW:name = IDC_SIZENESW; break;
        case SG_CURSOR_MOVE:       name = IDC_SIZEALL;  break;
        case SG_CURSOR_NOT_ALLOWED:name = IDC_NO;       break;
        default:                   name = IDC_ARROW;    break;
    }
    SetCursor(LoadCursor(NULL, name));
#endif
}
