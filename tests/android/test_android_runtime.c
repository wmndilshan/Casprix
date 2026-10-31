#include "android_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

typedef struct {
    int creates;
    int resumes;
    int pauses;
    int destroys;
} LifecycleCounts;

static void on_create(AndroidActivity* activity, AndroidIntent* intent) {
    LifecycleCounts* counts = (LifecycleCounts*)activity->user_data;
    (void)intent;
    counts->creates++;
}

static void on_resume(AndroidActivity* activity) {
    ((LifecycleCounts*)activity->user_data)->resumes++;
}

static void on_pause(AndroidActivity* activity) {
    ((LifecycleCounts*)activity->user_data)->pauses++;
}

static void on_destroy(AndroidActivity* activity) {
    ((LifecycleCounts*)activity->user_data)->destroys++;
}

/* Lightweight host frame-loop implementation for Android architecture tests. */
SGApp* sg_app_create(const char* title, int width, int height) {
    SGApp* app = (SGApp*)calloc(1, sizeof(SGApp));
    (void)title;
    if (app) {
        app->width = width;
        app->height = height;
        app->needs_layout = 1;
        app->needs_repaint = 1;
    }
    return app;
}

void sg_app_destroy(SGApp* app) {
    if (!app) return;
    if (app->root) sg_node_destroy_tree(app->root);
    free(app);
}

void sg_app_set_root(SGApp* app, SGNode* root) { if (app) app->root = root; }
void sg_app_run(SGApp* app) { if (app) app->running = 1; }
void sg_app_quit(SGApp* app) { if (app) app->running = 0; }
void sg_app_invalidate(SGApp* app) { if (app) app->needs_repaint = 1; }
void sg_app_relayout(SGApp* app) { if (app) app->needs_layout = 1; }
SkiaCanvas sg_app_get_canvas(SGApp* app) { (void)app; return NULL; }
SGAnimationPool* sg_app_get_animations(SGApp* app) { return app ? app->animations : NULL; }

static AndroidActivity* make_activity(const char* name, SGNode* root,
                                      LifecycleCounts* counts) {
    AndroidActivity* activity = android_activity_create(name);
    activity->root = root;
    activity->user_data = counts;
    activity->on_create = on_create;
    activity->on_resume = on_resume;
    activity->on_pause = on_pause;
    activity->on_destroy = on_destroy;
    return activity;
}

int main(void) {
    AndroidIntent* intent = android_intent_create("Details");
    android_intent_put(intent, "id", "41");
    android_intent_put(intent, "id", "42");
    CHECK(intent->count == 1, "Intent updates an existing key instead of duplicating it");
    CHECK(strcmp(android_intent_get(intent, "id"), "42") == 0,
          "Intent returns the latest value");

    AndroidViewModel* vm = android_viewmodel_create();
    android_viewmodel_set(vm, "valid", "123");
    android_viewmodel_set(vm, "invalid", "12x");
    CHECK(android_viewmodel_get_int(vm, "valid", -1) == 123,
          "ViewModel parses valid integers");
    CHECK(android_viewmodel_get_int(vm, "invalid", -1) == -1,
          "ViewModel preserves the default for malformed integers");

    AndroidApp* app = android_app_create("runtime-test", 720, 1600);
    LifecycleCounts home_counts = {0};
    LifecycleCounts details_counts = {0};
    SGNode* home_root = sg_node_create(SG_NODE_CONTAINER);
    SGNode* details_root = sg_node_create(SG_NODE_CONTAINER);
    AndroidActivity* home = make_activity("Home", home_root, &home_counts);
    AndroidActivity* details = make_activity("Details", details_root, &details_counts);

    android_app_set_main_activity(app, home);
    CHECK(home_counts.creates == 1 && home_counts.resumes == 1,
          "Main activity receives create then resume");
    CHECK(android_activity_stack_depth(app->nav) == 1, "Main activity establishes stack depth 1");
    CHECK(!android_can_pop_activity(app->nav), "Root activity cannot be popped by Back");

    android_app_lifecycle(app, ANDROID_LIFECYCLE_PAUSE);
    android_app_lifecycle(app, ANDROID_LIFECYCLE_PAUSE);
    CHECK(home_counts.pauses == 1, "Duplicate system pause is idempotent");
    android_app_lifecycle(app, ANDROID_LIFECYCLE_RESUME);
    CHECK(home_counts.resumes == 2, "System resume reaches the current activity");

    android_push_activity(app->nav, details, intent);
    CHECK(home_counts.pauses == 2, "Pushing a screen pauses the previous screen");
    CHECK(details_counts.creates == 1 && details_counts.resumes == 1,
          "Pushed activity receives create then resume");
    CHECK(android_activity_stack_depth(app->nav) == 2 && android_can_pop_activity(app->nav),
          "Navigation stack reports a Back destination");

    CHECK(android_handle_back(app->nav) == 1, "Back consumes a non-root destination");
    CHECK(details_counts.pauses == 1 && details_counts.destroys == 1,
          "Back pauses and destroys the removed activity exactly once");
    CHECK(home_counts.resumes == 3, "Back resumes the previous activity");
    CHECK(android_handle_back(app->nav) == 0, "Back delegates to Android at the root");

    android_push_activity(app->nav, details, intent);
    CHECK(details_counts.creates == 2 && details_counts.resumes == 2,
          "A previously removed activity can be recreated and resumed");

    android_app_set_safe_insets(app, 2, 10, 4, 20);
    CHECK(android_app_get_safe_inset_left(app) == 2 &&
          android_app_get_safe_inset_top(app) == 10 &&
          android_app_get_safe_inset_right(app) == 4 &&
          android_app_get_safe_inset_bottom(app) == 20,
          "Safe-area insets round-trip through the app runtime");

    android_app_lifecycle(app, ANDROID_LIFECYCLE_DESTROY);
    CHECK(details_counts.pauses == 2 && details_counts.destroys == 2,
          "App destruction finalizes the active activity once");
    CHECK(home_counts.destroys == 1,
          "App destruction also finalizes paused activities in the back stack");

    android_app_destroy(app);
    android_activity_destroy(home);
    android_activity_destroy(details);
    sg_node_destroy_tree(details_root);
    android_viewmodel_destroy(vm);
    android_intent_destroy(intent);

    if (failures) {
        fprintf(stderr, "%d Android runtime test(s) failed\n", failures);
        return 1;
    }
    puts("Android runtime navigation/lifecycle tests passed");
    return 0;
}
