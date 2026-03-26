/*
 * Android Runtime Implementation
 *
 * Activity stack navigation, ViewModel state, Intent data passing,
 * top-level AndroidApp, and Snackbar overlay.
 *
 * Platform: Windows desktop (using SGApp) and Android NDK (via
 *           android_ndk_runtime.c which calls android_app_run internally).
 */

#include "android_runtime.h"
#include "layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __ANDROID__
#include "android_renderer.h"
#endif

static AndroidApp* g_bound_android_app = NULL;

static void android_configure_nav_container(AndroidApp* app) {
    if (!app || !app->nav || !app->nav->container) return;

    app->nav->container->layout_type = SG_LAYOUT_COLUMN;
    app->nav->container->align_items = SG_ALIGN_STRETCH;
    app->nav->container->flex_grow   = 1.0f;
    app->nav->container->min_width   = (float)app->width;
    app->nav->container->min_height  = (float)app->height;
    sg_style_set_padding(app->nav->container,
                         (double)app->safe_inset_top,
                         (double)app->safe_inset_right,
                         (double)app->safe_inset_bottom,
                         (double)app->safe_inset_left);
}

static int android_init_nav(AndroidApp* app) {
    if (!app || !app->sg_app) return 0;
    if (app->nav) return 1;

    app->nav = android_activity_manager_create(app->sg_app);
    if (!app->nav) return 0;

    android_configure_nav_container(app);
    return 1;
}

/* ========================================================================
 * Intent
 * ======================================================================== */

AndroidIntent* android_intent_create(const char* target) {
    AndroidIntent* intent = (AndroidIntent*)calloc(1, sizeof(AndroidIntent));
    if (!intent) return NULL;
    if (target)
        snprintf(intent->target, ANDROID_MAX_STR, "%s", target);
    return intent;
}

void android_intent_put(AndroidIntent* intent, const char* key, const char* value) {
    if (!intent || !key || !value) return;
    if (intent->count >= ANDROID_MAX_INTENT_KEYS) return;

    int idx = intent->count;
    snprintf(intent->keys[idx],   sizeof(intent->keys[idx]),   "%s", key);
    snprintf(intent->values[idx], sizeof(intent->values[idx]), "%s", value);
    intent->count++;
}

const char* android_intent_get(AndroidIntent* intent, const char* key) {
    if (!intent || !key) return NULL;
    for (int i = 0; i < intent->count; i++) {
        if (strcmp(intent->keys[i], key) == 0)
            return intent->values[i];
    }
    return NULL;
}

void android_intent_destroy(AndroidIntent* intent) {
    free(intent);
}

/* ========================================================================
 * ViewModel
 * ======================================================================== */

AndroidViewModel* android_viewmodel_create(void) {
    AndroidViewModel* vm = (AndroidViewModel*)calloc(1, sizeof(AndroidViewModel));
    return vm;
}

void android_viewmodel_destroy(AndroidViewModel* vm) {
    free(vm);
}

void android_viewmodel_set(AndroidViewModel* vm, const char* key, const char* value) {
    if (!vm || !key || !value) return;

    /* Update existing key */
    for (int i = 0; i < vm->count; i++) {
        if (strcmp(vm->keys[i], key) == 0) {
            snprintf(vm->values[i], ANDROID_MAX_STR, "%s", value);
            return;
        }
    }

    /* Add new key */
    if (vm->count >= ANDROID_MAX_VM_SLOTS) return;
    int idx = vm->count++;
    snprintf(vm->keys[idx],   sizeof(vm->keys[idx]),   "%s", key);
    snprintf(vm->values[idx], ANDROID_MAX_STR, "%s", value);
}

const char* android_viewmodel_get(AndroidViewModel* vm, const char* key) {
    if (!vm || !key) return NULL;
    for (int i = 0; i < vm->count; i++) {
        if (strcmp(vm->keys[i], key) == 0)
            return vm->values[i];
    }
    return NULL;
}

