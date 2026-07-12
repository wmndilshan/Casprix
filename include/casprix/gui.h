// Casprix GUI Library
// Simple cross-platform GUI using native APIs

#ifndef NUWAN_GUI_H
#define NUWAN_GUI_H

#if !defined(CASPRIX_SUPPRESS_LEGACY_GUI_DEPRECATION)
    #if defined(_MSC_VER)
        #pragma message("include/casprix/gui.h is deprecated; migrate to the skia module (lib/skia/ui or runtime/skia)")
    #else
        #warning "include/casprix/gui.h is deprecated; migrate to the skia module (lib/skia/ui or runtime/skia)"
    #endif
#endif

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #include <windows.h>
    typedef HWND widget_handle_t;
#else
    // For Linux, we'd use GTK or X11
    typedef void* widget_handle_t;
#endif

// Color structure
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} nuwan_color_t;

// Widget types
typedef enum {
    NUWAN_WIDGET_WINDOW,
    NUWAN_WIDGET_BUTTON,
    NUWAN_WIDGET_LABEL,
    NUWAN_WIDGET_TEXTBOX,
    NUWAN_WIDGET_PANEL
} nuwan_widget_type_t;

// Base widget structure
typedef struct nuwan_widget {
    widget_handle_t handle;
    nuwan_widget_type_t type;
    int x, y, width, height;
    bool visible;
    bool enabled;
    char* text;
    void (*on_click)(struct nuwan_widget* widget);
    void* user_data;
    struct nuwan_widget* parent;
} nuwan_widget_t;

// Window structure (maps to Casprix Window class)
typedef struct {
    nuwan_widget_t base;
    char* title;
    bool resizable;
    bool has_menu;
    int child_count;
    nuwan_widget_t** children;
} nuwan_window_t;

// Button structure (maps to Casprix Button class)
typedef struct {
    nuwan_widget_t base;
} nuwan_button_t;

// Label structure (maps to Casprix Label class)
typedef struct {
    nuwan_widget_t base;
    nuwan_color_t text_color;
    nuwan_color_t bg_color;
} nuwan_label_t;

// TextBox structure (maps to Casprix TextBox class)
typedef struct {
    nuwan_widget_t base;
    bool multiline;
    bool readonly;
    int max_length;
} nuwan_textbox_t;

// CheckBox structure
typedef struct {
    nuwan_widget_t base;
    bool checked;
} nuwan_checkbox_t;

// ComboBox structure
typedef struct {
    nuwan_widget_t base;
    int item_count;
    int selected_index;
} nuwan_combobox_t;

// ProgressBar structure
typedef struct {
    nuwan_widget_t base;
    int min_value;
    int max_value;
    int current_value;
} nuwan_progressbar_t;

// ListBox structure
typedef struct {
    nuwan_widget_t base;
    int item_count;
    int selected_index;
} nuwan_listbox_t;

// ============================================================================
// GUI Core API
// ============================================================================

void nuwan_gui_init();
void nuwan_gui_cleanup();
void nuwan_gui_run();
void nuwan_gui_quit();

// ============================================================================
// Window API
// ============================================================================

nuwan_window_t* nuwan_window_create(const char* title, int width, int height);
void nuwan_window_show(nuwan_window_t* window);
void nuwan_window_hide(nuwan_window_t* window);
void nuwan_window_add_widget(nuwan_window_t* window, nuwan_widget_t* widget);
void nuwan_window_destroy(nuwan_window_t* window);

// ============================================================================
// Button API
// ============================================================================

nuwan_button_t* nuwan_button_create(const char* text, int x, int y, int width, int height);
void nuwan_button_set_callback(nuwan_button_t* button, void (*callback)(nuwan_widget_t*));
void nuwan_button_destroy(nuwan_button_t* button);

// ============================================================================
// Label API
// ============================================================================

nuwan_label_t* nuwan_label_create(const char* text, int x, int y, int width, int height);
void nuwan_label_set_text(nuwan_label_t* label, const char* text);
void nuwan_label_set_color(nuwan_label_t* label, nuwan_color_t color);
void nuwan_label_destroy(nuwan_label_t* label);

// ============================================================================
// TextBox API
// ============================================================================

nuwan_textbox_t* nuwan_textbox_create(int x, int y, int width, int height, bool multiline);
char* nuwan_textbox_get_text(nuwan_textbox_t* textbox);
void nuwan_textbox_set_text(nuwan_textbox_t* textbox, const char* text);
void nuwan_textbox_destroy(nuwan_textbox_t* textbox);

// ============================================================================
// CheckBox API
// ============================================================================

nuwan_checkbox_t* nuwan_checkbox_create(const char* text, int x, int y, int width, int height);
bool nuwan_checkbox_is_checked(nuwan_checkbox_t* checkbox);
void nuwan_checkbox_set_checked(nuwan_checkbox_t* checkbox, bool checked);
void nuwan_checkbox_destroy(nuwan_checkbox_t* checkbox);

// ============================================================================
// ComboBox API
// ============================================================================

nuwan_combobox_t* nuwan_combobox_create(int x, int y, int width, int height);
void nuwan_combobox_add_item(nuwan_combobox_t* combo, const char* item);
int nuwan_combobox_get_selected(nuwan_combobox_t* combo);
void nuwan_combobox_set_selected(nuwan_combobox_t* combo, int index);
void nuwan_combobox_destroy(nuwan_combobox_t* combo);

// ============================================================================
// ProgressBar API
// ============================================================================

nuwan_progressbar_t* nuwan_progressbar_create(int x, int y, int width, int height);
void nuwan_progressbar_set_value(nuwan_progressbar_t* progress, int value);
int nuwan_progressbar_get_value(nuwan_progressbar_t* progress);
void nuwan_progressbar_destroy(nuwan_progressbar_t* progress);

// ============================================================================
// ListBox API
// ============================================================================

nuwan_listbox_t* nuwan_listbox_create(int x, int y, int width, int height);
void nuwan_listbox_add_item(nuwan_listbox_t* listbox, const char* item);
void nuwan_listbox_remove_item(nuwan_listbox_t* listbox, int index);
int nuwan_listbox_get_selected(nuwan_listbox_t* listbox);
char* nuwan_listbox_get_item_text(nuwan_listbox_t* listbox, int index);
void nuwan_listbox_destroy(nuwan_listbox_t* listbox);

// ============================================================================
// MessageBox API
// ============================================================================

// MessageBox types: 0=OK, 1=OKCANCEL, 2=YESNO, 3=INFO, 4=WARNING, 5=ERROR
void nuwan_messagebox_show(const char* title, const char* message, int type);

#endif // NUWAN_GUI_H
