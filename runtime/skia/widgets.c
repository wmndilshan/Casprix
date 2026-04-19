/*
 * Core Widgets — Factory functions that create configured SGNodes
 *
 * Each widget creates a scene graph node (or small subtree),
 * attaches event handlers, and stores widget-specific state
 * in the node's user_data pointer.
 */

#include "widgets.h"
#include "layout.h"
#include "skia_c.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WIDGET_FOCUS_RING_COLOR  0xFF1A73E8
#define WIDGET_FOCUS_RING_SHADOW 8.0f
#define WIDGET_FOCUS_RING_WIDTH  2.0f

/* ========================================================================
 * Internal Utilities
 * ======================================================================== */

static uint32_t color_lighten(uint32_t c, int amount) {
    int a = (c >> 24) & 0xFF;
    int r = ((c >> 16) & 0xFF) + amount; if (r > 255) r = 255;
    int g = ((c >>  8) & 0xFF) + amount; if (g > 255) g = 255;
    int b = ((c      ) & 0xFF) + amount; if (b > 255) b = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t color_darken(uint32_t c, int amount) {
    int a = (c >> 24) & 0xFF;
    int r = ((c >> 16) & 0xFF) - amount; if (r < 0) r = 0;
    int g = ((c >>  8) & 0xFF) - amount; if (g < 0) g = 0;
    int b = ((c      ) & 0xFF) - amount; if (b < 0) b = 0;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

typedef struct {
    WidgetType type;    /* WIDGET_TAB_PANEL */
    SGNode**   contents;
    int        count;
    int        active;
    SGNode*    header;
} TabPanelState;

static void widget_apply_focus_ring(SGNode* node,
                                    int* focus_visible,
                                    uint32_t* saved_border_color,
                                    float* saved_border_width,
                                    int* saved_elevation,
                                    float* saved_shadow_offset_x,
                                    float* saved_shadow_offset_y,
                                    float* saved_shadow_blur,
                                    uint32_t* saved_shadow_color) {
    if (!node || !focus_visible || *focus_visible) return;

    *saved_border_color = node->style.border_color;
    *saved_border_width = node->style.border_width;
    *saved_elevation = node->style.elevation;
    *saved_shadow_offset_x = node->style.shadow_offset_x;
    *saved_shadow_offset_y = node->style.shadow_offset_y;
    *saved_shadow_blur = node->style.shadow_blur;
    *saved_shadow_color = node->style.shadow_color;
    *focus_visible = 1;

    node->style.border_color = WIDGET_FOCUS_RING_COLOR;
    if (node->style.border_width < WIDGET_FOCUS_RING_WIDTH) {
        node->style.border_width = WIDGET_FOCUS_RING_WIDTH;
    }
    node->style.elevation = 0;
    node->style.shadow_offset_x = 0.0f;
    node->style.shadow_offset_y = 0.0f;
    node->style.shadow_blur = WIDGET_FOCUS_RING_SHADOW;
    node->style.shadow_color = 0x401A73E8;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

static void widget_restore_focus_ring(SGNode* node,
                                      int* focus_visible,
                                      uint32_t* saved_border_color,
                                      float* saved_border_width,
                                      int* saved_elevation,
                                      float* saved_shadow_offset_x,
                                      float* saved_shadow_offset_y,
                                      float* saved_shadow_blur,
                                      uint32_t* saved_shadow_color) {
    if (!node || !focus_visible || !*focus_visible) return;

    node->style.border_color = *saved_border_color;
    node->style.border_width = *saved_border_width;
    node->style.elevation = *saved_elevation;
    node->style.shadow_offset_x = *saved_shadow_offset_x;
    node->style.shadow_offset_y = *saved_shadow_offset_y;
    node->style.shadow_blur = *saved_shadow_blur;
    node->style.shadow_color = *saved_shadow_color;
    *focus_visible = 0;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Button Widget
 * ======================================================================== */

static void button_on_mouse_enter(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;
    state->state = BUTTON_HOVERED;
    node->style.background = state->hover_color;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

static void button_on_mouse_leave(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;
    state->state = BUTTON_NORMAL;
    node->style.background = state->normal_color;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

static void button_on_mouse_down(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;
    state->state = BUTTON_PRESSED;
    node->style.background = state->press_color;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

static void button_on_click(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;
    state->state = BUTTON_HOVERED;
    node->style.background = state->hover_color;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);

    if (state->on_click) {
        state->on_click(node, state->callback_data);
    }
}

static void button_on_focus_in(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;
    widget_apply_focus_ring(node, &state->focus_visible,
                            &state->saved_border_color, &state->saved_border_width,
                            &state->saved_elevation, &state->saved_shadow_offset_x,
                            &state->saved_shadow_offset_y, &state->saved_shadow_blur,
                            &state->saved_shadow_color);
}

static void button_on_focus_out(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state) return;
    widget_restore_focus_ring(node, &state->focus_visible,
                              &state->saved_border_color, &state->saved_border_width,
                              &state->saved_elevation, &state->saved_shadow_offset_x,
                              &state->saved_shadow_offset_y, &state->saved_shadow_blur,
                              &state->saved_shadow_color);
}

static void button_on_key_down(SGNode* node, int event_type, void* event_data, void* udata) {
    SGEvent* evt = (SGEvent*)event_data;
    ButtonWidgetState* state;

    (void)event_type; (void)udata;
    if (!evt) return;

    state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;

    if (evt->data.key.keycode == SG_KEY_RETURN || evt->data.key.keycode == SG_KEY_SPACE) {
        button_on_click(node, SG_EVENT_CLICK, event_data, udata);
        evt->consumed = 1;
    }
}

SGNode* widget_button(const char* label, SkiaFont font,
                       ButtonClickCallback on_click, void* user_data) {
    /* Create button container */
    SGNode* btn = sg_node_create(SG_NODE_CONTAINER);
    btn->layout_type = SG_LAYOUT_ROW;
    btn->justify = SG_JUSTIFY_CENTER;
    btn->align_items = SG_ALIGN_CENTER;
    btn->flags |= SG_INTERACTIVE;

    /* Default button style */
    btn->style.background = 0xFF4285F4;     /* Google blue */
    btn->style.border_radius = 6.0f;
    btn->style.padding[0] = 8.0f;           /* top */
    btn->style.padding[1] = 16.0f;          /* right */
    btn->style.padding[2] = 8.0f;           /* bottom */
    btn->style.padding[3] = 16.0f;          /* left */
    btn->min_height = 36.0f;
    
    /* BUGFIX: Prevent button resize on state changes */
    btn->flex_shrink = 0.0f;  /* Don't shrink below content size */

    /* Create label child */
    SGNode* text_node = widget_text(label, font, 0xFFFFFFFF);
    sg_node_add_child(btn, text_node);

    /* Widget state */
    ButtonWidgetState* state = (ButtonWidgetState*)calloc(1, sizeof(ButtonWidgetState));
    state->type = WIDGET_BUTTON;
    state->state = BUTTON_NORMAL;
    state->normal_color = 0xFF4285F4;
    state->hover_color = color_lighten(0xFF4285F4, 20);
    state->press_color = color_darken(0xFF4285F4, 30);
    state->disabled_color = 0xFFBDBDBD;
    state->text_color = 0xFFFFFFFF;
    state->on_click = on_click;
    state->callback_data = user_data;
    btn->user_data = state;

    /* Event handlers */
    sg_node_on(btn, SG_EVENT_MOUSE_ENTER, button_on_mouse_enter, NULL);
    sg_node_on(btn, SG_EVENT_MOUSE_LEAVE, button_on_mouse_leave, NULL);
    sg_node_on(btn, SG_EVENT_MOUSE_DOWN, button_on_mouse_down, NULL);
    sg_node_on(btn, SG_EVENT_CLICK, button_on_click, NULL);
    sg_node_on(btn, SG_EVENT_FOCUS_IN, button_on_focus_in, NULL);
    sg_node_on(btn, SG_EVENT_FOCUS_OUT, button_on_focus_out, NULL);
    sg_node_on(btn, SG_EVENT_KEY_DOWN, button_on_key_down, NULL);

    return btn;
}

void widget_button_set_colors(SGNode* btn, uint32_t normal, uint32_t hover,
                               uint32_t press, uint32_t text) {
    ButtonWidgetState* state = (ButtonWidgetState*)btn->user_data;
    if (!state || state->type != WIDGET_BUTTON) return;
    state->normal_color = normal;
    state->hover_color = hover;
    state->press_color = press;
    state->text_color = text;
    btn->style.background = normal;

    /* Update label color */
    SGNode* label = btn->first_child;
    if (label && label->type == SG_NODE_TEXT) {
        label->data.text.color = text;
    }
    sg_node_mark_dirty(btn, SG_DIRTY_PAINT);
}

void widget_button_set_enabled(SGNode* btn, int enabled) {
    ButtonWidgetState* state = (ButtonWidgetState*)btn->user_data;
    if (!state || state->type != WIDGET_BUTTON) return;
    if (enabled) {
        state->state = BUTTON_NORMAL;
        btn->style.background = state->normal_color;
        btn->flags |= SG_INTERACTIVE;
    } else {
        state->state = BUTTON_DISABLED;
        btn->style.background = state->disabled_color;
        btn->flags &= ~SG_INTERACTIVE;
    }
    sg_node_mark_dirty(btn, SG_DIRTY_PAINT);
}

void widget_button_set_callback(SGNode* btn, ButtonClickCallback on_click, void* user_data) {
    ButtonWidgetState* state = (ButtonWidgetState*)btn->user_data;
    if (!state || state->type != WIDGET_BUTTON) return;
    state->on_click = on_click;
    state->callback_data = user_data;
}

ButtonState widget_button_get_state(SGNode* btn) {
    ButtonWidgetState* state = (ButtonWidgetState*)btn->user_data;
    if (!state || state->type != WIDGET_BUTTON) return BUTTON_NORMAL;
    return state->state;
}

/* ========================================================================
 * Text Widget
 * ======================================================================== */

SGNode* widget_text(const char* text, SkiaFont font, uint32_t color) {
    SGNode* node = sg_node_create(SG_NODE_TEXT);
    if (text) {
        node->data.text.text = (char*)malloc(strlen(text) + 1);
        strcpy(node->data.text.text, text);
    }
    node->data.text.font = font;
    node->ownership_flags &= ~SG_NODE_OWNS_FONT;
    node->data.text.color = color;
    node->data.text.align = SG_TEXT_ALIGN_LEFT;
    return node;
}

void widget_text_set(SGNode* node, const char* text) {
    if (!node || node->type != SG_NODE_TEXT) return;
    free(node->data.text.text);
    if (text) {
        node->data.text.text = (char*)malloc(strlen(text) + 1);
        strcpy(node->data.text.text, text);
    } else {
        node->data.text.text = NULL;
    }
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

/* ========================================================================
 * Text Input Widget
 * ======================================================================== */

static void text_input_sync_node_text(SGNode* node, TextInputWidgetState* state) {
    if (!node || !state) return;
    node->data.text.text = state->text;
}

static void text_input_notify_change(SGNode* node, TextInputWidgetState* state) {
    if (state && state->on_change) {
        state->on_change(node, state->text ? state->text : "", state->callback_data);
    }
}

static int text_input_ensure_capacity(TextInputWidgetState* state, int needed) {
    char* resized;
    int new_cap;

    if (!state || needed <= 0) return 0;
    if (state->text_capacity >= needed) return 1;

    new_cap = state->text_capacity > 0 ? state->text_capacity * 2 : 64;
    if (new_cap < needed) new_cap = needed;
    if (new_cap < 64) new_cap = 64;

    resized = (char*)realloc(state->text, (size_t)new_cap);
    if (!resized) return 0;

    state->text = resized;
    state->text_capacity = new_cap;
    return 1;
}

static int text_input_insert(TextInputWidgetState* state, const char* str, int len) {
    if (!state || !str || len <= 0) return 0;
    if (state->cursor_pos < 0) state->cursor_pos = 0;
    if (state->cursor_pos > state->text_len) state->cursor_pos = state->text_len;
    if (!text_input_ensure_capacity(state, state->text_len + len + 1)) return 0;

    /* Shift text after cursor */
    memmove(state->text + state->cursor_pos + len,
            state->text + state->cursor_pos,
            state->text_len - state->cursor_pos + 1);
    memcpy(state->text + state->cursor_pos, str, len);
    state->text_len += len;
    state->text[state->text_len] = '\0';
    state->cursor_pos += len;
    return 1;
}

static int text_input_delete_range(TextInputWidgetState* state, int start, int end) {
    if (!state) return 0;
    if (start >= end || start < 0 || end > state->text_len) return 0;
    memmove(state->text + start, state->text + end,
            state->text_len - end + 1);
    state->text_len -= (end - start);
    state->text[state->text_len] = '\0';
    state->cursor_pos = start;
    return 1;
}

static void text_input_on_focus_in(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state) return;
    state->focused = 1;
    state->cursor_blink_time = 0;
    widget_apply_focus_ring(node, &state->focus_visible,
                            &state->saved_border_color, &state->saved_border_width,
                            &state->saved_elevation, &state->saved_shadow_offset_x,
                            &state->saved_shadow_offset_y, &state->saved_shadow_blur,
                            &state->saved_shadow_color);
}

static void text_input_on_focus_out(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state) return;
    state->focused = 0;
    state->selection_start = -1;
    state->selection_end = -1;
    widget_restore_focus_ring(node, &state->focus_visible,
                              &state->saved_border_color, &state->saved_border_width,
                              &state->saved_elevation, &state->saved_shadow_offset_x,
                              &state->saved_shadow_offset_y, &state->saved_shadow_blur,
                              &state->saved_shadow_color);
}

static void text_input_on_key_down(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)udata;
    SGEvent* evt = (SGEvent*)event_data;
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state || !state->focused) return;

    int key = evt->data.key.keycode;
    int mods = evt->mods;
    int handled = 0;
    int changed = 0;

    switch (key) {
        case SG_KEY_LEFT:
            if (state->cursor_pos > 0) state->cursor_pos--;
            handled = 1;
            break;
        case SG_KEY_RIGHT:
            if (state->cursor_pos < state->text_len) state->cursor_pos++;
            handled = 1;
            break;
        case SG_KEY_HOME:
            state->cursor_pos = 0;
            handled = 1;
            break;
        case SG_KEY_END:
            state->cursor_pos = state->text_len;
            handled = 1;
            break;
        case SG_KEY_BACKSPACE:
            if (state->selection_start >= 0 && state->selection_start != state->selection_end) {
                int s = state->selection_start < state->selection_end ?
                        state->selection_start : state->selection_end;
                int e = state->selection_start > state->selection_end ?
                        state->selection_start : state->selection_end;
                changed = text_input_delete_range(state, s, e);
                state->selection_start = -1;
                state->selection_end = -1;
            } else if (state->cursor_pos > 0) {
                changed = text_input_delete_range(state, state->cursor_pos - 1, state->cursor_pos);
            }
            handled = 1;
            break;
        case SG_KEY_DELETE:
            if (state->selection_start >= 0 && state->selection_start != state->selection_end) {
                int s = state->selection_start < state->selection_end ?
                        state->selection_start : state->selection_end;
                int e = state->selection_start > state->selection_end ?
                        state->selection_start : state->selection_end;
                changed = text_input_delete_range(state, s, e);
                state->selection_start = -1;
                state->selection_end = -1;
            } else if (state->cursor_pos < state->text_len) {
                changed = text_input_delete_range(state, state->cursor_pos, state->cursor_pos + 1);
            }
            handled = 1;
            break;
        case SG_KEY_A:
            if (mods & SG_MOD_CTRL) {
                state->selection_start = 0;
                state->selection_end = state->text_len;
                state->cursor_pos = state->text_len;
                handled = 1;
            }
            break;
        default:
            break;
    }

    if (!handled) return;

    if (changed) {
        text_input_sync_node_text(node, state);
        text_input_notify_change(node, state);
    }

    state->cursor_blink_time = 0; /* Reset blink on key press */
    sg_node_mark_dirty(node, changed ? (SG_DIRTY_LAYOUT | SG_DIRTY_PAINT) : SG_DIRTY_PAINT);
    evt->consumed = 1;
}

static void text_input_on_text(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)udata;
    SGEvent* evt = (SGEvent*)event_data;
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state || !state->focused) return;

    /* Delete selection if any */
    if (state->selection_start >= 0 && state->selection_start != state->selection_end) {
        int s = state->selection_start < state->selection_end ?
                state->selection_start : state->selection_end;
        int e = state->selection_start > state->selection_end ?
                state->selection_start : state->selection_end;
        text_input_delete_range(state, s, e);
        state->selection_start = -1;
    }

    /* Insert typed text */
    int len = (int)strlen(evt->data.text_input.text);
    if (len > 0) {
        if (text_input_insert(state, evt->data.text_input.text, len)) {
            text_input_sync_node_text(node, state);
            text_input_notify_change(node, state);
        }
    }

    state->cursor_blink_time = 0;
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
    evt->consumed = 1;
}

