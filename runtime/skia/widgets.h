/*
 * Core Widgets — Pre-built UI components for Casperix UI
 *
 * Widgets are factory functions that create configured SGNodes with
 * event handlers for common interactive elements: buttons, text inputs,
 * scroll views, etc.
 *
 * Each widget stores its internal state in the node's user_data pointer.
 */

#ifndef WIDGETS_H
#define WIDGETS_H

#include "scene_graph.h"
#include "animation.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Widget State Tags (stored in user_data)
 * ======================================================================== */

typedef enum {
    WIDGET_NONE = 0,
    WIDGET_BUTTON,
    WIDGET_TEXT_INPUT,
    WIDGET_CHECKBOX,
    WIDGET_SLIDER,
    WIDGET_SCROLL_VIEW,
    WIDGET_LIST_VIEW,
    WIDGET_PROGRESS_BAR,
    WIDGET_CUSTOM_CANVAS,
    WIDGET_TABS,
    WIDGET_MENU,
    WIDGET_TAB_PANEL,
} WidgetType;

/* ========================================================================
 * Button Widget
 * ======================================================================== */

typedef enum {
    BUTTON_NORMAL,
    BUTTON_HOVERED,
    BUTTON_PRESSED,
    BUTTON_DISABLED,
} ButtonState;

typedef void (*ButtonClickCallback)(SGNode* button, void* user_data);

typedef struct {
    WidgetType type;              /* WIDGET_BUTTON */
    ButtonState state;
    int focus_visible;
    uint32_t normal_color;        /* Default background */
    uint32_t hover_color;         /* Hovered background */
    uint32_t press_color;         /* Pressed background */
    uint32_t disabled_color;      /* Disabled background */
    uint32_t text_color;
    uint32_t saved_border_color;
    float saved_border_width;
    int saved_elevation;
    float saved_shadow_offset_x;
    float saved_shadow_offset_y;
    float saved_shadow_blur;
    uint32_t saved_shadow_color;
    ButtonClickCallback on_click;
    void* callback_data;
} ButtonWidgetState;

/* Create a button with label text.
 * Returns the root SGNode for the button. */
SGNode* widget_button(const char* label, SkiaFont font,
                       ButtonClickCallback on_click, void* user_data);

/* Set button colors */
void widget_button_set_colors(SGNode* btn, uint32_t normal, uint32_t hover,
                               uint32_t press, uint32_t text);

/* Enable/disable button */
void widget_button_set_enabled(SGNode* btn, int enabled);

/* Update button click callback after construction */
void widget_button_set_callback(SGNode* btn, ButtonClickCallback on_click, void* user_data);

/* Get button state */
ButtonState widget_button_get_state(SGNode* btn);

/* ========================================================================
 * Text Widget
 * ======================================================================== */

/* Create a simple text label */
SGNode* widget_text(const char* text, SkiaFont font, uint32_t color);

/* Update text content */
void widget_text_set(SGNode* node, const char* text);

/* ========================================================================
 * Text Input Widget
 * ======================================================================== */

typedef void (*TextChangeCallback)(SGNode* input, const char* text, void* user_data);

typedef struct {
    WidgetType type;              /* WIDGET_TEXT_INPUT */
    char* text;                   /* Current text content */
    int text_len;                 /* Current length */
    int text_capacity;            /* Buffer capacity */
    int cursor_pos;               /* Cursor position (char index) */
    int selection_start;          /* Selection start (-1 = no selection) */
    int selection_end;            /* Selection end */
    char* placeholder;            /* Placeholder text */
    int focused;                  /* Is this input focused? */
    int focus_visible;
    float scroll_offset;          /* Horizontal scroll for overflow text */
    double cursor_blink_time;     /* For cursor blink animation */
    uint32_t saved_border_color;
    float saved_border_width;
    int saved_elevation;
    float saved_shadow_offset_x;
    float saved_shadow_offset_y;
    float saved_shadow_blur;
    uint32_t saved_shadow_color;
    TextChangeCallback on_change;
    void* callback_data;
} TextInputWidgetState;

/* Create a text input field */
SGNode* widget_text_input(const char* placeholder, SkiaFont font);

