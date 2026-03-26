#include "../runtime.h"
#include "../../include/casprix/gui.h"
#include "../../include/casprix/stdlib.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Platform-specific includes and definitions
// ============================================================================

#ifdef _WIN32
    #include <windows.h>
    #include <commctrl.h>
    #pragma comment(lib, "comctl32.lib")

    #define WINDOW_CLASS_NAME "CasprixWindowClass"
    #define BUTTON_ID_BASE 1000
    #define CONTROL_ID_BASE 2000

    // Global application instance
    static HINSTANCE g_hInstance = NULL;
    static bool g_gui_initialized = false;
    static bool g_should_quit = false;
    static WNDCLASSEX g_wc = {0};

#else
    // For future Linux/GTK implementation
    // For now, provide stub implementations
#endif

// ============================================================================
// Widget registry for tracking all created widgets
// ============================================================================

#define MAX_WIDGETS 1024
static nuwan_widget_t* g_widget_registry[MAX_WIDGETS] = {NULL};
static int g_next_widget_id = 1;

static int register_widget(nuwan_widget_t* widget) {
    if (!widget) return -1;

    for (int i = 1; i < MAX_WIDGETS; i++) {
        if (g_widget_registry[i] == NULL) {
            g_widget_registry[i] = widget;
            return i;
        }
    }
    return -1;
}

static void unregister_widget(int id) {
    if (id > 0 && id < MAX_WIDGETS) {
        g_widget_registry[id] = NULL;
    }
}

static nuwan_widget_t* get_widget(int id) {
    if (id > 0 && id < MAX_WIDGETS) {
        return g_widget_registry[id];
    }
    return NULL;
}

// ============================================================================
// Windows Implementation
// ============================================================================

#ifdef _WIN32

// Forward declarations
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// GUI Initialization and Cleanup
// ============================================================================

void nuwan_gui_init() {
    if (g_gui_initialized) return;

    g_hInstance = GetModuleHandle(NULL);

    // Initialize common controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    // Register window class
    g_wc.cbSize = sizeof(WNDCLASSEX);
    g_wc.style = CS_HREDRAW | CS_VREDRAW;
    g_wc.lpfnWndProc = WindowProc;
    g_wc.cbClsExtra = 0;
    g_wc.cbWndExtra = 0;
    g_wc.hInstance = g_hInstance;
    g_wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    g_wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    g_wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    g_wc.lpszMenuName = NULL;
    g_wc.lpszClassName = WINDOW_CLASS_NAME;
    g_wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    RegisterClassEx(&g_wc);

    g_gui_initialized = true;
    g_should_quit = false;
}

void nuwan_gui_cleanup() {
    if (!g_gui_initialized) return;

    // Clean up all registered widgets
    for (int i = 1; i < MAX_WIDGETS; i++) {
        if (g_widget_registry[i] != NULL) {
            nuwan_widget_t* widget = g_widget_registry[i];
            if (widget->handle && widget->type != NUWAN_WIDGET_WINDOW) {
                DestroyWindow((HWND)widget->handle);
            }
        }
    }

    UnregisterClass(WINDOW_CLASS_NAME, g_hInstance);
    g_gui_initialized = false;
}