static void text_input_on_click(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)udata;
    SGEvent* evt = (SGEvent*)event_data;
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state) return;

    /* Approximate cursor position from click x */
    float local_x = evt->data.mouse.local_x - node->style.padding[3];
    if (local_x < 0) local_x = 0;

    /* Measure character widths to find cursor position */
    if (node->type == SG_NODE_TEXT && node->data.text.font && state->text_len > 0) {
        float* widths = (float*)malloc(state->text_len * sizeof(float));
        skia_font_get_char_widths(node->data.text.font, state->text, widths);

        float accum = -state->scroll_offset;
        state->cursor_pos = state->text_len;
        for (int i = 0; i < state->text_len; i++) {
            if (accum + widths[i] / 2 > local_x) {
                state->cursor_pos = i;
                break;
            }
            accum += widths[i];
        }
        free(widths);
    } else {
        state->cursor_pos = 0;
    }

    state->selection_start = -1;
    state->cursor_blink_time = 0;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

SGNode* widget_text_input(const char* placeholder, SkiaFont font) {
    SGNode* node = sg_node_create(SG_NODE_TEXT);
    node->flags |= SG_INTERACTIVE;

    /* Text input styling */
    node->style.background = 0xFFFFFFFF;
    node->style.border_color = 0xFFCCCCCC;
    node->style.border_width = 1.0f;
    node->style.border_radius = 4.0f;
    node->style.padding[0] = 8.0f;
    node->style.padding[1] = 12.0f;
    node->style.padding[2] = 8.0f;
    node->style.padding[3] = 12.0f;
    node->min_height = 36.0f;
    node->min_width = 120.0f;

    node->data.text.font = font;
    node->ownership_flags &= ~SG_NODE_OWNS_FONT;
    node->data.text.color = 0xFF333333;

    /* Widget state */
    TextInputWidgetState* state = (TextInputWidgetState*)calloc(1, sizeof(TextInputWidgetState));
    state->type = WIDGET_TEXT_INPUT;
    state->text_capacity = 64;
    state->text = (char*)calloc(state->text_capacity, 1);
    state->selection_start = -1;
    state->selection_end = -1;
    if (placeholder) {
        state->placeholder = (char*)malloc(strlen(placeholder) + 1);
        strcpy(state->placeholder, placeholder);
    }
    node->user_data = state;

    /* Set node text to empty (will show placeholder) */
    node->data.text.text = state->text;

    /* Event handlers */
    sg_node_on(node, SG_EVENT_FOCUS_IN, text_input_on_focus_in, NULL);
    sg_node_on(node, SG_EVENT_FOCUS_OUT, text_input_on_focus_out, NULL);
    sg_node_on(node, SG_EVENT_KEY_DOWN, text_input_on_key_down, NULL);
    sg_node_on(node, SG_EVENT_TEXT_INPUT, text_input_on_text, NULL);
    sg_node_on(node, SG_EVENT_CLICK, text_input_on_click, NULL);

    return node;
}

