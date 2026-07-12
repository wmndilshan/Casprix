/*
 * Win32 Skia Window
 *
 * Creates a native Win32 window backed by a Skia raster surface.
 * Skia renders to a pixel buffer → StretchDIBits blits to the HWND.
 *
 * Translates Win32 messages (WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_KEYDOWN, etc.)
 * into the unified SGEvent system.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "skia_window_host.h"

/* ========================================================================
 * SkiaWindow Structure
 * ======================================================================== */

typedef struct SkiaWindow {
    HWND hwnd;
    int width;
    int height;

    /* Skia rendering surface */
    SkiaSurface surface;
    void* pixel_data;          /* Points into DIB pixel buffer */
    int row_bytes;

    /* DIB section for blitting */
    BITMAPINFO bmi;
    HBITMAP dib_bitmap;
    void* dib_bits;            /* DIB-owned pixel memory */

    /* State */
    int needs_redraw;
    int running;
    int mouse_x, mouse_y;
    int mouse_buttons;         /* Bitmask of pressed buttons */
    int dpi_scale;             /* DPI scaling factor (100 = 1x, 200 = 2x) */

    /* Callbacks */
    SkiaWindowPaintCallback on_paint;
    SkiaWindowEventCallback on_event;
    void* paint_ctx;
    void* event_ctx;
} SkiaWindow;

/* Forward declarations */
static LRESULT CALLBACK skia_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static void skia_window_recreate_surface(SkiaWindow* win);

/* Window class name */
static const char* SKIA_WINDOW_CLASS = "CasperixSkiaWindow";
static int g_class_registered = 0;

static double skia_host_timestamp(void) {
    static LARGE_INTEGER freq = { 0 };
    LARGE_INTEGER now;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }

    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

static int skia_host_modifiers(void) {
    int mods = 0;

    if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= SG_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= SG_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000)    mods |= SG_MOD_ALT;
    if (GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000) {
        mods |= SG_MOD_SUPER;
    }

    return mods;
}

static void skia_window_emit_event(SkiaWindow* win, SGEvent* event) {
    if (!win || !event || !win->on_event) return;
    event->timestamp = skia_host_timestamp();
    win->on_event(win, event, win->event_ctx);
}

/* ========================================================================
 * Window Creation / Destruction
 * ======================================================================== */

SkiaWindow* skia_window_create(const char* title, int width, int height) {
    SkiaWindow* win = (SkiaWindow*)calloc(1, sizeof(SkiaWindow));
    if (!win) return NULL;

    win->width = width;
    win->height = height;
    win->needs_redraw = 1;
    win->running = 0;
    win->dpi_scale = 100;

    /* Register window class once */
    if (!g_class_registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = skia_wndproc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;  /* We paint everything via Skia */
        wc.lpszClassName = SKIA_WINDOW_CLASS;
        wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);

        if (!RegisterClassExA(&wc)) {
            fprintf(stderr, "Failed to register window class\n");
            free(win);
            return NULL;
        }
        g_class_registered = 1;
    }

    /* Calculate window rect for desired client area */
    RECT rc = { 0, 0, width, height };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rc, style, FALSE);

    win->hwnd = CreateWindowExA(
        0,
        SKIA_WINDOW_CLASS,
        title ? title : "Casperix",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL,
        GetModuleHandle(NULL),
        win  /* Pass SkiaWindow pointer via CREATESTRUCT */
    );

    if (!win->hwnd) {
        fprintf(stderr, "Failed to create window\n");
        free(win);
        return NULL;
    }

    /* Store SkiaWindow pointer in HWND user data */
    SetWindowLongPtrA(win->hwnd, GWLP_USERDATA, (LONG_PTR)win);

    /* Create Skia raster surface with DIB backing */
    skia_window_recreate_surface(win);

    return win;
}

void skia_window_destroy(SkiaWindow* win) {
    if (!win) return;
    if (win->surface) skia_surface_destroy(win->surface);
    if (win->dib_bitmap) DeleteObject(win->dib_bitmap);
    if (win->hwnd) DestroyWindow(win->hwnd);
    free(win);
}