void nuwan_gui_run() {
    printf("Entering GUI event loop...\n");
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, NULL, 0, 0)) && !g_should_quit) {
        if (bRet == -1) {
            printf("GetMessage error\n");
            break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    printf("Exiting GUI event loop (g_should_quit=%d, bRet=%d)\n", g_should_quit, bRet);
}

void nuwan_gui_quit() {
    g_should_quit = true;
    PostQuitMessage(0);
}

// ============================================================================
// Event Handler
// ============================================================================

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_COMMAND: {
            // Button clicks and menu commands
            int control_id = LOWORD(wParam);
            HWND control_hwnd = (HWND)lParam;

            // Find widget by handle
            for (int i = 1; i < MAX_WIDGETS; i++) {
                nuwan_widget_t* widget = g_widget_registry[i];
                if (widget && widget->handle == (widget_handle_t)control_hwnd) {
                    if (widget->on_click) {
                        widget->on_click(widget);
                    }
                    break;
                }
            }
            return 0;
        }

        case WM_CLOSE:
            printf("WM_CLOSE received\n");
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            printf("WM_DESTROY received\n");
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

// ============================================================================
// Window Implementation
// ============================================================================

nuwan_window_t* nuwan_window_create(const char* title, int width, int height) {
    if (!g_gui_initialized) {
        nuwan_gui_init();
    }

    nuwan_window_t* window = (nuwan_window_t*)calloc(1, sizeof(nuwan_window_t));
    if (!window) return NULL;

    // Initialize base widget
    window->base.type = NUWAN_WIDGET_WINDOW;
    window->base.x = CW_USEDEFAULT;
    window->base.y = CW_USEDEFAULT;
    window->base.width = width;
    window->base.height = height;
    window->base.visible = false;
    window->base.enabled = true;
    window->base.parent = NULL;

    // Initialize window-specific fields
    window->title = title ? strdup(title) : strdup("Casprix Window");
    window->resizable = true;
    window->has_menu = false;
    window->child_count = 0;
    window->children = NULL;

    // Create native window
    DWORD style = WS_OVERLAPPEDWINDOW;
    HWND hwnd = CreateWindowEx(
        0,
        WINDOW_CLASS_NAME,
        window->title,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height,
        NULL, NULL, g_hInstance, NULL
    );

    if (!hwnd) {
        free(window->title);
        free(window);
        return NULL;
    }

    window->base.handle = (widget_handle_t)hwnd;

    // Store window pointer in window user data
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)window);

    return window;
}

void nuwan_window_show(nuwan_window_t* window) {
    if (!window || !window->base.handle) return;

    ShowWindow((HWND)window->base.handle, SW_SHOW);
    UpdateWindow((HWND)window->base.handle);
    window->base.visible = true;
}

void nuwan_window_hide(nuwan_window_t* window) {
    if (!window || !window->base.handle) return;

    ShowWindow((HWND)window->base.handle, SW_HIDE);
    window->base.visible = false;
}

void nuwan_window_add_widget(nuwan_window_t* window, nuwan_widget_t* widget) {
    if (!window || !widget) return;

    // Allocate or expand children array
    window->children = (nuwan_widget_t**)realloc(
        window->children,
        (window->child_count + 1) * sizeof(nuwan_widget_t*)
    );

    window->children[window->child_count] = widget;
    window->child_count++;
    widget->parent = &window->base;

    // Set parent for native control
    if (widget->handle) {
        SetParent((HWND)widget->handle, (HWND)window->base.handle);
    }
}

void nuwan_window_destroy(nuwan_window_t* window) {
    if (!window) return;

    // Destroy all children
    for (int i = 0; i < window->child_count; i++) {
        nuwan_widget_t* child = window->children[i];
        if (child && child->handle) {
            DestroyWindow((HWND)child->handle);
            free(child);
        }
    }

    free(window->children);
    free(window->title);

    if (window->base.handle) {
        DestroyWindow((HWND)window->base.handle);
    }

    free(window);
}

// ============================================================================
// Button Implementation
// ============================================================================

nuwan_button_t* nuwan_button_create(const char* text, int x, int y, int width, int height) {
    nuwan_button_t* button = (nuwan_button_t*)calloc(1, sizeof(nuwan_button_t));
    if (!button) return NULL;

    // Initialize base widget
    button->base.type = NUWAN_WIDGET_BUTTON;
    button->base.x = x;
    button->base.y = y;
    button->base.width = width;
    button->base.height = height;
    button->base.visible = true;
    button->base.enabled = true;
    button->base.text = text ? strdup(text) : strdup("Button");
    button->base.on_click = NULL;
    button->base.parent = NULL;

    // Create native button (initially without parent)
    HWND hwnd = CreateWindowEx(
        0,
        "BUTTON",
        button->base.text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, width, height,
        NULL,  // Parent will be set when added to window
        (HMENU)(BUTTON_ID_BASE + g_next_widget_id),
        g_hInstance,
        NULL
    );

    if (!hwnd) {
        free(button->base.text);
        free(button);
        return NULL;
    }

    button->base.handle = (widget_handle_t)hwnd;

    // Set default font
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);

    return button;
}

