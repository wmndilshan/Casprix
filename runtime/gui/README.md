# Casprix GUI Library

A comprehensive cross-platform GUI library for the Casprix programming language, built using native Windows API (Win32) and designed for future Linux support (GTK/X11).

## Features

### Core Components

1. **Window Management**
   - Create and manage application windows
   - Show/hide windows
   - Add child widgets
   - Full event handling system

2. **Basic Controls**
   - **Button**: Clickable buttons with callback support
   - **Label**: Static text display with color customization
   - **TextBox**: Single-line and multi-line text input
   - **CheckBox**: Boolean checkbox controls
   - **ComboBox**: Dropdown selection lists
   - **ListBox**: Scrollable list of items
   - **ProgressBar**: Visual progress indication

3. **Dialogs**
   - MessageBox with multiple types (Info, Warning, Error)
   - Support for OK, OK/Cancel, Yes/No buttons

### Architecture

#### Event System
- Callback-based event handling
- Button click events via `on_click` callbacks
- Window message loop (`nuwan_gui_run()`)

#### Widget Hierarchy
- Base widget structure with common properties
- Type-safe widget casting
- Parent-child relationships

## API Overview

### Initialization and Cleanup

```c
void nuwan_gui_init();              // Initialize GUI system
void nuwan_gui_cleanup();           // Cleanup resources
void nuwan_gui_run();               // Run event loop (blocking)
void nuwan_gui_quit();              // Exit event loop
```

### Window API

```c
nuwan_window_t* nuwan_window_create(const char* title, int width, int height);
void nuwan_window_show(nuwan_window_t* window);
void nuwan_window_hide(nuwan_window_t* window);
void nuwan_window_add_widget(nuwan_window_t* window, nuwan_widget_t* widget);
void nuwan_window_destroy(nuwan_window_t* window);
```

### Button API

```c
nuwan_button_t* nuwan_button_create(const char* text, int x, int y, int width, int height);
void nuwan_button_set_callback(nuwan_button_t* button, void (*callback)(nuwan_widget_t*));
void nuwan_button_destroy(nuwan_button_t* button);
```

### Label API

```c
nuwan_label_t* nuwan_label_create(const char* text, int x, int y, int width, int height);
void nuwan_label_set_text(nuwan_label_t* label, const char* text);
void nuwan_label_set_color(nuwan_label_t* label, nuwan_color_t color);
void nuwan_label_destroy(nuwan_label_t* label);
```

### TextBox API

```c
nuwan_textbox_t* nuwan_textbox_create(int x, int y, int width, int height, bool multiline);
char* nuwan_textbox_get_text(nuwan_textbox_t* textbox);
void nuwan_textbox_set_text(nuwan_textbox_t* textbox, const char* text);
void nuwan_textbox_destroy(nuwan_textbox_t* textbox);
```

### CheckBox API

```c
nuwan_checkbox_t* nuwan_checkbox_create(const char* text, int x, int y, int width, int height);
bool nuwan_checkbox_is_checked(nuwan_checkbox_t* checkbox);
void nuwan_checkbox_set_checked(nuwan_checkbox_t* checkbox, bool checked);
void nuwan_checkbox_destroy(nuwan_checkbox_t* checkbox);
```

### ComboBox API

```c
nuwan_combobox_t* nuwan_combobox_create(int x, int y, int width, int height);
void nuwan_combobox_add_item(nuwan_combobox_t* combo, const char* item);
int nuwan_combobox_get_selected(nuwan_combobox_t* combo);
void nuwan_combobox_set_selected(nuwan_combobox_t* combo, int index);
void nuwan_combobox_destroy(nuwan_combobox_t* combo);
```

### ListBox API

```c
nuwan_listbox_t* nuwan_listbox_create(int x, int y, int width, int height);
void nuwan_listbox_add_item(nuwan_listbox_t* listbox, const char* item);
void nuwan_listbox_remove_item(nuwan_listbox_t* listbox, int index);
int nuwan_listbox_get_selected(nuwan_listbox_t* listbox);
char* nuwan_listbox_get_item_text(nuwan_listbox_t* listbox, int index);
void nuwan_listbox_destroy(nuwan_listbox_t* listbox);
```