const char* widget_text_input_get_value(SGNode* node) {
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_TEXT_INPUT) return "";
    return state->text;
}

void widget_text_input_set_value(SGNode* node, const char* text) {
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_TEXT_INPUT) return;

    int len = text ? (int)strlen(text) : 0;
    if (!text_input_ensure_capacity(state, len + 1)) return;
    if (text) {
        memcpy(state->text, text, len);
    }
    state->text[len] = '\0';
    state->text_len = len;
    state->cursor_pos = len;
    state->selection_start = -1;
    state->selection_end = -1;

    /* Update the node text pointer */
    text_input_sync_node_text(node, state);
    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

void widget_text_input_on_change(SGNode* node, TextChangeCallback callback, void* user_data) {
    TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_TEXT_INPUT) return;
    state->on_change = callback;
    state->callback_data = user_data;
}

/* ========================================================================
 * Checkbox Widget
 * ======================================================================== */

static void checkbox_on_click(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    CheckboxWidgetState* state = (CheckboxWidgetState*)node->user_data;
    if (!state) return;
    state->checked = !state->checked;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
    if (state->on_change) {
        state->on_change(node, state->checked, state->callback_data);
    }
}

SGNode* widget_checkbox(const char* label, SkiaFont font, int initial_checked) {
    SGNode* row = sg_node_create(SG_NODE_CONTAINER);
    row->layout_type = SG_LAYOUT_ROW;
    row->align_items = SG_ALIGN_CENTER;
    row->spacing = 8.0f;
    row->flags |= SG_INTERACTIVE;

    /* Check box indicator */
    SGNode* box = sg_node_create(SG_NODE_RECT);
    box->min_width = 18.0f;
    box->min_height = 18.0f;
    box->style.border_color = 0xFF666666;
    box->style.border_width = 2.0f;
    box->style.border_radius = 3.0f;
    box->style.background = initial_checked ? 0xFF4285F4 : 0xFFFFFFFF;
    sg_node_add_child(row, box);

    /* Label */
    if (label) {
        SGNode* text = widget_text(label, font, 0xFF333333);
        sg_node_add_child(row, text);
    }

    /* State */
    CheckboxWidgetState* state = (CheckboxWidgetState*)calloc(1, sizeof(CheckboxWidgetState));
    state->type = WIDGET_CHECKBOX;
    state->checked = initial_checked;
    row->user_data = state;

    sg_node_on(row, SG_EVENT_CLICK, checkbox_on_click, NULL);

    return row;
}

