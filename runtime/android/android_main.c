/*
 * android_main.c — NativeActivity entry point for ND/Casprix apps
 *
 * This replaces Java entirely. Android calls android_main() directly.
 * No Android Views, no XML layouts.  Skia draws everything.
 *
 * Flutter does exactly this:
 *   android_main() → EGL init → Skia GrContext → render loop
 *
 * The user's .cpx app only needs to define:
 *   func app_main(app: AndroidApp) { ... }
 *
 * Compile to:  libMainActivity.so  (exported symbol: ANativeActivity_onCreate)
 * AndroidManifest.xml:
 *   <activity android:name="android.app.NativeActivity">
 *     <meta-data android:name="android.app.lib_name" android:value="MainActivity"/>
 *   </activity>
 */

#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/log.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "android_egl.h"
#include "android_renderer.h"
#include "android_runtime.h"
#include "scene_graph.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "CasprixApp", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "CasprixApp", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CasprixApp", __VA_ARGS__)

/* ============================================================================
 * App state shared between main thread and event callbacks
 * ========================================================================== */

typedef struct {
    ANativeActivity*  activity;
    ANativeWindow*    window;
    AInputQueue*      input_queue;

    AndroidEGL*       egl;
    AndroidApp*       casprix_app;    /* ND runtime app handle */

    int               running;
    int               visible;
    int               surface_ready;
    int               has_content_rect;
    int               content_left;
    int               content_top;
    int               content_right;
    int               content_bottom;

    /* Touch state passed into scene graph event system */
    float last_touch_x;
    float last_touch_y;

    pthread_t  render_thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} AppState;

static void apply_content_rect_locked(AppState* state) {
    if (!state || !state->casprix_app || !state->has_content_rect) return;

    int width = 0;
    int height = 0;
    if (state->egl) {
        android_egl_get_size(state->egl, &width, &height);
    } else if (state->window) {
        width = ANativeWindow_getWidth(state->window);
        height = ANativeWindow_getHeight(state->window);
    }
    if (width <= 0 || height <= 0) return;

    int left = state->content_left;
    int top = state->content_top;
    int right = width - state->content_right;
    int bottom = height - state->content_bottom;

    if (right < 0) right = 0;
    if (bottom < 0) bottom = 0;

    android_app_set_safe_insets(state->casprix_app, left, top, right, bottom);
    LOGI("Applied content rect safe insets: l=%d t=%d r=%d b=%d (window %dx%d)",
         left, top, right, bottom, width, height);
}

/* ============================================================================
 * Time helper — milliseconds since epoch
 * ========================================================================== */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ============================================================================
 * Forward declaration — user app entry point (generated from .cpx)
 * ============================================================================
 * The Casprix compiler generates:
 *   void cpx_app_main(AndroidApp* app);
 * from the top-level code in the user's .cpx file.
 */
extern void cpx_app_main(AndroidApp* app);

/* ============================================================================
 * Render loop (runs on a dedicated thread so the main thread stays free
 * for Android lifecycle callbacks — same pattern as Flutter's raster thread)
 * ========================================================================== */