### ProgressBar API

```c
nuwan_progressbar_t* nuwan_progressbar_create(int x, int y, int width, int height);
void nuwan_progressbar_set_value(nuwan_progressbar_t* progress, int value);
int nuwan_progressbar_get_value(nuwan_progressbar_t* progress);
void nuwan_progressbar_destroy(nuwan_progressbar_t* progress);
```

### MessageBox API

```c
// MessageBox types: 0=OK, 1=OKCANCEL, 2=YESNO, 3=INFO, 4=WARNING, 5=ERROR
void nuwan_messagebox_show(const char* title, const char* message, int type);
```

## Example Usage

### Simple Hello World

```c
#include "nuwan/gui.h"

void on_button_click(nuwan_widget_t* widget) {
    nuwan_messagebox_show("Hello", "Button clicked!", 3);
}

int main() {
    nuwan_gui_init();

    nuwan_window_t* window = nuwan_window_create("Hello World", 400, 300);

    nuwan_button_t* button = nuwan_button_create("Click Me", 150, 120, 100, 30);
    nuwan_button_set_callback(button, on_button_click);

    nuwan_window_add_widget(window, (nuwan_widget_t*)button);
    nuwan_window_show(window);

    nuwan_gui_run();

    nuwan_window_destroy(window);
    nuwan_gui_cleanup();

    return 0;
}
```

### Calculator Example

See [examples/gui/gui_calculator.cpx](../../examples/gui/gui_calculator.cpx) for a Skia/GUI sample (requires building with `ENABLE_SKIA_GUI=ON` where applicable).

### Other samples

See [examples/gui/](../../examples/gui/) for additional `.cpx` demos (`gui_hello.cpx`, `gui_test.cpx`, `modern_gui_demo.cpx`, …).

## Building

### Windows (MinGW/MSYS2)

```bash
# Compile GUI runtime
gcc -c runtime/gui/gui_runtime.c -o obj/gui_runtime.o -Iinclude

# Compile and link your application
gcc your_app.c obj/gui_runtime.o -o your_app.exe -lcomctl32 -lgdi32 -luser32
```

### Required Libraries (Windows)
- `comctl32` - Common Controls
- `gdi32` - Graphics Device Interface
- `user32` - User Interface

## Platform Support

### Windows
- ✅ Fully implemented using Win32 API
- ✅ Common Controls (ComCtl32)
- ✅ Native look and feel
- ✅ Full event handling

### Linux
- ⏳ Planned (GTK or X11)
- Stub implementations currently return NULL/error codes

## Technical Details

### Widget Registry
- Supports up to 1024 concurrent widgets
- Automatic handle management
- Type-safe widget tracking

### Memory Management
- Manual memory allocation with proper cleanup
- Destroy functions free all allocated resources
- Window destruction automatically cleans up children

### Thread Safety
- GUI operations should be performed on the main thread
- Event loop runs on the main thread
- No built-in thread synchronization

## Limitations

1. **Coordinate System**: Absolute positioning only (no automatic layout)
2. **Styling**: Limited customization (uses native OS styling)
3. **Validation**: Minimal input validation
4. **Events**: Button click events only (no hover, key press, etc.)
5. **Menus**: Not yet implemented
6. **Custom Drawing**: Not supported

## Future Enhancements

- [ ] Linux/GTK implementation
- [ ] Automatic layout managers (Grid, Box, Flow)
- [ ] More event types (hover, focus, key events)
- [ ] Menu bar and context menus
- [ ] Toolbar and status bar
- [ ] Custom drawing canvas
- [ ] Drag and drop support
- [ ] Tab controls and panels
- [ ] Tree view and data grids
- [ ] Rich text editing
- [ ] Image display widgets

## License

Part of the Casprix programming language project.