int widget_checkbox_is_checked(SGNode* node) {
    CheckboxWidgetState* state = (CheckboxWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_CHECKBOX) return 0;
    return state->checked;
}

void widget_checkbox_set_checked(SGNode* node, int checked) {
    CheckboxWidgetState* state = (CheckboxWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_CHECKBOX) return;
    state->checked = checked;

    /* Update visual */
    SGNode* box = node->first_child;
    if (box) {
        box->style.background = checked ? 0xFF4285F4 : 0xFFFFFFFF;
    }
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void widget_checkbox_on_change(SGNode* node, CheckboxChangeCallback callback, void* user_data) {
    CheckboxWidgetState* state = (CheckboxWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_CHECKBOX) return;
    state->on_change = callback;
    state->callback_data = user_data;
}

/* ========================================================================
 * Slider Widget
 * ======================================================================== */

static void slider_update_visual(SGNode* node, SliderWidgetState* state) {
    /* Slider has track (child 0) and thumb (child 1) */
    SGNode* track = node->first_child;
    SGNode* thumb = track ? track->next_sibling : NULL;
    if (!track || !thumb) return;

    float range = state->max_val - state->min_val;
    float t = (range > 0) ? (state->value - state->min_val) / range : 0;

    /* Position thumb along track */
    float track_w = node->bounds.w - 16.0f; /* thumb width */
    thumb->bounds.x = node->bounds.x + t * track_w;
}

static void slider_on_mouse_down(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)udata;
    SGEvent* evt = (SGEvent*)event_data;
    SliderWidgetState* state = (SliderWidgetState*)node->user_data;
    if (!state) return;

    state->dragging = 1;

    /* Set value based on click position */
    float local_x = evt->data.mouse.local_x;
    float t = local_x / node->bounds.w;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    state->value = state->min_val + t * (state->max_val - state->min_val);

    slider_update_visual(node, state);
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);

    if (state->on_change) {
        state->on_change(node, state->value, state->callback_data);
    }
    evt->consumed = 1;
}