void android_viewmodel_set_int(AndroidViewModel* vm, const char* key, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    android_viewmodel_set(vm, key, buf);
}

int android_viewmodel_get_int(AndroidViewModel* vm, const char* key, int default_val) {
    const char* v = android_viewmodel_get(vm, key);
    if (!v) return default_val;
    return atoi(v);
}

/* ========================================================================
 * AndroidActivity
 * ======================================================================== */

AndroidActivity* android_activity_create(const char* name) {
    AndroidActivity* act = (AndroidActivity*)calloc(1, sizeof(AndroidActivity));
    if (!act) return NULL;
    if (name)
        snprintf(act->name, ANDROID_MAX_STR, "%s", name);
    act->viewmodel = android_viewmodel_create();
    return act;
}

void android_activity_destroy(AndroidActivity* activity) {
    if (!activity) return;
    if (activity->on_destroy)
        activity->on_destroy(activity);
    if (activity->viewmodel)
        android_viewmodel_destroy(activity->viewmodel);
    /* Note: activity->root is owned by the scene graph; caller destroys SGApp */
    free(activity);
}

void android_activity_set_root(AndroidActivity* activity, SGNode* root) {
    if (!activity) return;
    activity->root = root;
}

SGNode* android_activity_get_root(AndroidActivity* activity) {
    return activity ? activity->root : NULL;
}

AndroidViewModel* android_activity_get_viewmodel(AndroidActivity* activity) {
    return activity ? activity->viewmodel : NULL;
}

const char* android_activity_get_name(AndroidActivity* activity) {
    return activity ? activity->name : NULL;
}

/* ========================================================================
 * ActivityManager — Navigation stack
 * ======================================================================== */

AndroidActivityManager* android_activity_manager_create(SGApp* app) {
    AndroidActivityManager* mgr = (AndroidActivityManager*)calloc(1, sizeof(AndroidActivityManager));
    if (!mgr) return NULL;
    mgr->app = app;
    mgr->top = -1;

    /* Create a transparent root container that holds the current screen */
    mgr->container = sg_node_create(SG_NODE_CONTAINER);
    mgr->container->flags |= SG_VISIBLE;
    sg_app_set_root(app, mgr->container);
    return mgr;
}

void android_activity_manager_destroy(AndroidActivityManager* mgr) {
    if (!mgr) return;
    /* Activities are owned by the user; we only manage the stack pointers */
    free(mgr);
}

static void _swap_screen(AndroidActivityManager* mgr, SGNode* new_root) {
    /* Remove all children from container and attach new_root */
    SGNode* child = mgr->container->first_child;
    while (child) {
        SGNode* next = child->next_sibling;
        sg_node_remove_child(mgr->container, child);
        child = next;
    }
    if (new_root) {
        new_root->flex_grow = 1.0f;
        sg_node_add_child(mgr->container, new_root);
    }
    sg_node_mark_layout_dirty(mgr->container);
    sg_node_mark_paint_dirty(mgr->container);
    sg_app_relayout(mgr->app);
    sg_app_invalidate(mgr->app);
}

void android_push_activity(AndroidActivityManager* mgr,
                            AndroidActivity* activity,
                            AndroidIntent* intent) {
    if (!mgr || !activity) return;
    if (mgr->top >= ANDROID_MAX_ACTIVITIES - 1) {
        fprintf(stderr, "[android] Activity stack overflow (max %d)\n",
                ANDROID_MAX_ACTIVITIES);
        return;
    }

    /* Pause current */
    if (mgr->top >= 0) {
        AndroidActivity* current = mgr->stack[mgr->top];
        if (current && current->on_pause)
            current->on_pause(current);
    }

    /* Push new */
    mgr->top++;
    mgr->stack[mgr->top] = activity;

    /* Lifecycle */
    if (!activity->created) {
        if (activity->on_create)
            activity->on_create(activity, intent);
        activity->created = 1;
    }
    if (activity->on_resume)
        activity->on_resume(activity);

    _swap_screen(mgr, activity->root);
}