static void* render_thread_func(void* arg) {
    AppState* state = (AppState*)arg;

    /* Wait until a window surface has been created */
    pthread_mutex_lock(&state->mutex);
    while (!state->window) {
        pthread_cond_wait(&state->cond, &state->mutex);
    }
    ANativeWindow* window = state->window;
    pthread_mutex_unlock(&state->mutex);

    /* Initialise EGL on this thread (EGL contexts are per-thread) */
    state->egl = android_egl_create(window);
    if (!state->egl) {
        LOGE("EGL create failed!");
        return NULL;
    }
    android_egl_make_current(state->egl);

    /* Create the Casprix Android runtime app */
    int width, height;
    android_egl_get_size(state->egl, &width, &height);

    state->casprix_app = android_app_create_native(
        state->activity->assetManager,
        width, height
    );
    if (!state->casprix_app) {
        LOGE("android_app_create_native failed");
        android_egl_destroy(state->egl);
        return NULL;
    }

    state->casprix_app->native_renderer = android_renderer_create(state->egl);
    if (!state->casprix_app->native_renderer) {
        LOGE("android_renderer_create failed");
        android_app_destroy(state->casprix_app);
        android_egl_destroy(state->egl);
        return NULL;
    }
    android_app_bind_renderer(state->casprix_app, state->casprix_app->native_renderer);

    pthread_mutex_lock(&state->mutex);
    apply_content_rect_locked(state);
    pthread_mutex_unlock(&state->mutex);

    /* Hand off to user .cpx app main */
    cpx_app_main(state->casprix_app);

    /* ── Main render / event loop ── */
    double frame_budget = 1000.0 / 60.0;   /* 60 fps */

    while (state->running) {
        double t0 = now_ms();

        pthread_mutex_lock(&state->mutex);

        /* Handle window surface changes */
        if (!state->surface_ready && state->window) {
            android_egl_surface_changed(state->egl, state->window);
            int w, h;
            android_egl_get_size(state->egl, &w, &h);
            android_app_resize(state->casprix_app, w, h);
            apply_content_rect_locked(state);
            state->surface_ready = 1;
        }
        if (!state->visible || !state->surface_ready) {
            pthread_mutex_unlock(&state->mutex);
            usleep(16000);
            continue;
        }

        /* Drain input events */
        AInputEvent* event = NULL;
        AInputQueue* iq = state->input_queue;
        if (iq) {
            while (AInputQueue_getEvent(iq, &event) >= 0) {
                if (AInputQueue_preDispatchEvent(iq, event)) continue;

                int handled = 0;
                if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
                    int action = AMotionEvent_getAction(event)
                                 & AMOTION_EVENT_ACTION_MASK;
                    float x = AMotionEvent_getX(event, 0);
                    float y = AMotionEvent_getY(event, 0);

                    SGEventType sg_type = 0;
                    if      (action == AMOTION_EVENT_ACTION_DOWN)  sg_type = SG_EVENT_MOUSE_DOWN;
                    else if (action == AMOTION_EVENT_ACTION_UP)    sg_type = SG_EVENT_MOUSE_UP;
                    else if (action == AMOTION_EVENT_ACTION_MOVE)  sg_type = SG_EVENT_MOUSE_MOVE;

                    if (sg_type != 0) {
                        SGEvent sg_event = {0};
                        sg_event.type = sg_type;
                        sg_event.data.mouse.x = x;
                        sg_event.data.mouse.y = y;
                        sg_event.data.mouse.button = SG_BUTTON_LEFT;
                        android_app_dispatch_event(state->casprix_app, &sg_event);
                        handled = 1;
                    }
                } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
                    int action  = AKeyEvent_getAction(event);
                    int keycode = AKeyEvent_getKeyCode(event);
                    /* Pass key events to scene graph text inputs */
                    SGEvent sg_event = {0};
                    sg_event.type = (action == AKEY_EVENT_ACTION_DOWN)
                                    ? SG_EVENT_KEY_DOWN : SG_EVENT_KEY_UP;
                    sg_event.data.key.keycode = keycode;
                    android_app_dispatch_event(state->casprix_app, &sg_event);
                    handled = 1;
                }

                AInputQueue_finishEvent(iq, event, handled);
            }
        }

        pthread_mutex_unlock(&state->mutex);

        /* Update animations / snackbar timers */
        android_app_tick(state->casprix_app, now_ms() / 1000.0);

        /* ── Render frame ── */
        /* android_renderer_begin_frame returns SkCanvas* (cast as void*) */
        void* canvas = android_app_begin_frame(state->casprix_app);
        if (canvas) {
            android_app_render(state->casprix_app, canvas);
            android_app_end_frame(state->casprix_app);
        }

        /* Cap to 60 fps */
        double elapsed = now_ms() - t0;
        if (elapsed < frame_budget) {
            usleep((useconds_t)((frame_budget - elapsed) * 1000.0));
        }
    }

    android_app_destroy(state->casprix_app);
    android_egl_destroy(state->egl);
    return NULL;
}

/* ============================================================================
 * ANativeActivity callbacks (called by Android on the main thread)
 * ========================================================================== */

static void on_window_created(ANativeActivity* act, ANativeWindow* window) {
    AppState* state = (AppState*)act->instance;
    LOGI("onNativeWindowCreated");
    pthread_mutex_lock(&state->mutex);
    state->window        = window;
    state->surface_ready = 0;
    pthread_cond_broadcast(&state->cond);
    pthread_mutex_unlock(&state->mutex);
}

static void on_window_destroyed(ANativeActivity* act, ANativeWindow* window) {
    AppState* state = (AppState*)act->instance;
    LOGI("onNativeWindowDestroyed");
    pthread_mutex_lock(&state->mutex);
    state->window        = NULL;
    state->surface_ready = 0;
    pthread_mutex_unlock(&state->mutex);
}