static void slider_on_mouse_move(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)udata;
    SGEvent* evt = (SGEvent*)event_data;
    SliderWidgetState* state = (SliderWidgetState*)node->user_data;
    if (!state || !state->dragging) return;

    float local_x = evt->data.mouse.local_x;
    float t = local_x / node->bounds.w;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    state->value = state->min_val + t * (state->max_val - state->min_val);

    slider_update_visual(node, state);
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);

    if (state->on_change) {
        state->on_change(node, state->value, state->callback_data);
    }
    evt->consumed = 1;
}

static void slider_on_mouse_up(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    SliderWidgetState* state = (SliderWidgetState*)node->user_data;
    if (state) state->dragging = 0;
}

SGNode* widget_slider(double min_val, double max_val, double initial) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->layout_type = SG_LAYOUT_STACK;
    node->flags |= SG_INTERACTIVE;
    node->min_height = 24.0f;
    node->min_width = 100.0f;

    /* Track */
    SGNode* track = sg_node_create(SG_NODE_RECT);
    track->min_height = 4.0f;
    track->style.background = 0xFFDDDDDD;
    track->style.border_radius = 2.0f;
    track->align_self = SG_ALIGN_CENTER;
    sg_node_add_child(node, track);

    /* Thumb */
    SGNode* thumb = sg_node_create(SG_NODE_RECT);
    thumb->min_width = 16.0f;
    thumb->min_height = 16.0f;
    thumb->style.background = 0xFF4285F4;
    thumb->style.border_radius = 8.0f;
    thumb->style.elevation = 2;
    sg_node_add_child(node, thumb);

    /* State */
    SliderWidgetState* state = (SliderWidgetState*)calloc(1, sizeof(SliderWidgetState));
    state->type = WIDGET_SLIDER;
    state->min_val = min_val;
    state->max_val = max_val;
    state->value = initial;
    node->user_data = state;

    sg_node_on(node, SG_EVENT_MOUSE_DOWN, slider_on_mouse_down, NULL);
    sg_node_on(node, SG_EVENT_MOUSE_MOVE, slider_on_mouse_move, NULL);
    sg_node_on(node, SG_EVENT_MOUSE_UP, slider_on_mouse_up, NULL);

    return node;
}

double widget_slider_get_value(SGNode* node) {
    SliderWidgetState* state = (SliderWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_SLIDER) return 0;
    return state->value;
}

void widget_slider_set_value(SGNode* node, double value) {
    SliderWidgetState* state = (SliderWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_SLIDER) return;
    state->value = value;
    slider_update_visual(node, state);
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
}

void widget_slider_on_change(SGNode* node, SliderChangeCallback callback, void* user_data) {
    SliderWidgetState* state = (SliderWidgetState*)node->user_data;
    if (!state || state->type != WIDGET_SLIDER) return;
    state->on_change = callback;
    state->callback_data = user_data;
}

/* ========================================================================
 * Layout Containers
 * ======================================================================== */

SGNode* widget_column(double spacing) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->layout_type = SG_LAYOUT_COLUMN;
    node->spacing = spacing;
    node->align_items = SG_ALIGN_STRETCH;
    return node;
}

SGNode* widget_row(double spacing) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->layout_type = SG_LAYOUT_ROW;
    node->spacing = spacing;
    node->align_items = SG_ALIGN_CENTER;
    return node;
}

SGNode* widget_stack(void) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->layout_type = SG_LAYOUT_STACK;
    return node;
}

SGNode* widget_container(void) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->layout_type = SG_LAYOUT_NONE;
    return node;
}

/* ========================================================================
 * Scroll View
 * ======================================================================== */

static void scroll_on_scroll(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)udata;
    SGEvent* evt = (SGEvent*)event_data;
    ScrollViewState* state = (ScrollViewState*)node->user_data;
    if (!state) return;

    float scroll_speed = 40.0f;
    state->scroll_y -= evt->data.scroll.dy * scroll_speed;

    /* Clamp scroll */
    float max_scroll = state->content_h - state->viewport_h;
    if (max_scroll < 0) max_scroll = 0;
    if (state->scroll_y < 0) state->scroll_y = 0;
    if (state->scroll_y > max_scroll) state->scroll_y = max_scroll;

    sg_node_mark_dirty(node, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
    evt->consumed = 1;
}

SGNode* widget_scroll_view(void) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->layout_type = SG_LAYOUT_STACK;
    node->flags |= SG_CLIP_CHILDREN | SG_INTERACTIVE;

    ScrollViewState* state = (ScrollViewState*)calloc(1, sizeof(ScrollViewState));
    state->type = WIDGET_SCROLL_VIEW;
    state->show_v_scroll = 1;
    node->user_data = state;

    sg_node_on(node, SG_EVENT_MOUSE_SCROLL, scroll_on_scroll, NULL);

    return node;
}