int android_pop_activity(AndroidActivityManager* mgr) {
    if (!mgr || mgr->top < 0) return 0;

    /* Pause + destroy current */
    AndroidActivity* current = mgr->stack[mgr->top];
    if (current) {
        if (current->on_pause)   current->on_pause(current);
        if (current->on_destroy) current->on_destroy(current);
        current->created = 0;
    }
    mgr->stack[mgr->top] = NULL;
    mgr->top--;

    if (mgr->top < 0) {
        /* Stack is empty — close app */
        _swap_screen(mgr, NULL);
        sg_app_quit(mgr->app);
        return 0;
    }

    /* Resume previous */
    AndroidActivity* prev = mgr->stack[mgr->top];
    if (prev) {
        if (prev->on_resume) prev->on_resume(prev);
        _swap_screen(mgr, prev->root);
    }
    return 1;
}

AndroidActivity* android_current_activity(AndroidActivityManager* mgr) {
    if (!mgr || mgr->top < 0) return NULL;
    return mgr->stack[mgr->top];
}

void android_replace_activity(AndroidActivityManager* mgr,
                               AndroidActivity* activity,
                               AndroidIntent* intent) {
    if (!mgr || !activity) return;
    /* Pop current without resuming previous, then push new */
    if (mgr->top >= 0) {
        AndroidActivity* current = mgr->stack[mgr->top];
        if (current) {
            if (current->on_pause)   current->on_pause(current);
            if (current->on_destroy) current->on_destroy(current);
            current->created = 0;
        }
        mgr->stack[mgr->top] = NULL;
        mgr->top--;
    }
    android_push_activity(mgr, activity, intent);
}

/* ========================================================================
 * AndroidApp
 * ======================================================================== */

AndroidApp* android_app_create(const char* app_name, int width, int height) {
#ifdef __ANDROID__
    if (g_bound_android_app && g_bound_android_app->native_mode) {
        AndroidApp* app = g_bound_android_app;
        if (app_name) {
            snprintf(app->app_name, ANDROID_MAX_STR, "%s", app_name);
        }
        app->width = width;
        app->height = height;
        if (app->sg_app) {
            app->sg_app->width = width;
            app->sg_app->height = height;
            app->sg_app->needs_layout = 1;
            app->sg_app->needs_repaint = 1;
        }
        android_init_nav(app);
        android_configure_nav_container(app);
        return app;
    }
#endif

    AndroidApp* app = (AndroidApp*)calloc(1, sizeof(AndroidApp));
    if (!app) return NULL;

    if (app_name)
        snprintf(app->app_name, ANDROID_MAX_STR, "%s", app_name);
    app->width  = width;
    app->height = height;

    app->sg_app = sg_app_create(app_name ? app_name : "Android App", width, height);
    if (!app->sg_app) {
        free(app);
        return NULL;
    }

    if (!android_init_nav(app)) {
        sg_app_destroy(app->sg_app);
        free(app);
        return NULL;
    }

    return app;
}

void android_app_destroy(AndroidApp* app) {
    if (!app) return;
#ifdef __ANDROID__
    if (app->native_renderer) {
        android_renderer_destroy((AndroidRenderer*)app->native_renderer);
        app->native_renderer = NULL;
    }
    if (g_bound_android_app == app) {
        g_bound_android_app = NULL;
    }
#endif
    android_activity_manager_destroy(app->nav);
    sg_app_destroy(app->sg_app);
    free(app);
}

void android_app_set_main_activity(AndroidApp* app, AndroidActivity* activity) {
    if (!app || !activity) return;
    android_push_activity(app->nav, activity, NULL);
}

void android_app_run(AndroidApp* app) {
    if (!app) return;
    sg_app_run(app->sg_app);
}

