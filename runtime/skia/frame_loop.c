/*
 * Frame Loop — Main application run loop
 *
 * Ties together window, scene graph, layout, events, animations,
 * font management, and text caching into a 60 FPS render loop.
 */

#include "frame_loop.h"
#include "layout.h"
#include "style.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ========================================================================
 * High-Resolution Timer
 * ======================================================================== */

static double get_time_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

/* ========================================================================
 * Host Event Callback → SGEvent dispatch
 * ======================================================================== */

static void on_window_event_callback(SkiaWindow* win, const SGEvent* event, void* ctx) {
    SGApp* app = (SGApp*)ctx;
    if (!app || !event) return;
    (void)win;

    switch (event->type) {
        case SG_EVENT_RESIZE:
            app->width = event->data.resize.width;
            app->height = event->data.resize.height;
            app->needs_layout = 1;
            app->needs_repaint = 1;
            if (app->events) {
                SGEvent forwarded = *event;
                sg_dispatch_event(app->events, &forwarded);
            }
            break;

        case SG_EVENT_CLOSE:
            app->running = 0;
            break;

        default:
            if (app->events) {
                SGEvent forwarded = *event;
                sg_dispatch_event(app->events, &forwarded);
            }
            app->needs_repaint = 1;
            break;
    }
}

/* ========================================================================
 * Application Lifecycle
 * ======================================================================== */

SGApp* sg_app_create(const char* title, int width, int height) {
    SGApp* app = (SGApp*)calloc(1, sizeof(SGApp));
    if (!app) return NULL;

    app->width = width;
    app->height = height;
    app->target_fps = 60;
    app->needs_layout = 1;
    app->needs_repaint = 1;

    /* Create window */
    app->window = skia_window_create(title, width, height);
    if (!app->window) {
        free(app);
        return NULL;
    }

    /* Create subsystems */
    app->animations = sg_animation_pool_create();
    app->fonts = font_manager_create();
    app->text_cache = text_cache_create();

    /* Wire up window callbacks */
    skia_window_set_event_callback(app->window, on_window_event_callback, app);

    return app;
}

void sg_app_destroy(SGApp* app) {
    if (!app) return;

    if (app->on_cleanup) {
        app->on_cleanup(app->user_data);
    }

    if (app->root) {
        app->root->ownership_flags &= ~SG_NODE_OWNER_APP;
        sg_node_destroy(app->root);
    }
    if (app->events) sg_event_manager_destroy(app->events);
    if (app->animations) sg_animation_pool_destroy(app->animations);
    if (app->fonts) font_manager_destroy(app->fonts);
    if (app->text_cache) text_cache_destroy(app->text_cache);
    if (app->window) skia_window_destroy(app->window);

    free(app);
}

/* ========================================================================
 * Configuration
 * ======================================================================== */

void sg_app_set_root(SGApp* app, SGNode* root) {
    if (!app) return;

    /* Clean up old event manager */
    if (app->events) {
        sg_event_manager_destroy(app->events);
        app->events = NULL;
    }

    if (app->root) {
        app->root->ownership_flags &= ~SG_NODE_OWNER_APP;
        sg_node_destroy(app->root);
    }

    app->root = root;
    if (root) {
        sg_node_retain(root);
        root->ownership_flags |= SG_NODE_OWNER_APP;
        app->events = sg_event_manager_create(root);
        sg_focus_rebuild_tab_order(app->events);
    }
    app->needs_layout = 1;
    app->needs_repaint = 1;
}

void sg_app_set_fps(SGApp* app, int fps) {
    if (app && fps > 0) app->target_fps = fps;
}

void sg_app_on_frame(SGApp* app, void (*callback)(void* app, float dt)) {
    if (app) app->on_frame = callback;
}

void sg_app_on_setup(SGApp* app, void (*callback)(void* app)) {
    if (app) app->on_setup = callback;
}

void sg_app_on_cleanup(SGApp* app, void (*callback)(void* app)) {
    if (app) app->on_cleanup = callback;
}