/* Get/set text input value */
const char* widget_text_input_get_value(SGNode* node);
void        widget_text_input_set_value(SGNode* node, const char* text);

/* Set change callback */
void widget_text_input_on_change(SGNode* node, TextChangeCallback callback, void* user_data);

/* ========================================================================
 * Checkbox Widget
 * ======================================================================== */

typedef void (*CheckboxChangeCallback)(SGNode* checkbox, int checked, void* user_data);

typedef struct {
    WidgetType type;              /* WIDGET_CHECKBOX */
    int checked;
    CheckboxChangeCallback on_change;
    void* callback_data;
} CheckboxWidgetState;

SGNode* widget_checkbox(const char* label, SkiaFont font, int initial_checked);
int     widget_checkbox_is_checked(SGNode* node);
void    widget_checkbox_set_checked(SGNode* node, int checked);
void    widget_checkbox_on_change(SGNode* node, CheckboxChangeCallback callback, void* user_data);

/* ========================================================================
 * Slider Widget
 * ======================================================================== */

typedef void (*SliderChangeCallback)(SGNode* slider, float value, void* user_data);

typedef struct {
    WidgetType type;              /* WIDGET_SLIDER */
    float value;                  /* Current value (0.0 - 1.0) */
    float min_val, max_val;       /* Value range */
    int dragging;                 /* Currently being dragged? */
    SliderChangeCallback on_change;
    void* callback_data;
} SliderWidgetState;

SGNode* widget_slider(double min_val, double max_val, double initial);
double  widget_slider_get_value(SGNode* node);
void    widget_slider_set_value(SGNode* node, double value);
void    widget_slider_on_change(SGNode* node, SliderChangeCallback callback, void* user_data);

/* ========================================================================
 * Layout Containers
 * ======================================================================== */

/* Column: vertical flex layout */
SGNode* widget_column(double spacing);

/* Row: horizontal flex layout */
SGNode* widget_row(double spacing);

/* Stack: overlapping children */
SGNode* widget_stack(void);

/* Plain container (no layout) */
SGNode* widget_container(void);

/* ========================================================================
 * Scroll View
 * ======================================================================== */

typedef struct {
    WidgetType type;              /* WIDGET_SCROLL_VIEW */
    float scroll_x, scroll_y;    /* Current scroll offset */
    float content_w, content_h;  /* Total content size */
    float viewport_w, viewport_h;/* Visible area size */
    int show_h_scroll;           /* Show horizontal scrollbar */
    int show_v_scroll;           /* Show vertical scrollbar */
    int dragging_scrollbar;      /* Currently dragging scrollbar */
} ScrollViewState;

SGNode* widget_scroll_view(void);
void    widget_scroll_view_set_content(SGNode* sv, SGNode* content);
void    widget_scroll_to(SGNode* sv, double x, double y);

/* ========================================================================
 * Image View
 * ======================================================================== */

SGNode* widget_image(const char* path, int fit);
SGNode* widget_image_from_handle(SkiaImage img, int fit);

/* ========================================================================
 * Separator
 * ======================================================================== */

SGNode* widget_separator(int horizontal, uint32_t color, double thickness);

/* ========================================================================
 * Spacer (for flex layouts)
 * ======================================================================== */

SGNode* widget_spacer(double grow);

/* ========================================================================
 * Progress Bar Widget
 * ======================================================================== */

typedef struct {
    WidgetType type;              /* WIDGET_PROGRESS_BAR */
    float value;                  /* Target logical value */
    float display_value;          /* Animated displayed value */
    float anim_from_value;
    float min_val, max_val;       /* Value range */
    int indeterminate;            /* Indeterminate animation mode */
    uint32_t fill_color;          /* Progress fill color */
    uint32_t bg_color;            /* Background color */
    float anim_pos;               /* Animation position for indeterminate */
    float anim_duration_sec;      /* 0 = instant updates */
    SGEasing anim_easing;
    int animating;
    float anim_elapsed_sec;
} ProgressBarState;