void android_app_quit(AndroidApp* app) {
    if (!app) return;
    sg_app_quit(app->sg_app);
}

SGApp* android_app_get_sg_app(AndroidApp* app) {
    return app ? app->sg_app : NULL;
}

AndroidActivityManager* android_app_get_nav(AndroidApp* app) {
    return app ? app->nav : NULL;
}

void android_app_bind_current(AndroidApp* app) {
    g_bound_android_app = app;
    if (app) {
        app->native_mode = 1;
    }
}

void android_app_bind_renderer(AndroidApp* app, void* renderer) {
    if (!app) return;
    app->native_renderer = renderer;
}

void android_app_set_safe_insets(AndroidApp* app,
                                 int left,
                                 int top,
                                 int right,
                                 int bottom) {
    if (!app) return;

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right < 0) right = 0;
    if (bottom < 0) bottom = 0;

    app->safe_inset_left = left;
    app->safe_inset_top = top;
    app->safe_inset_right = right;
    app->safe_inset_bottom = bottom;

    android_configure_nav_container(app);
    if (app->sg_app) {
        sg_app_relayout(app->sg_app);
        sg_app_invalidate(app->sg_app);
    }
}

int android_app_get_safe_inset_top(AndroidApp* app) {
    return app ? app->safe_inset_top : 0;
}

int android_app_get_safe_inset_right(AndroidApp* app) {
    return app ? app->safe_inset_right : 0;
}

int android_app_get_safe_inset_bottom(AndroidApp* app) {
    return app ? app->safe_inset_bottom : 0;
}

int android_app_get_safe_inset_left(AndroidApp* app) {
    return app ? app->safe_inset_left : 0;
}

#ifdef __ANDROID__
AndroidApp* android_app_create_native(AAssetManager* assets, int width, int height) {
    AndroidApp* app = (AndroidApp*)calloc(1, sizeof(AndroidApp));
    if (!app) return NULL;

    snprintf(app->app_name, ANDROID_MAX_STR, "%s", "Casprix Android");
    app->width = width;
    app->height = height;
    app->native_mode = 1;
    app->native_assets = assets;

    app->sg_app = sg_app_create(app->app_name, width, height);
    if (!app->sg_app) {
        free(app);
        return NULL;
    }

    if (!android_init_nav(app)) {
        sg_app_destroy(app->sg_app);
        free(app);
        return NULL;
    }

    g_bound_android_app = app;
    return app;
}
#endif

void android_app_resize(AndroidApp* app, int width, int height) {
    if (!app || !app->sg_app) return;

    app->width = width;
    app->height = height;
    app->sg_app->width = width;
    app->sg_app->height = height;
    app->sg_app->needs_layout = 1;
    app->sg_app->needs_repaint = 1;
    android_configure_nav_container(app);

#ifdef __ANDROID__
    if (app->native_renderer) {
        android_renderer_resize((AndroidRenderer*)app->native_renderer, width, height);
    }
#endif

    if (app->sg_app->events) {
        sg_dispatch_resize(app->sg_app->events, width, height);
    }
}

void android_app_tick(AndroidApp* app, double current_time_seconds) {
    if (!app || !app->sg_app) return;

    SGApp* sg = app->sg_app;
    if (!sg->running) {
        sg_app_run(sg);
    }

    sg->dt = current_time_seconds - sg->time;
    if (sg->dt < 0.0) sg->dt = 0.0;
    sg->time = current_time_seconds;

    if (sg->animations) {
        int active = sg_animation_tick(sg->animations, (float)sg->dt);
        if (active > 0) {
            sg->needs_repaint = 1;
        }
    }

    if (sg->on_frame) {
        sg->on_frame(sg->user_data, (float)sg->dt);
    }

    if (sg->root && (sg->needs_layout || sg_layout_needs_update(sg->root))) {
        sg_layout_compute(sg->root, (float)sg->width, (float)sg->height);
        sg->needs_layout = 0;
        sg->needs_repaint = 1;
        if (sg->events) {
            sg_focus_rebuild_tab_order(sg->events);
        }
    }
}