void sg_app_set_user_data(SGApp* app, void* data) {
    if (app) app->user_data = data;
}

/* ========================================================================
 * Running
 * ======================================================================== */

void sg_app_run(SGApp* app) {
    if (!app || !app->window) return;

    skia_window_show(app->window);
    app->running = 1;
    app->time = get_time_seconds();

    /* Setup callback */
    if (app->on_setup) {
        app->on_setup(app->user_data);
    }

    double frame_budget = 1.0 / app->target_fps;

    while (app->running) {
        double frame_start = get_time_seconds();
        app->dt = frame_start - app->time;
        app->time = frame_start;

        /* 1. Process OS events (poll returns 0 while running, <0 on close) */
        if (skia_window_poll(app->window) < 0) {
            /* Window was closed */
            app->running = 0;
            break;
        }

        /* 2. Tick animations */
        if (app->animations) {
            int active = sg_animation_tick(app->animations, (float)app->dt);
            if (active > 0) {
                app->needs_repaint = 1;
            }
        }

        /* 3. User frame callback */
        if (app->on_frame) {
            app->on_frame(app->user_data, (float)app->dt);
        }

        /* 4. Layout if needed */
        if (app->root && (app->needs_layout || sg_layout_needs_update(app->root))) {
            sg_layout_compute(app->root, (float)app->width, (float)app->height);
            app->needs_layout = 0;
            app->needs_repaint = 1;

            /* Rebuild tab order after layout changes */
            if (app->events) {
                sg_focus_rebuild_tab_order(app->events);
            }
        }

        /* 5. Render if needed */
        if (app->needs_repaint || skia_window_needs_redraw(app->window)) {
            SkiaCanvas canvas = skia_window_get_canvas(app->window);
            if (canvas) {
                /* Clear background */
                skia_canvas_clear(canvas, 0xFFF5F5F5); /* Light gray */

                /* Render scene graph */
                if (app->root) {
                    sg_render(app->root, canvas);
                }

                /* Present */
                skia_window_present(app->window);
            }
            app->needs_repaint = 0;
        }

        app->frame_count++;

        /* 6. Frame rate limiting */
        double frame_elapsed = get_time_seconds() - frame_start;
        double sleep_time = frame_budget - frame_elapsed;
        if (sleep_time > 0.001) {
#ifdef _WIN32
            Sleep((DWORD)(sleep_time * 1000));
#else
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)(sleep_time * 1e9);
            nanosleep(&ts, NULL);
#endif
        }
    }
}

void sg_app_quit(SGApp* app) {
    if (app) {
        app->running = 0;
        if (app->window) skia_window_quit(app->window);
    }
}

void sg_app_invalidate(SGApp* app) {
    if (app) app->needs_repaint = 1;
}

void sg_app_relayout(SGApp* app) {
    if (app) {
        app->needs_layout = 1;
        app->needs_repaint = 1;
    }
}

/* ========================================================================
 * Accessors
 * ======================================================================== */

SkiaCanvas sg_app_get_canvas(SGApp* app) {
    if (!app || !app->window) return NULL;
    return skia_window_get_canvas(app->window);
}

FontManager* sg_app_get_fonts(SGApp* app) {
    return app ? app->fonts : NULL;
}

TextCache* sg_app_get_text_cache(SGApp* app) {
    return app ? app->text_cache : NULL;
}

SGAnimationPool* sg_app_get_animations(SGApp* app) {
    return app ? app->animations : NULL;
}

SGEventManager* sg_app_get_events(SGApp* app) {
    return app ? app->events : NULL;
}

double sg_app_get_time(SGApp* app) {
    return app ? app->time : 0;
}

float sg_app_get_dt(SGApp* app) {
    return app ? (float)app->dt : 0;
}

int sg_app_get_fps_actual(SGApp* app) {
    if (!app || app->dt <= 0) return 0;
    return (int)(1.0 / app->dt);
}