/* ========================================================================
 * Surface Management
 * ======================================================================== */

static void skia_window_recreate_surface(SkiaWindow* win) {
    /* Destroy old surface and DIB */
    if (win->surface) {
        skia_surface_destroy(win->surface);
        win->surface = NULL;
    }
    if (win->dib_bitmap) {
        DeleteObject(win->dib_bitmap);
        win->dib_bitmap = NULL;
    }

    /* Create DIB section */
    memset(&win->bmi, 0, sizeof(win->bmi));
    win->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    win->bmi.bmiHeader.biWidth = win->width;
    win->bmi.bmiHeader.biHeight = -win->height;  /* Top-down DIB */
    win->bmi.bmiHeader.biPlanes = 1;
    win->bmi.bmiHeader.biBitCount = 32;
    win->bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(win->hwnd);
    win->dib_bitmap = CreateDIBSection(hdc, &win->bmi, DIB_RGB_COLORS,
                                        &win->dib_bits, NULL, 0);
    ReleaseDC(win->hwnd, hdc);

    if (!win->dib_bitmap || !win->dib_bits) {
        fprintf(stderr, "Failed to create DIB section\n");
        return;
    }

    win->row_bytes = win->width * 4;

    /* Create Skia surface backed by the DIB pixel buffer.
     * Skia renders BGRA8888 which matches the DIB format on Windows. */
    win->surface = skia_surface_make_raster_direct(
        win->width, win->height,
        win->dib_bits, win->row_bytes
    );

    if (!win->surface) {
        /* Fallback: create standalone Skia surface and copy pixels on present */
        win->surface = skia_surface_make_raster(win->width, win->height);
    }

    win->needs_redraw = 1;
}

/* ========================================================================
 * Presentation
 * ======================================================================== */

void skia_window_show(SkiaWindow* win) {
    if (win && win->hwnd) {
        ShowWindow(win->hwnd, SW_SHOW);
        UpdateWindow(win->hwnd);
    }
}

void skia_window_hide(SkiaWindow* win) {
    if (win && win->hwnd) ShowWindow(win->hwnd, SW_HIDE);
}

void skia_window_present(SkiaWindow* win) {
    if (!win || !win->hwnd || !win->surface) return;

    /* If using direct DIB surface, pixels are already in dib_bits.
     * If using standalone surface, copy pixels to DIB. */
    void* skia_pixels = skia_surface_peek_pixels(win->surface, NULL);
    if (skia_pixels && skia_pixels != win->dib_bits) {
        memcpy(win->dib_bits, skia_pixels, (size_t)win->row_bytes * win->height);
    }

    /* Blit DIB to window */
    HDC hdc = GetDC(win->hwnd);
    StretchDIBits(hdc,
        0, 0, win->width, win->height,
        0, 0, win->width, win->height,
        win->dib_bits, &win->bmi,
        DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(win->hwnd, hdc);

    win->needs_redraw = 0;
}

/* ========================================================================
 * Canvas Access
 * ======================================================================== */

SkiaCanvas skia_window_get_canvas(SkiaWindow* win) {
    if (!win || !win->surface) return NULL;
    return skia_surface_get_canvas(win->surface);
}

int skia_window_get_width(SkiaWindow* win) {
    return win ? win->width : 0;
}

int skia_window_get_height(SkiaWindow* win) {
    return win ? win->height : 0;
}

void skia_window_set_title(SkiaWindow* win, const char* title) {
    if (win && win->hwnd) SetWindowTextA(win->hwnd, title);
}

void skia_window_invalidate(SkiaWindow* win) {
    if (win) {
        win->needs_redraw = 1;
        if (win->hwnd) InvalidateRect(win->hwnd, NULL, FALSE);
    }
}

int skia_window_needs_redraw(SkiaWindow* win) {
    return win ? win->needs_redraw : 0;
}

/* ========================================================================
 * Callback Registration
 * ======================================================================== */

void skia_window_set_paint_callback(SkiaWindow* win,
    SkiaWindowPaintCallback cb, void* ctx) {
    if (!win) return;
    win->on_paint = cb;
    win->paint_ctx = ctx;
}

void skia_window_set_event_callback(SkiaWindow* win,
    SkiaWindowEventCallback cb, void* ctx) {
    if (!win) return;
    win->on_event = cb;
    win->event_ctx = ctx;
}

/* ========================================================================
 * Message Loop
 * ======================================================================== */

int skia_window_run(SkiaWindow* win) {
    if (!win) return -1;

    win->running = 1;
    skia_window_show(win);

    MSG msg;
    while (win->running) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                win->running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            /* Idle: repaint if needed */
            if (win->needs_redraw) {
                if (win->on_paint) {
                    SkiaCanvas canvas = skia_window_get_canvas(win);
                    win->on_paint(win, canvas, win->paint_ctx);
                }
                skia_window_present(win);
            }
            Sleep(1);  /* Don't burn CPU when idle */
        }
    }

    return 0;
}