void nuwan_button_set_callback(nuwan_button_t* button, void (*callback)(nuwan_widget_t*)) {
    if (!button) return;
    button->base.on_click = callback;
}

void nuwan_button_destroy(nuwan_button_t* button) {
    if (!button) return;

    if (button->base.handle) {
        DestroyWindow((HWND)button->base.handle);
    }

    free(button->base.text);
    free(button);
}

// ============================================================================
// Label Implementation
// ============================================================================

nuwan_label_t* nuwan_label_create(const char* text, int x, int y, int width, int height) {
    nuwan_label_t* label = (nuwan_label_t*)calloc(1, sizeof(nuwan_label_t));
    if (!label) return NULL;

    // Initialize base widget
    label->base.type = NUWAN_WIDGET_LABEL;
    label->base.x = x;
    label->base.y = y;
    label->base.width = width;
    label->base.height = height;
    label->base.visible = true;
    label->base.enabled = true;
    label->base.text = text ? strdup(text) : strdup("Label");
    label->base.parent = NULL;

    // Default colors
    label->text_color.r = 0;
    label->text_color.g = 0;
    label->text_color.b = 0;
    label->text_color.a = 255;

    label->bg_color.r = 240;
    label->bg_color.g = 240;
    label->bg_color.b = 240;
    label->bg_color.a = 255;

    // Create native label (STATIC control)
    HWND hwnd = CreateWindowEx(
        0,
        "STATIC",
        label->base.text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, width, height,
        NULL,
        (HMENU)(CONTROL_ID_BASE + g_next_widget_id),
        g_hInstance,
        NULL
    );

    if (!hwnd) {
        free(label->base.text);
        free(label);
        return NULL;
    }

    label->base.handle = (widget_handle_t)hwnd;

    // Set default font
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);

    return label;
}

void nuwan_label_set_text(nuwan_label_t* label, const char* text) {
    if (!label || !text) return;

    free(label->base.text);
    label->base.text = strdup(text);

    if (label->base.handle) {
        SetWindowText((HWND)label->base.handle, text);
    }
}

void nuwan_label_set_color(nuwan_label_t* label, nuwan_color_t color) {
    if (!label) return;
    label->text_color = color;

    // Note: Setting text color in Windows requires custom drawing
    // or subclassing the control. For simplicity, this is omitted.
}

void nuwan_label_destroy(nuwan_label_t* label) {
    if (!label) return;

    if (label->base.handle) {
        DestroyWindow((HWND)label->base.handle);
    }

    free(label->base.text);
    free(label);
}

// ============================================================================
// TextBox Implementation
// ============================================================================

nuwan_textbox_t* nuwan_textbox_create(int x, int y, int width, int height, bool multiline) {
    nuwan_textbox_t* textbox = (nuwan_textbox_t*)calloc(1, sizeof(nuwan_textbox_t));
    if (!textbox) return NULL;

    // Initialize base widget
    textbox->base.type = NUWAN_WIDGET_TEXTBOX;
    textbox->base.x = x;
    textbox->base.y = y;
    textbox->base.width = width;
    textbox->base.height = height;
    textbox->base.visible = true;
    textbox->base.enabled = true;
    textbox->base.text = strdup("");
    textbox->base.parent = NULL;

    textbox->multiline = multiline;
    textbox->readonly = false;
    textbox->max_length = 32767;

    // Create native edit control
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
    if (multiline) {
        style |= ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL;
    }

    HWND hwnd = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        "EDIT",
        "",
        style,
        x, y, width, height,
        NULL,
        (HMENU)(CONTROL_ID_BASE + g_next_widget_id + 100),
        g_hInstance,
        NULL
    );

    if (!hwnd) {
        free(textbox->base.text);
        free(textbox);
        return NULL;
    }

    textbox->base.handle = (widget_handle_t)hwnd;

    // Set default font
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Set text limit
    SendMessage(hwnd, EM_LIMITTEXT, (WPARAM)textbox->max_length, 0);

    return textbox;
}