void widget_scroll_view_set_content(SGNode* sv, SGNode* content) {
    if (!sv || !content) return;

    /* Remove existing children */
    while (sv->first_child) {
        sg_node_remove_child(sv, sv->first_child);
    }

    sg_node_add_child(sv, content);
    sg_node_mark_dirty(sv, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

void widget_scroll_to(SGNode* sv, double x, double y) {
    ScrollViewState* state = (ScrollViewState*)sv->user_data;
    if (!state || state->type != WIDGET_SCROLL_VIEW) return;
    state->scroll_x = sg_clampf((float)x, 0.0f, fmaxf(0.0f, state->content_w - state->viewport_w));
    state->scroll_y = sg_clampf((float)y, 0.0f, fmaxf(0.0f, state->content_h - state->viewport_h));
    sg_node_mark_dirty(sv, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

/* ========================================================================
 * Image View
 * ======================================================================== */

SGNode* widget_image(const char* path, int fit) {
    SGNode* node = sg_node_create(SG_NODE_IMAGE);
    if (path) {
        node->data.image.image = skia_image_load_from_file(path);
        if (node->data.image.image) {
            node->ownership_flags |= SG_NODE_OWNS_IMAGE;
        }
    }
    node->data.image.fit = fit;
    return node;
}

SGNode* widget_image_from_handle(SkiaImage img, int fit) {
    SGNode* node = sg_node_create(SG_NODE_IMAGE);
    node->data.image.image = img;
    node->ownership_flags &= ~SG_NODE_OWNS_IMAGE;
    node->data.image.fit = fit;
    return node;
}

/* ========================================================================
 * Separator
 * ======================================================================== */

SGNode* widget_separator(int horizontal, uint32_t color, double thickness) {
    SGNode* node = sg_node_create(SG_NODE_RECT);
    node->style.background = color;
    if (horizontal) {
        node->min_height = thickness;
        node->flex_grow = 1.0f; /* stretch horizontally */
    } else {
        node->min_width = thickness;
        node->flex_grow = 1.0f; /* stretch vertically */
    }
    return node;
}

/* ========================================================================
 * Spacer
 * ======================================================================== */

SGNode* widget_spacer(double grow) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->flex_grow = grow;
    return node;
}

/* ========================================================================
 * Widget Cleanup
 * ======================================================================== */

void widget_cleanup(SGNode* node) {
    if (!node || !node->user_data) return;

    WidgetType* type_ptr = (WidgetType*)node->user_data;
    switch (*type_ptr) {
        case WIDGET_TEXT_INPUT: {
            TextInputWidgetState* state = (TextInputWidgetState*)node->user_data;
            /* Don't free state->text — it's the same pointer as node->data.text.text,
             * which gets freed in sg_node_destroy */
            free(state->placeholder);
            free(state);
            break;
        }
        case WIDGET_TABS: {
            TabsState* state = (TabsState*)node->user_data;
            for (int i = 0; i < state->count; i++) {
                free(state->labels[i]);
            }
            free(state->labels);
            free(state);
            break;
        }
        case WIDGET_MENU: {
            MenuState* state = (MenuState*)node->user_data;
            for (int i = 0; i < state->count; i++) {
                free(state->items[i]);
            }
            free(state->items);
            free(state);
            break;
        }
        case WIDGET_TAB_PANEL: {
            TabPanelState* state = (TabPanelState*)node->user_data;
            free(state->contents);
            free(state);
            break;
        }
        case WIDGET_CUSTOM_CANVAS: {
            CustomCanvasState* state = (CustomCanvasState*)node->user_data;
            if (state->destroy_data && state->draw_data) {
                state->destroy_data(state->draw_data);
            }
            free(state);
            break;
        }
        case WIDGET_BUTTON:
        case WIDGET_CHECKBOX:
        case WIDGET_SLIDER:
        case WIDGET_SCROLL_VIEW:
        case WIDGET_PROGRESS_BAR:
            free(node->user_data);
            break;
        default:
            break;
    }
}

/* ========================================================================
 * Progress Bar Widget
 * ======================================================================== */

SGNode* widget_progress_bar(double min_val, double max_val, double value) {
    SGNode* node = sg_node_create(SG_NODE_CONTAINER);
    node->layout_type = SG_LAYOUT_STACK;
    node->min_height = 8.0f;
    node->min_width = 200.0f;
    
    /* Background track */
    SGNode* track = sg_node_create(SG_NODE_RECT);
    track->style.background = 0xFFE0E0E0;
    track->style.border_radius = 4.0f;
    track->flex_grow = 1.0f;
    sg_node_add_child(node, track);
    
    /* Progress fill */
    SGNode* fill = sg_node_create(SG_NODE_RECT);
    fill->style.background = 0xFF4CAF50;  /* Green */
    fill->style.border_radius = 4.0f;
    fill->align_self = SG_ALIGN_START;
    sg_node_add_child(node, fill);
    
    /* State */
    ProgressBarState* state = (ProgressBarState*)calloc(1, sizeof(ProgressBarState));
    state->type = WIDGET_PROGRESS_BAR;
    state->value = value;
    state->min_val = min_val;
    state->max_val = max_val;
    state->fill_color = 0xFF4CAF50;
    state->bg_color = 0xFFE0E0E0;
    node->user_data = state;
    
    /* Set initial width */
    float range = max_val - min_val;
    float t = (range > 0) ? (value - min_val) / range : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    fill->min_width = 200.0f * t;  /* Will be updated by layout */
    
    return node;
}

void widget_progress_set_value(SGNode* bar, double value) {
    ProgressBarState* state = (ProgressBarState*)bar->user_data;
    if (!state || state->type != WIDGET_PROGRESS_BAR) return;
    
    state->value = value;
    
    /* Update fill width */
    SGNode* fill = bar->first_child ? bar->first_child->next_sibling : NULL;
    if (fill) {
        float range = state->max_val - state->min_val;
        float t = (range > 0) ? (value - state->min_val) / range : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        fill->min_width = bar->bounds.w * t;
        sg_node_mark_dirty(bar, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
    }
}

void widget_progress_set_indeterminate(SGNode* bar, int indeterminate) {
    ProgressBarState* state = (ProgressBarState*)bar->user_data;
    if (!state || state->type != WIDGET_PROGRESS_BAR) return;
    state->indeterminate = indeterminate;
    sg_node_mark_dirty(bar, SG_DIRTY_PAINT);
}

void widget_progress_set_colors(SGNode* bar, uint32_t fill, uint32_t bg) {
    ProgressBarState* state = (ProgressBarState*)bar->user_data;
    if (!state || state->type != WIDGET_PROGRESS_BAR) return;
    
    state->fill_color = fill;
    state->bg_color = bg;
    
    /* Update children */
    SGNode* track = bar->first_child;
    SGNode* fill_node = track ? track->next_sibling : NULL;
    if (track) track->style.background = bg;
    if (fill_node) fill_node->style.background = fill;
    sg_node_mark_dirty(bar, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Custom Canvas Widget
 * ======================================================================== */

static void widget_canvas_draw_adapter(SkiaCanvas canvas, SGRect bounds, void* ctx) {
    CustomCanvasState* state = (CustomCanvasState*)ctx;
    if (!state || !state->draw_fn) return;

    state->width = bounds.w;
    state->height = bounds.h;
    state->draw_fn(canvas, bounds.w, bounds.h, state->draw_data);
}

SGNode* widget_canvas(float width, float height, CanvasDrawCallback draw_fn, void* user_data) {
    SGNode* node = sg_node_create(SG_NODE_CANVAS);
    node->min_width = width;
    node->min_height = height;
    
    CustomCanvasState* state = (CustomCanvasState*)calloc(1, sizeof(CustomCanvasState));
    state->type = WIDGET_CUSTOM_CANVAS;
    state->draw_fn = draw_fn;
    state->destroy_data = NULL;
    state->draw_data = user_data;
    state->width = width;
    state->height = height;
    node->user_data = state;
    
    /* Set the canvas draw callback */
    node->data.canvas.draw_fn = widget_canvas_draw_adapter;
    node->data.canvas.ctx = state;
    
    return node;
}

void widget_canvas_invalidate(SGNode* canvas) {
    if (!canvas) return;
    sg_node_mark_dirty(canvas, SG_DIRTY_PAINT);
}

void widget_canvas_set_callback(SGNode* canvas, CanvasDrawCallback draw_fn, void* user_data) {
    CustomCanvasState* state = (CustomCanvasState*)canvas->user_data;
    if (!state || state->type != WIDGET_CUSTOM_CANVAS) return;
    
    state->draw_fn = draw_fn;
    state->draw_data = user_data;
    canvas->data.canvas.draw_fn = widget_canvas_draw_adapter;
    canvas->data.canvas.ctx = state;
    sg_node_mark_dirty(canvas, SG_DIRTY_PAINT);
}

void widget_canvas_set_data_destructor(SGNode* canvas, CanvasDestroyCallback destroy_data) {
    CustomCanvasState* state;

    if (!canvas) return;
    state = (CustomCanvasState*)canvas->user_data;
    if (!state || state->type != WIDGET_CUSTOM_CANVAS) return;

    state->destroy_data = destroy_data;
}

/* ========================================================================
 * Tabs Widget
 * ======================================================================== */

static void tabs_on_click(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data;
    int tab_index = (int)(intptr_t)udata;
    
    /* Find parent tabs container */
    SGNode* tabs = node->parent;
    if (!tabs) return;
    
    TabsState* state = (TabsState*)tabs->user_data;
    if (!state || state->type != WIDGET_TABS) return;
    
    if (tab_index == state->active_tab) return;
    
    /* Update active tab */
    int old_active = state->active_tab;
    state->active_tab = tab_index;
    
    /* Update visual: highlight active tab */
    SGNode* tab = tabs->first_child;
    int i = 0;
    while (tab) {
        if (i == state->active_tab) {
            tab->style.background = 0xFF4285F4;
            tab->style.border_width = 0;
        } else {
            tab->style.background = 0xFFF0F0F0;
            tab->style.border_width = 0;
        }
        tab = tab->next_sibling;
        i++;
    }
    
    sg_node_mark_dirty(tabs, SG_DIRTY_PAINT);
    
    if (state->on_change) {
        state->on_change(tabs, tab_index, state->callback_data);
    }
}

SGNode* widget_tabs(const char** labels, int count, TabChangeCallback on_change, void* user_data) {
    SGNode* container = sg_node_create(SG_NODE_CONTAINER);
    container->layout_type = SG_LAYOUT_ROW;
    container->spacing = 2.0f;
    container->style.padding[2] = 2.0f;  /* Bottom padding */
    
    /* Create tab buttons */
    for (int i = 0; i < count; i++) {
        SGNode* tab = sg_node_create(SG_NODE_CONTAINER);
        tab->layout_type = SG_LAYOUT_ROW;
        tab->justify = SG_JUSTIFY_CENTER;
        tab->align_items = SG_ALIGN_CENTER;
        tab->flags |= SG_INTERACTIVE;
        tab->style.padding[0] = 8.0f;
        tab->style.padding[1] = 16.0f;
        tab->style.padding[2] = 8.0f;
        tab->style.padding[3] = 16.0f;
        tab->style.background = (i == 0) ? 0xFF4285F4 : 0xFFF0F0F0;
        tab->min_height = 36.0f;
        
        /* Tab label */
        SkiaFont font = skia_font_create("Arial", 14.0f);
        SGNode* label = widget_text(labels[i], font, (i == 0) ? 0xFFFFFFFF : 0xFF333333);
        label->ownership_flags |= SG_NODE_OWNS_FONT;
        sg_node_add_child(tab, label);
        
        /* Click handler */
        sg_node_on(tab, SG_EVENT_CLICK, tabs_on_click, (void*)(intptr_t)i);
        
        sg_node_add_child(container, tab);
    }
    
    /* State */
    TabsState* state = (TabsState*)calloc(1, sizeof(TabsState));
    state->type = WIDGET_TABS;
    state->count = count;
    state->labels = (char**)calloc(count, sizeof(char*));
    for (int i = 0; i < count; i++) {
        state->labels[i] = strdup(labels[i]);
    }
    state->active_tab = 0;
    state->on_change = on_change;
    state->callback_data = user_data;
    container->user_data = state;
    
    return container;
}

void widget_tabs_set_active(SGNode* tabs, int index) {
    TabsState* state = (TabsState*)tabs->user_data;
    if (!state || state->type != WIDGET_TABS) return;
    if (index < 0 || index >= state->count) return;
    
    state->active_tab = index;
    
    /* Update visuals */
    SGNode* tab = tabs->first_child;
    int i = 0;
    while (tab) {
        tab->style.background = (i == index) ? 0xFF4285F4 : 0xFFF0F0F0;
        /* Update text color */
        SGNode* label = tab->first_child;
        if (label && label->type == SG_NODE_TEXT) {
            label->data.text.color = (i == index) ? 0xFFFFFFFF : 0xFF333333;
        }
        tab = tab->next_sibling;
        i++;
    }
    
    sg_node_mark_dirty(tabs, SG_DIRTY_PAINT);
}

int widget_tabs_get_active(SGNode* tabs) {
    TabsState* state = (TabsState*)tabs->user_data;
    if (!state || state->type != WIDGET_TABS) return -1;
    return state->active_tab;
}

/* ========================================================================
 * Menu/Dropdown Widget
 * ======================================================================== */

static void menu_item_on_click(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data;
    int item_index = (int)(intptr_t)udata;
    
    /* Find parent menu */
    SGNode* menu = node->parent;
    if (!menu) return;
    
    MenuState* state = (MenuState*)menu->user_data;
    if (!state || state->type != WIDGET_MENU) return;
    
    /* Hide menu */
    menu->flags &= ~SG_VISIBLE;
    state->visible = 0;
    
    /* Call callback */
    if (state->on_select) {
        state->on_select(menu, item_index, state->callback_data);
    }
    
    sg_node_mark_dirty(menu, SG_DIRTY_PAINT);
}

SGNode* widget_menu(const char** items, int count, MenuItemCallback on_select, void* user_data) {
    SGNode* menu = sg_node_create(SG_NODE_CONTAINER);
    menu->layout_type = SG_LAYOUT_COLUMN;
    menu->style.background = 0xFFFFFFFF;
    menu->style.border_color = 0xFFCCCCCC;
    menu->style.border_width = 1.0f;
    menu->style.border_radius = 4.0f;
    menu->style.elevation = 3;  /* Drop shadow */
    menu->min_width = 150.0f;
    menu->flags &= ~SG_VISIBLE;  /* Start hidden */
    
    /* Create menu items */
    for (int i = 0; i < count; i++) {
        SGNode* item = sg_node_create(SG_NODE_CONTAINER);
        item->layout_type = SG_LAYOUT_ROW;
        item->align_items = SG_ALIGN_CENTER;
        item->flags |= SG_INTERACTIVE;
        item->style.padding[0] = 8.0f;
        item->style.padding[1] = 16.0f;
        item->style.padding[2] = 8.0f;
        item->style.padding[3] = 16.0f;
        item->min_height = 32.0f;
        
        SkiaFont font = skia_font_create("Arial", 14.0f);
        SGNode* label = widget_text(items[i], font, 0xFF333333);
        label->ownership_flags |= SG_NODE_OWNS_FONT;
        sg_node_add_child(item, label);
        
        sg_node_on(item, SG_EVENT_CLICK, menu_item_on_click, (void*)(intptr_t)i);
        sg_node_add_child(menu, item);
    }
    
    /* State */
    MenuState* state = (MenuState*)calloc(1, sizeof(MenuState));
    state->type = WIDGET_MENU;
    state->count = count;
    state->items = (char**)calloc(count, sizeof(char*));
    for (int i = 0; i < count; i++) {
        state->items[i] = strdup(items[i]);
    }
    state->visible = 0;
    state->hovered_item = -1;
    state->on_select = on_select;
    state->callback_data = user_data;
    menu->user_data = state;
    
    return menu;
}

void widget_menu_show(SGNode* menu, float x, float y) {
    MenuState* state = (MenuState*)menu->user_data;
    if (!state || state->type != WIDGET_MENU) return;
    
    state->visible = 1;
    menu->flags |= SG_VISIBLE;
    menu->transform.tx = x;
    menu->transform.ty = y;
    sg_node_mark_dirty(menu, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);
}

void widget_menu_hide(SGNode* menu) {
    MenuState* state = (MenuState*)menu->user_data;
    if (!state || state->type != WIDGET_MENU) return;

    state->visible = 0;
    menu->flags &= ~SG_VISIBLE;
    sg_node_mark_dirty(menu, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Tab Panel — compound widget with auto content switching
 * ======================================================================== */

static void tabpanel_on_change(SGNode* tabs, int index, void* data) {
    TabPanelState* state = (TabPanelState*)data;
    for (int i = 0; i < state->count; i++)
        sg_node_set_visible(state->contents[i], i == index);
    state->active = index;
    (void)tabs;
}

SGNode* widget_tabpanel(const char* l1, SGNode* c1,
                         const char* l2, SGNode* c2,
                         const char* l3, SGNode* c3,
                         const char* l4, SGNode* c4) {
    const char* labels[4];
    SGNode*     contents[4];
    int         count = 0;

    if (l1 && l1[0] && c1) { labels[count] = l1; contents[count++] = c1; }
    if (l2 && l2[0] && c2) { labels[count] = l2; contents[count++] = c2; }
    if (l3 && l3[0] && c3) { labels[count] = l3; contents[count++] = c3; }
    if (l4 && l4[0] && c4) { labels[count] = l4; contents[count++] = c4; }
    if (count == 0) return NULL;

    TabPanelState* state = (TabPanelState*)malloc(sizeof(TabPanelState));
    state->type     = WIDGET_TAB_PANEL;
    state->count    = count;
    state->active   = 0;
    state->contents = (SGNode**)malloc(count * sizeof(SGNode*));
    for (int i = 0; i < count; i++) state->contents[i] = contents[i];

    SGNode* header = widget_tabs(labels, count, tabpanel_on_change, state);
    state->header  = header;

    /* Show only the first tab's content */
    for (int i = 0; i < count; i++)
        sg_node_set_visible(contents[i], i == 0);

    /* Root column: header row + all content nodes (visibility toggled) */
    SGNode* root = widget_column(0.0f);
    sg_node_add_child(root, header);
    for (int i = 0; i < count; i++)
        sg_node_add_child(root, contents[i]);

    root->user_data = state;
    return root;
}

int widget_tabpanel_get_active(SGNode* root) {
    if (!root || !root->user_data) return 0;
    return ((TabPanelState*)root->user_data)->active;
}

void widget_tabpanel_set_active(SGNode* root, int index) {
    if (!root || !root->user_data) return;
    TabPanelState* state = (TabPanelState*)root->user_data;
    if (index < 0 || index >= state->count) return;
    tabpanel_on_change(state->header, index, state);
    widget_tabs_set_active(state->header, index);
}