void* android_app_begin_frame(AndroidApp* app) {
    if (!app) return NULL;
#ifdef __ANDROID__
    if (app->native_renderer) {
        return android_renderer_begin_frame((AndroidRenderer*)app->native_renderer);
    }
#endif
    return (void*)sg_app_get_canvas(app->sg_app);
}

void android_app_render(AndroidApp* app, void* canvas) {
    if (!app || !app->sg_app || !canvas) return;
    if (app->sg_app->root) {
        sg_render(app->sg_app->root, (SkiaCanvas)canvas);
    }
    app->sg_app->needs_repaint = 0;
}

void android_app_end_frame(AndroidApp* app) {
    if (!app) return;
#ifdef __ANDROID__
    if (app->native_renderer) {
        android_renderer_end_frame((AndroidRenderer*)app->native_renderer);
    }
#endif
}

void android_app_dispatch_event(AndroidApp* app, SGEvent* event) {
    if (!app || !app->sg_app || !app->sg_app->events || !event) return;

    switch (event->type) {
        case SG_EVENT_MOUSE_DOWN:
            sg_dispatch_mouse_down(app->sg_app->events,
                                   event->data.mouse.x,
                                   event->data.mouse.y,
                                   event->data.mouse.button,
                                   event->mods);
            break;
        case SG_EVENT_MOUSE_UP:
            sg_dispatch_mouse_up(app->sg_app->events,
                                 event->data.mouse.x,
                                 event->data.mouse.y,
                                 event->data.mouse.button,
                                 event->mods);
            break;
        case SG_EVENT_MOUSE_MOVE:
            sg_dispatch_mouse_move(app->sg_app->events,
                                   event->data.mouse.x,
                                   event->data.mouse.y,
                                   event->mods);
            break;
        case SG_EVENT_MOUSE_SCROLL:
            sg_dispatch_mouse_scroll(app->sg_app->events,
                                     event->data.scroll.x,
                                     event->data.scroll.y,
                                     event->data.scroll.dx,
                                     event->data.scroll.dy,
                                     event->mods);
            break;
        case SG_EVENT_KEY_DOWN:
            sg_dispatch_key_down(app->sg_app->events,
                                 event->data.key.keycode,
                                 event->data.key.scancode,
                                 event->mods);
            break;
        case SG_EVENT_KEY_UP:
            sg_dispatch_key_up(app->sg_app->events,
                               event->data.key.keycode,
                               event->data.key.scancode,
                               event->mods);
            break;
        case SG_EVENT_TEXT_INPUT:
            sg_dispatch_text_input(app->sg_app->events,
                                   event->data.text_input.text);
            break;
        case SG_EVENT_RESIZE:
            sg_dispatch_resize(app->sg_app->events,
                               event->data.resize.width,
                               event->data.resize.height);
            break;
        default:
            sg_dispatch_event(app->sg_app->events, event);
            break;
    }

    sg_app_invalidate(app->sg_app);
}

void android_app_lifecycle(AndroidApp* app, AndroidLifecycleEvent event) {
    if (!app || !app->sg_app) return;

    switch (event) {
        case ANDROID_LIFECYCLE_RESUME:
            app->sg_app->running = 1;
            break;
        case ANDROID_LIFECYCLE_PAUSE:
        case ANDROID_LIFECYCLE_STOP:
            break;
        case ANDROID_LIFECYCLE_DESTROY:
            sg_app_quit(app->sg_app);
            break;
    }
}

/* ========================================================================
 * Ripple Effect
 * ======================================================================== */