static void on_window_resized(ANativeActivity* act, ANativeWindow* window) {
    AppState* state = (AppState*)act->instance;
    pthread_mutex_lock(&state->mutex);
    state->surface_ready = 0;   /* force EGL resize on next frame */
    pthread_mutex_unlock(&state->mutex);
}

static void on_resume(ANativeActivity* act) {
    AppState* state = (AppState*)act->instance;
    pthread_mutex_lock(&state->mutex);
    state->visible = 1;
    if (state->casprix_app)
        android_app_lifecycle(state->casprix_app, ANDROID_LIFECYCLE_RESUME);
    pthread_mutex_unlock(&state->mutex);
}

static void on_pause(ANativeActivity* act) {
    AppState* state = (AppState*)act->instance;
    pthread_mutex_lock(&state->mutex);
    state->visible = 0;
    if (state->casprix_app)
        android_app_lifecycle(state->casprix_app, ANDROID_LIFECYCLE_PAUSE);
    pthread_mutex_unlock(&state->mutex);
}

static void on_stop(ANativeActivity* act) {
    AppState* state = (AppState*)act->instance;
    if (state->casprix_app)
        android_app_lifecycle(state->casprix_app, ANDROID_LIFECYCLE_STOP);
}

static void on_destroy(ANativeActivity* act) {
    AppState* state = (AppState*)act->instance;
    LOGI("onDestroy");
    state->running = 0;
    if (state->casprix_app) {
        android_app_lifecycle(state->casprix_app, ANDROID_LIFECYCLE_DESTROY);
    }
    pthread_join(state->render_thread, NULL);
    pthread_mutex_destroy(&state->mutex);
    pthread_cond_destroy(&state->cond);
    free(state);
}

static void on_input_queue_created(ANativeActivity* act, AInputQueue* queue) {
    AppState* state = (AppState*)act->instance;
    pthread_mutex_lock(&state->mutex);
    state->input_queue = queue;
    pthread_mutex_unlock(&state->mutex);
}

static void on_input_queue_destroyed(ANativeActivity* act, AInputQueue* queue) {
    AppState* state = (AppState*)act->instance;
    pthread_mutex_lock(&state->mutex);
    state->input_queue = NULL;
    pthread_mutex_unlock(&state->mutex);
}

static void on_content_rect_changed(ANativeActivity* act, const ARect* rect) {
    AppState* state = (AppState*)act->instance;
    if (!state || !rect) return;

    pthread_mutex_lock(&state->mutex);
    state->content_left = rect->left;
    state->content_top = rect->top;
    state->content_right = rect->right;
    state->content_bottom = rect->bottom;
    state->has_content_rect = 1;

    apply_content_rect_locked(state);

    LOGI("onContentRectChanged: left=%d top=%d right=%d bottom=%d",
         rect->left, rect->top, rect->right, rect->bottom);
    pthread_mutex_unlock(&state->mutex);
}

/* ============================================================================
 * ANativeActivity_onCreate — called by Android when the library loads
 * This is the real entry point (equivalent to Activity.onCreate in Java).
 * ========================================================================== */

__attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity* activity,
                              void* saved_state, size_t saved_state_size) {
    LOGI("ANativeActivity_onCreate — Casprix runtime");

    AppState* state = (AppState*)calloc(1, sizeof(AppState));
    state->activity = activity;
    state->running  = 1;
    state->visible  = 0;

    pthread_mutex_init(&state->mutex, NULL);
    pthread_cond_init(&state->cond, NULL);

    /* Register lifecycle callbacks */
    activity->callbacks->onNativeWindowCreated   = on_window_created;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onNativeWindowResized   = on_window_resized;
    activity->callbacks->onResume                = on_resume;
    activity->callbacks->onPause                 = on_pause;
    activity->callbacks->onStop                  = on_stop;
    activity->callbacks->onDestroy               = on_destroy;
    activity->callbacks->onInputQueueCreated     = on_input_queue_created;
    activity->callbacks->onInputQueueDestroyed   = on_input_queue_destroyed;
    activity->callbacks->onContentRectChanged    = on_content_rect_changed;

    activity->instance = state;

    /* Spawn the render thread */
    pthread_create(&state->render_thread, NULL, render_thread_func, state);
}
