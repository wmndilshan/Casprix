#include "frame_loop.h"
#include "layout.h"
#include <stdlib.h>

SGApp* sg_app_create(const char* title, int width, int height) {
    (void)title;

    SGApp* app = (SGApp*)calloc(1, sizeof(SGApp));
    if (!app) return NULL;

    app->width = width;
    app->height = height;
    app->target_fps = 60;
    app->needs_layout = 1;
    app->needs_repaint = 1;
    app->animations = sg_animation_pool_create();
    app->fonts = font_manager_create();
    app->text_cache = text_cache_create();
    return app;
}

void sg_app_destroy(SGApp* app) {
    if (!app) return;

    if (app->on_cleanup) {
        app->on_cleanup(app->user_data);
    }

    if (app->root) sg_node_destroy(app->root);
    if (app->events) sg_event_manager_destroy(app->events);
    if (app->animations) sg_animation_pool_destroy(app->animations);
    if (app->fonts) font_manager_destroy(app->fonts);
    if (app->text_cache) text_cache_destroy(app->text_cache);
    free(app);
}

void sg_app_set_root(SGApp* app, SGNode* root) {
    if (!app) return;

    if (app->events) {
        sg_event_manager_destroy(app->events);
    }

    app->root = root;
    app->events = root ? sg_event_manager_create(root) : NULL;
    if (app->events) {
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

void sg_app_run(SGApp* app) {
    if (!app) return;

    if (!app->running && app->on_setup) {
        app->on_setup(app->user_data);
    }
    app->running = 1;
}

void sg_app_quit(SGApp* app) {
    if (app) app->running = 0;
}

void sg_app_invalidate(SGApp* app) {
    if (app) app->needs_repaint = 1;
}

void sg_app_relayout(SGApp* app) {
    if (app) app->needs_layout = 1;
}

SkiaCanvas sg_app_get_canvas(SGApp* app) {
    (void)app;
    return NULL;
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
    return app ? app->time : 0.0;
}

float sg_app_get_dt(SGApp* app) {
    return app ? (float)app->dt : 0.0f;
}

int sg_app_get_fps_actual(SGApp* app) {
    if (!app || app->dt <= 0.0) return 0;
    return (int)(1.0 / app->dt);
}