char* nuwan_textbox_get_text(nuwan_textbox_t* textbox) {
    if (!textbox || !textbox->base.handle) return NULL;

    HWND hwnd = (HWND)textbox->base.handle;
    int len = GetWindowTextLength(hwnd);

    if (len == 0) {
        return strdup("");
    }

    char* buffer = (char*)malloc(len + 1);
    if (!buffer) return NULL;

    GetWindowText(hwnd, buffer, len + 1);
    return buffer;
}

void nuwan_textbox_set_text(nuwan_textbox_t* textbox, const char* text) {
    if (!textbox || !textbox->base.handle) return;

    free(textbox->base.text);
    textbox->base.text = text ? strdup(text) : strdup("");

    SetWindowText((HWND)textbox->base.handle, textbox->base.text);
}

void nuwan_textbox_destroy(nuwan_textbox_t* textbox) {
    if (!textbox) return;

    if (textbox->base.handle) {
        DestroyWindow((HWND)textbox->base.handle);
    }

    free(textbox->base.text);
    free(textbox);
}

// ============================================================================
// Additional Components
// ============================================================================

// CheckBox
nuwan_checkbox_t* nuwan_checkbox_create(const char* text, int x, int y, int width, int height) {
    nuwan_checkbox_t* checkbox = (nuwan_checkbox_t*)calloc(1, sizeof(nuwan_checkbox_t));
    if (!checkbox) return NULL;

    checkbox->base.type = NUWAN_WIDGET_BUTTON;  // Using button type for now
    checkbox->base.x = x;
    checkbox->base.y = y;
    checkbox->base.width = width;
    checkbox->base.height = height;
    checkbox->base.visible = true;
    checkbox->base.enabled = true;
    checkbox->base.text = text ? strdup(text) : strdup("CheckBox");
    checkbox->checked = false;

    HWND hwnd = CreateWindowEx(
        0,
        "BUTTON",
        checkbox->base.text,
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, width, height,
        NULL,
        (HMENU)(CONTROL_ID_BASE + g_next_widget_id + 200),
        g_hInstance,
        NULL
    );

    if (!hwnd) {
        free(checkbox->base.text);
        free(checkbox);
        return NULL;
    }

    checkbox->base.handle = (widget_handle_t)hwnd;

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);

    return checkbox;
}