SGNode* widget_progress_bar(double min_val, double max_val, double value);
void    widget_progress_set_value(SGNode* bar, double value);
void    widget_progress_configure_animation(SGNode* bar, float duration_sec, SGEasing easing);
void    widget_progress_tick(SGNode* bar, float dt_sec);
void    widget_progress_set_indeterminate(SGNode* bar, int indeterminate);
void    widget_progress_set_colors(SGNode* bar, uint32_t fill, uint32_t bg);

/* ========================================================================
 * Custom Canvas Widget
 * ======================================================================== */

typedef void (*CanvasDrawCallback)(SkiaCanvas canvas, float width, float height, void* user_data);
typedef void (*CanvasDestroyCallback)(void* user_data);

typedef struct {
    WidgetType type;              /* WIDGET_CUSTOM_CANVAS */
    CanvasDrawCallback draw_fn;   /* User-provided draw function */
    CanvasDestroyCallback destroy_data;
    void* draw_data;              /* User data passed to draw_fn */
    float width, height;          /* Canvas dimensions */
} CustomCanvasState;

SGNode* widget_canvas(float width, float height, CanvasDrawCallback draw_fn, void* user_data);
void    widget_canvas_invalidate(SGNode* canvas);
void    widget_canvas_set_callback(SGNode* canvas, CanvasDrawCallback draw_fn, void* user_data);
void    widget_canvas_set_data_destructor(SGNode* canvas, CanvasDestroyCallback destroy_data);

/* ========================================================================
 * Tabs Widget
 * ======================================================================== */

typedef void (*TabChangeCallback)(SGNode* tabs, int index, void* user_data);

typedef struct {
    WidgetType type;              /* WIDGET_TABS */
    char** labels;                /* Tab labels */
    int count;                    /* Number of tabs */
    int active_tab;               /* Currently active tab index */
    TabChangeCallback on_change;
    void* callback_data;
} TabsState;

SGNode* widget_tabs(const char** labels, int count, TabChangeCallback on_change, void* user_data);
void    widget_tabs_set_active(SGNode* tabs, int index);
int     widget_tabs_get_active(SGNode* tabs);

/* ========================================================================
 * Menu/Dropdown Widget
 * ======================================================================== */

typedef void (*MenuItemCallback)(SGNode* menu, int item_index, void* user_data);

typedef struct {
    WidgetType type;              /* WIDGET_MENU */
    char** items;                 /* Menu item labels */
    int count;                    /* Number of items */
    int visible;                  /* Is menu shown? */
    int hovered_item;             /* Currently hovered item (-1 = none) */
    MenuItemCallback on_select;
    void* callback_data;
} MenuState;

SGNode* widget_menu(const char** items, int count, MenuItemCallback on_select, void* user_data);
void    widget_menu_show(SGNode* menu, float x, float y);
void    widget_menu_hide(SGNode* menu);

/* ========================================================================
 * Tab Panel (compound widget — auto-manages tab content switching)
 * ======================================================================== */

/* Creates a complete tab UI from up to 4 label+content pairs.
 * Pass NULL label to mark end (e.g. l3=NULL means only 2 tabs).
 * Tab clicks automatically toggle SG_VISIBLE on the matching content node.
 * Returns a root Column node (tabs header + content area). */
SGNode* widget_tabpanel(const char* l1, SGNode* c1,
                         const char* l2, SGNode* c2,
                         const char* l3, SGNode* c3,
                         const char* l4, SGNode* c4);

/* Get/set the active tab index of a tabpanel root node. */
int  widget_tabpanel_get_active(SGNode* root);
void widget_tabpanel_set_active(SGNode* root, int index);

/* ========================================================================
 * Widget Cleanup
 * ======================================================================== */

/* Free widget state attached to a node. Called automatically by sg_node_destroy. */
void widget_cleanup(SGNode* node);

/* Optional per-widget custom rendering hook.
 * Returns non-zero if the widget fully rendered its own content. */
int widget_render_override(SkiaCanvas canvas, SGNode* node);

/* Tick widget-local animation/state such as caret blinking.
 * Returns non-zero when a repaint is needed. */
int widget_tick_tree(SGNode* node, float dt);

#ifdef __cplusplus
}
#endif

#endif /* WIDGETS_H */