/* Non-blocking: process all pending messages, return 0 if still running */
int skia_window_poll(SkiaWindow* win) {
    if (!win) return -1;

    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            win->running = 0;
            return -1;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void skia_window_quit(SkiaWindow* win) {
    if (win) win->running = 0;
}

/* ========================================================================
 * Window Procedure — Win32 Message Handler
 * ======================================================================== */

static LRESULT CALLBACK skia_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    SkiaWindow* win = (SkiaWindow*)(LONG_PTR)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTA* cs = (CREATESTRUCTA*)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (win && win->surface) {
                /* Trigger repaint callback */
                if (win->on_paint) {
                    SkiaCanvas canvas = skia_window_get_canvas(win);
                    win->on_paint(win, canvas, win->paint_ctx);
                }
                skia_window_present(win);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE: {
            if (!win) break;
            int new_width = LOWORD(lparam);
            int new_height = HIWORD(lparam);
            if (new_width > 0 && new_height > 0 &&
                (new_width != win->width || new_height != win->height)) {
                SGEvent evt = { 0 };
                win->width = new_width;
                win->height = new_height;
                skia_window_recreate_surface(win);
                evt.type = SG_EVENT_RESIZE;
                evt.data.resize.width = new_width;
                evt.data.resize.height = new_height;
                skia_window_emit_event(win, &evt);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!win) break;
            SGEvent evt = { 0 };
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            win->mouse_x = x;
            win->mouse_y = y;
            evt.type = SG_EVENT_MOUSE_MOVE;
            evt.mods = skia_host_modifiers();
            evt.data.mouse.x = (float)x;
            evt.data.mouse.y = (float)y;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!win) break;
            SetCapture(hwnd);
            SGEvent evt = { 0 };
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            win->mouse_buttons |= (1 << SG_BUTTON_LEFT);
            evt.type = SG_EVENT_MOUSE_DOWN;
            evt.mods = skia_host_modifiers();
            evt.data.mouse.x = (float)x;
            evt.data.mouse.y = (float)y;
            evt.data.mouse.button = SG_BUTTON_LEFT;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!win) break;
            ReleaseCapture();
            SGEvent evt = { 0 };
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            win->mouse_buttons &= ~(1 << SG_BUTTON_LEFT);
            evt.type = SG_EVENT_MOUSE_UP;
            evt.mods = skia_host_modifiers();
            evt.data.mouse.x = (float)x;
            evt.data.mouse.y = (float)y;
            evt.data.mouse.button = SG_BUTTON_LEFT;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_RBUTTONDOWN: {
            if (!win) break;
            SGEvent evt = { 0 };
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            win->mouse_buttons |= (1 << SG_BUTTON_RIGHT);
            evt.type = SG_EVENT_MOUSE_DOWN;
            evt.mods = skia_host_modifiers();
            evt.data.mouse.x = (float)x;
            evt.data.mouse.y = (float)y;
            evt.data.mouse.button = SG_BUTTON_RIGHT;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_RBUTTONUP: {
            if (!win) break;
            SGEvent evt = { 0 };
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            win->mouse_buttons &= ~(1 << SG_BUTTON_RIGHT);
            evt.type = SG_EVENT_MOUSE_UP;
            evt.mods = skia_host_modifiers();
            evt.data.mouse.x = (float)x;
            evt.data.mouse.y = (float)y;
            evt.data.mouse.button = SG_BUTTON_RIGHT;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!win) break;
            SGEvent evt = { 0 };
            int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            evt.type = SG_EVENT_MOUSE_SCROLL;
            evt.mods = skia_host_modifiers();
            evt.data.scroll.x = (float)win->mouse_x;
            evt.data.scroll.y = (float)win->mouse_y;
            evt.data.scroll.dx = 0.0f;
            evt.data.scroll.dy = (float)delta;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (!win) break;
            SGEvent evt = { 0 };
            int keycode = (int)wparam;
            evt.type = SG_EVENT_KEY_DOWN;
            evt.mods = skia_host_modifiers();
            evt.data.key.keycode = keycode;
            evt.data.key.scancode = (int)((lparam >> 16) & 0xFF);
            evt.data.key.repeat = (lparam & 0x40000000) ? 1 : 0;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (!win) break;
            SGEvent evt = { 0 };
            int keycode = (int)wparam;
            evt.type = SG_EVENT_KEY_UP;
            evt.mods = skia_host_modifiers();
            evt.data.key.keycode = keycode;
            evt.data.key.scancode = (int)((lparam >> 16) & 0xFF);
            evt.data.key.repeat = 0;
            skia_window_emit_event(win, &evt);
            return 0;
        }

        case WM_CHAR: {
            if (!win) break;
            /* Convert Windows wchar to UTF-8 */
            wchar_t wch = (wchar_t)wparam;
            char utf8[8] = {0};
            WideCharToMultiByte(CP_UTF8, 0, &wch, 1, utf8, sizeof(utf8) - 1, NULL, NULL);
            if (utf8[0] >= 32) {  /* Skip control characters */
                SGEvent evt = { 0 };
                evt.type = SG_EVENT_TEXT_INPUT;
                evt.mods = skia_host_modifiers();
                memcpy(evt.data.text_input.text, utf8, sizeof(evt.data.text_input.text) - 1);
                skia_window_emit_event(win, &evt);
            }
            return 0;
        }

        case WM_CLOSE:
            if (win) {
                SGEvent evt = { 0 };
                win->running = 0;
                evt.type = SG_EVENT_CLOSE;
                skia_window_emit_event(win, &evt);
            }
            PostQuitMessage(0);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1;  /* Skia handles all painting */
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

#else

#include "skia_window_host.h"
#include <stdlib.h>

struct SkiaWindow {
    int unsupported_platform;
};

SkiaWindow* skia_window_create(const char* title, int width, int height) {
    (void)title;
    (void)width;
    (void)height;
    return NULL;
}

void skia_window_destroy(SkiaWindow* win) { (void)win; }
void skia_window_show(SkiaWindow* win) { (void)win; }
void skia_window_hide(SkiaWindow* win) { (void)win; }
void skia_window_present(SkiaWindow* win) { (void)win; }
SkiaCanvas skia_window_get_canvas(SkiaWindow* win) { (void)win; return NULL; }
int skia_window_get_width(SkiaWindow* win) { (void)win; return 0; }
int skia_window_get_height(SkiaWindow* win) { (void)win; return 0; }
void skia_window_set_title(SkiaWindow* win, const char* title) { (void)win; (void)title; }
void skia_window_invalidate(SkiaWindow* win) { (void)win; }
int skia_window_needs_redraw(SkiaWindow* win) { (void)win; return 0; }
int skia_window_run(SkiaWindow* win) { (void)win; return -1; }
int skia_window_poll(SkiaWindow* win) { (void)win; return -1; }
void skia_window_quit(SkiaWindow* win) { (void)win; }
void skia_window_set_paint_callback(SkiaWindow* win, SkiaWindowPaintCallback cb, void* ctx) {
    (void)win; (void)cb; (void)ctx;
}
void skia_window_set_event_callback(SkiaWindow* win, SkiaWindowEventCallback cb, void* ctx) {
    (void)win; (void)cb; (void)ctx;
}

#endif /* _WIN32 */