bool nuwan_checkbox_is_checked(nuwan_checkbox_t* checkbox) {
    if (!checkbox || !checkbox->base.handle) return false;
    return SendMessage((HWND)checkbox->base.handle, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void nuwan_checkbox_set_checked(nuwan_checkbox_t* checkbox, bool checked) {
    if (!checkbox || !checkbox->base.handle) return;
    SendMessage((HWND)checkbox->base.handle, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    checkbox->checked = checked;
}

void nuwan_checkbox_destroy(nuwan_checkbox_t* checkbox) {
    if (!checkbox) return;
    if (checkbox->base.handle) {
        DestroyWindow((HWND)checkbox->base.handle);
    }
    free(checkbox->base.text);
    free(checkbox);
}

// ComboBox (Dropdown)
nuwan_combobox_t* nuwan_combobox_create(int x, int y, int width, int height) {
    nuwan_combobox_t* combo = (nuwan_combobox_t*)calloc(1, sizeof(nuwan_combobox_t));
    if (!combo) return NULL;

    combo->base.type = NUWAN_WIDGET_TEXTBOX;  // Using textbox type for now
    combo->base.x = x;
    combo->base.y = y;
    combo->base.width = width;
    combo->base.height = height;
    combo->base.visible = true;
    combo->base.enabled = true;
    combo->item_count = 0;
    combo->selected_index = -1;

    HWND hwnd = CreateWindowEx(
        0,
        "COMBOBOX",
        "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, width, height,
        NULL,
        (HMENU)(CONTROL_ID_BASE + g_next_widget_id + 300),
        g_hInstance,
        NULL
    );

    if (!hwnd) {
        free(combo);
        return NULL;
    }

    combo->base.handle = (widget_handle_t)hwnd;

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);

    return combo;
}

void nuwan_combobox_add_item(nuwan_combobox_t* combo, const char* item) {
    if (!combo || !combo->base.handle || !item) return;

    SendMessage((HWND)combo->base.handle, CB_ADDSTRING, 0, (LPARAM)item);
    combo->item_count++;
}

int nuwan_combobox_get_selected(nuwan_combobox_t* combo) {
    if (!combo || !combo->base.handle) return -1;
    return SendMessage((HWND)combo->base.handle, CB_GETCURSEL, 0, 0);
}

void nuwan_combobox_set_selected(nuwan_combobox_t* combo, int index) {
    if (!combo || !combo->base.handle) return;
    SendMessage((HWND)combo->base.handle, CB_SETCURSEL, (WPARAM)index, 0);
    combo->selected_index = index;
}

void nuwan_combobox_destroy(nuwan_combobox_t* combo) {
    if (!combo) return;
    if (combo->base.handle) {
        DestroyWindow((HWND)combo->base.handle);
    }
    free(combo);
}

// ProgressBar
nuwan_progressbar_t* nuwan_progressbar_create(int x, int y, int width, int height) {
    nuwan_progressbar_t* progress = (nuwan_progressbar_t*)calloc(1, sizeof(nuwan_progressbar_t));
    if (!progress) return NULL;

    progress->base.type = NUWAN_WIDGET_PANEL;  // Using panel type for now
    progress->base.x = x;
    progress->base.y = y;
    progress->base.width = width;
    progress->base.height = height;
    progress->base.visible = true;
    progress->base.enabled = true;
    progress->min_value = 0;
    progress->max_value = 100;
    progress->current_value = 0;

    HWND hwnd = CreateWindowEx(
        0,
        PROGRESS_CLASS,
        NULL,
        WS_CHILD | WS_VISIBLE,
        x, y, width, height,
        NULL,
        (HMENU)(CONTROL_ID_BASE + g_next_widget_id + 400),
        g_hInstance,
        NULL
    );

    if (!hwnd) {
        free(progress);
        return NULL;
    }

    progress->base.handle = (widget_handle_t)hwnd;

    SendMessage(hwnd, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(hwnd, PBM_SETPOS, 0, 0);

    return progress;
}

void nuwan_progressbar_set_value(nuwan_progressbar_t* progress, int value) {
    if (!progress || !progress->base.handle) return;

    if (value < progress->min_value) value = progress->min_value;
    if (value > progress->max_value) value = progress->max_value;

    progress->current_value = value;
    SendMessage((HWND)progress->base.handle, PBM_SETPOS, (WPARAM)value, 0);
}

int nuwan_progressbar_get_value(nuwan_progressbar_t* progress) {
    if (!progress) return 0;
    return progress->current_value;
}

void nuwan_progressbar_destroy(nuwan_progressbar_t* progress) {
    if (!progress) return;
    if (progress->base.handle) {
        DestroyWindow((HWND)progress->base.handle);
    }
    free(progress);
}

// ListBox
nuwan_listbox_t* nuwan_listbox_create(int x, int y, int width, int height) {
    nuwan_listbox_t* listbox = (nuwan_listbox_t*)calloc(1, sizeof(nuwan_listbox_t));
    if (!listbox) return NULL;

    listbox->base.type = NUWAN_WIDGET_TEXTBOX;  // Using textbox type for now
    listbox->base.x = x;
    listbox->base.y = y;
    listbox->base.width = width;
    listbox->base.height = height;
    listbox->base.visible = true;
    listbox->base.enabled = true;
    listbox->item_count = 0;
    listbox->selected_index = -1;

    HWND hwnd = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        "LISTBOX",
        NULL,
        WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
        x, y, width, height,
        NULL,
        (HMENU)(CONTROL_ID_BASE + g_next_widget_id + 500),
        g_hInstance,
        NULL
    );

    if (!hwnd) {
        free(listbox);
        return NULL;
    }

    listbox->base.handle = (widget_handle_t)hwnd;

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);

    return listbox;
}

void nuwan_listbox_add_item(nuwan_listbox_t* listbox, const char* item) {
    if (!listbox || !listbox->base.handle || !item) return;

    SendMessage((HWND)listbox->base.handle, LB_ADDSTRING, 0, (LPARAM)item);
    listbox->item_count++;
}

void nuwan_listbox_remove_item(nuwan_listbox_t* listbox, int index) {
    if (!listbox || !listbox->base.handle) return;

    SendMessage((HWND)listbox->base.handle, LB_DELETESTRING, (WPARAM)index, 0);
    listbox->item_count--;
}

int nuwan_listbox_get_selected(nuwan_listbox_t* listbox) {
    if (!listbox || !listbox->base.handle) return -1;
    return SendMessage((HWND)listbox->base.handle, LB_GETCURSEL, 0, 0);
}

char* nuwan_listbox_get_item_text(nuwan_listbox_t* listbox, int index) {
    if (!listbox || !listbox->base.handle) return NULL;

    int len = SendMessage((HWND)listbox->base.handle, LB_GETTEXTLEN, (WPARAM)index, 0);
    if (len == LB_ERR) return NULL;

    char* buffer = (char*)malloc(len + 1);
    if (!buffer) return NULL;

    SendMessage((HWND)listbox->base.handle, LB_GETTEXT, (WPARAM)index, (LPARAM)buffer);
    return buffer;
}

void nuwan_listbox_destroy(nuwan_listbox_t* listbox) {
    if (!listbox) return;
    if (listbox->base.handle) {
        DestroyWindow((HWND)listbox->base.handle);
    }
    free(listbox);
}

// MessageBox
void nuwan_messagebox_show(const char* title, const char* message, int type) {
    UINT uType = MB_OK;

    switch (type) {
        case 1: uType = MB_OKCANCEL; break;
        case 2: uType = MB_YESNO; break;
        case 3: uType = MB_ICONINFORMATION; break;
        case 4: uType = MB_ICONWARNING; break;
        case 5: uType = MB_ICONERROR; break;
    }

    MessageBox(NULL, message ? message : "", title ? title : "Casprix", uType);
}

#else // Linux/POSIX stub implementations

// ============================================================================
// Linux Stub Implementations (for future GTK/X11 implementation)
// ============================================================================

void nuwan_gui_init() {
    printf("GUI not implemented on this platform\n");
}

void nuwan_gui_cleanup() {}

void nuwan_gui_run() {}

void nuwan_gui_quit() {}

nuwan_window_t* nuwan_window_create(const char* title, int width, int height) {
    return NULL;
}

void nuwan_window_show(nuwan_window_t* window) {}

void nuwan_window_hide(nuwan_window_t* window) {}

void nuwan_window_add_widget(nuwan_window_t* window, nuwan_widget_t* widget) {}

void nuwan_window_destroy(nuwan_window_t* window) {}

nuwan_button_t* nuwan_button_create(const char* text, int x, int y, int width, int height) {
    return NULL;
}

void nuwan_button_set_callback(nuwan_button_t* button, void (*callback)(nuwan_widget_t*)) {}

void nuwan_button_destroy(nuwan_button_t* button) {}

nuwan_label_t* nuwan_label_create(const char* text, int x, int y, int width, int height) {
    return NULL;
}

void nuwan_label_set_text(nuwan_label_t* label, const char* text) {}

void nuwan_label_set_color(nuwan_label_t* label, nuwan_color_t color) {}

void nuwan_label_destroy(nuwan_label_t* label) {}

nuwan_textbox_t* nuwan_textbox_create(int x, int y, int width, int height, bool multiline) {
    return NULL;
}

char* nuwan_textbox_get_text(nuwan_textbox_t* textbox) {
    return NULL;
}

void nuwan_textbox_set_text(nuwan_textbox_t* textbox, const char* text) {}

void nuwan_textbox_destroy(nuwan_textbox_t* textbox) {}

void nuwan_messagebox_show(const char* title, const char* message, int type) {
    printf("%s: %s\n", title ? title : "Casprix", message ? message : "");
}

#endif // _WIN32