void android_ripple_animate(SGAnimationPool* pool, SGNode* node, double duration) {
    if (!pool || !node) return;
    float half = duration * 0.5f;

    /* Scale punch: 1.0 → 0.95 → 1.0 */
    SGAnimation* a1 = sg_animate(pool, node, SG_ANIM_SCALE_X,
                                  1.0f, 0.95f, half, SG_EASE_OUT_QUAD);
    sg_animate_delay(a1, 0.0f);

    SGAnimation* a2 = sg_animate(pool, node, SG_ANIM_SCALE_Y,
                                  1.0f, 0.95f, half, SG_EASE_OUT_QUAD);
    sg_animate_delay(a2, 0.0f);

    /* Recover */
    SGAnimation* a3 = sg_animate(pool, node, SG_ANIM_SCALE_X,
                                  0.95f, 1.0f, half, SG_EASE_OUT_BACK);
    sg_animate_delay(a3, half);

    SGAnimation* a4 = sg_animate(pool, node, SG_ANIM_SCALE_Y,
                                  0.95f, 1.0f, half, SG_EASE_OUT_BACK);
    sg_animate_delay(a4, half);

    (void)a3; (void)a4;
}

/* ========================================================================
 * Snackbar
 * ======================================================================== */

AndroidSnackbar* android_snackbar_create(SGNode* parent_root) {
    AndroidSnackbar* sb = (AndroidSnackbar*)calloc(1, sizeof(AndroidSnackbar));
    if (!sb) return NULL;

    /* Build: a fixed-height row at the bottom */
    sb->root = sg_node_create(SG_NODE_CONTAINER);
    sb->root->layout_type = SG_LAYOUT_ROW;
    sb->root->justify     = SG_JUSTIFY_CENTER;
    sb->root->align_items = SG_ALIGN_CENTER;
    sb->root->flags      &= ~SG_VISIBLE;    /* hidden by default */
    sb->root->style.background   = 0xFF323232;
    sb->root->style.border_radius = 4.0f;
    sb->root->style.padding[0]   = 10.0f;
    sb->root->style.padding[1]   = 16.0f;
    sb->root->style.padding[2]   = 10.0f;
    sb->root->style.padding[3]   = 16.0f;
    sb->root->style.elevation    = 4;
    sb->root->min_width          = 200.0f;
    sb->root->max_width          = 400.0f;

    /* Text node */
    SkiaFont font = skia_font_create("Segoe UI", 13.0f);
    sb->text_node = sg_node_create(SG_NODE_TEXT);
    sb->text_node->data.text.text  = strdup("Snackbar");
    sb->text_node->data.text.font  = font;
    sb->text_node->data.text.color = 0xFFFFFFFF;
    sb->text_node->flags          |= SG_VISIBLE;

    sg_node_add_child(sb->root, sb->text_node);

    if (parent_root)
        sg_node_add_child(parent_root, sb->root);

    sb->visible = 0;
    return sb;
}

void android_snackbar_show(AndroidSnackbar* sb, const char* message,
                            double duration_seconds, void* app_time_ptr) {
    if (!sb || !message) return;

    /* Update text */
    if (sb->text_node->data.text.text)
        free(sb->text_node->data.text.text);
    sb->text_node->data.text.text = strdup(message);

    sg_node_set_visible(sb->root, 1);
    sg_node_mark_paint_dirty(sb->root);
    sb->visible = 1;

    if (app_time_ptr) {
        double* t = (double*)app_time_ptr;
        sb->dismiss_time = *t + duration_seconds;
    } else {
        sb->dismiss_time = 0.0;
    }
}

void android_snackbar_hide(AndroidSnackbar* sb) {
    if (!sb) return;
    sg_node_set_visible(sb->root, 0);
    sb->visible = 0;
}

void android_snackbar_tick(AndroidSnackbar* sb, double current_time) {
    if (!sb || !sb->visible) return;
    if (sb->dismiss_time > 0.0 && current_time >= sb->dismiss_time)
        android_snackbar_hide(sb);
}
