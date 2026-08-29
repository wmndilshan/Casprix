/*
 * test_a11y_bridge.c — v1b verification: real SGNode-scene-graph traversal
 * through the accessibility bridge.
 *
 * Builds a scene graph directly (the SGNode-direct pattern used by the
 * error-resilience hardening tests), registers it with the bridge, and checks
 * count / parent-child / bounds / labels / roles / flags / ACTION_CLICK /
 * hidden-node re-parenting / structural-change firing.
 *
 * Linked with the real scene_graph.o + widgets.o + layout.o + events.o and a
 * tiny stub of the skia_* / text_layout_compute surface (never called on the
 * paths exercised here). No Android, no JNI — the JNI wrappers just forward to
 * the cpx_a11y_* functions this test drives directly.
 */
#include "a11y_bridge.h"
#include "scene_graph.h"
#include "widgets.h"
#include "events.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- assertions --------------------------------------------------------- */
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } \
} while (0)

/* --- structural-change spy ------------------------------------------------ */
static int g_change_calls = 0;
static void on_change(void* ud) { (void)ud; g_change_calls++; }

/* --- a click handler we can observe ------------------------------------- */
static int g_click_hits = 0;
static void on_click(SGNode* n, void* ud) {
    (void)n; (void)ud;
    g_click_hits++;
}

/* --- downstream content-changed consumer (stands in for the JNI up-call) --- */
static int g_consumer_hits = 0;
static void a11y_test_consumer(void) { g_consumer_hits++; }

/* Give a subtree hand-placed bounds (the real layout engine needs a full
 * skia backend; the bridge only reads node->bounds). */
static void set_bounds(SGNode* n, float x, float y, float w, float h) {
    n->bounds.x = x; n->bounds.y = y; n->bounds.w = w; n->bounds.h = h;
}

/* Locate the surfaced node with a given label (helper for id lookup). */
static int32_t id_with_label(const char* want) {
    int32_t count = cpx_a11y_node_count();
    for (int32_t i = 0; i < count; i++) {
        int32_t id = cpx_a11y_node_id_at(i);
        if (id >= 0 && strcmp(cpx_a11y_node_label(id), want) == 0) return id;
    }
    return -1;
}

int main(void) {
    /* ---------------------------------------------------------------- *
     * Scene:  root(container)
     *           title            SG_NODE_TEXT "Settings"      -> HEADER
     *           form (container, a11y_hidden = 1)   [decorative wrapper]
     *             btnSave        button "Save"      (enabled)
     *             btnDelete      button "Delete"    (DISABLED)
     *             btnIcon        button, explicit a11y_label "Add item"
     *             email          text input (placeholder "Email")
     * ---------------------------------------------------------------- */
    sg_a11y_set_structural_change_cb(on_change, NULL);

    SGNode* root = sg_node_create(SG_NODE_CONTAINER);
    set_bounds(root, 0, 0, 400, 800);

    SGNode* title = widget_text("Settings", NULL, 0xFF000000);
    sg_node_set_a11y_role(title, SG_A11Y_ROLE_HEADER);
    set_bounds(title, 16, 24, 200, 32);
    sg_node_add_child(root, title);

    SGNode* form = sg_node_create(SG_NODE_CONTAINER);
    form->a11y_hidden = 1;                     /* decorative-only wrapper */
    set_bounds(form, 16, 80, 368, 400);
    sg_node_add_child(root, form);

    SGNode* btnSave = widget_button("Save", NULL, on_click, NULL);
    set_bounds(btnSave, 16, 80, 120, 44);
    sg_node_add_child(form, btnSave);

    SGNode* btnDelete = widget_button("Delete", NULL, on_click, NULL);
    widget_button_set_enabled(btnDelete, 0);   /* -> BUTTON_DISABLED */
    btnDelete->state_flags |= SG_STATE_DISABLED;
    set_bounds(btnDelete, 16, 140, 120, 44);
    sg_node_add_child(form, btnDelete);

    SGNode* btnIcon = widget_button("+", NULL, on_click, NULL);
    sg_node_set_a11y_label(btnIcon, "Add item");
    set_bounds(btnIcon, 16, 200, 44, 44);
    sg_node_add_child(form, btnIcon);

    SGNode* email = widget_text_input("Email", NULL);
    set_bounds(email, 16, 260, 336, 46);
    sg_node_add_child(form, email);

    int change_after_build = g_change_calls;

    /* ---- register with the bridge ---- */
    cpx_a11y_set_root(root);
    cpx_a11y_set_root_origin(0, 100);          /* status bar 100px tall */

    printf("== Case 1: node count / parent-child / bounds / labels / roles ==\n");

    /* Surfaced nodes: title, btnSave, btnDelete, btnIcon, email.
     * NOT surfaced: root (plain container, no role/interactive/text),
     *               form (a11y_hidden), the buttons' hidden text children. */
    CHECK(cpx_a11y_node_count() == 5, "node_count == 5 surfaced nodes");

    int32_t idTitle  = id_with_label("Settings");
    int32_t idSave   = id_with_label("Save");
    int32_t idDelete = id_with_label("Delete");
    int32_t idIcon   = id_with_label("Add item");
    /* text input: label falls back to its own (empty) text -> "" ; find by role */
    int32_t idEmail = -1;
    for (int32_t i = 0; i < 5; i++) {
        int32_t id = cpx_a11y_node_id_at(i);
        if (strcmp(cpx_a11y_node_class_name(id), "android.widget.EditText") == 0)
            idEmail = id;
    }

    CHECK(idTitle > 0 && idSave > 0 && idDelete > 0 && idIcon > 0 && idEmail > 0,
          "all five surfaced nodes have distinct positive virtual ids");

    /* Hidden 'form' is transparent: its children re-parent to 'root', which is
     * itself not surfaced, so their a11y parent is -1 (the host view). */
    CHECK(cpx_a11y_node_parent(idSave)  == -1, "btnSave parent == -1 (form hidden, root not surfaced)");
    CHECK(cpx_a11y_node_parent(idTitle) == -1, "title parent == -1");
    CHECK(cpx_a11y_node_parent(idEmail) == -1, "email parent == -1");

    /* Bounds: node->bounds + origin(0,100). */
    CpxA11yRect r;
    CHECK(cpx_a11y_node_bounds(idTitle, &r) &&
          r.left == 16 && r.top == 124 && r.right == 216 && r.bottom == 156,
          "title bounds == {16,124,216,156} (bounds + origin)");
    CHECK(cpx_a11y_node_bounds(idSave, &r) &&
          r.left == 16 && r.top == 180 && r.right == 136 && r.bottom == 224,
          "btnSave bounds == {16,180,136,224}");
    CHECK(cpx_a11y_node_bounds(9999, &r) == 0, "unknown id -> no bounds");

    /* Labels. */
    CHECK(strcmp(cpx_a11y_node_label(idSave), "Save") == 0,
          "btnSave label derived from its child text node -> 'Save'");
    CHECK(strcmp(cpx_a11y_node_label(idIcon), "Add item") == 0,
          "btnIcon label from explicit a11y_label -> 'Add item' (not its '+' text)");
    CHECK(strcmp(cpx_a11y_node_label(idTitle), "Settings") == 0,
          "title label == 'Settings'");

    /* Roles / class names. */
    CHECK(strcmp(cpx_a11y_node_class_name(idTitle), "android.widget.TextView") == 0,
          "title (HEADER) -> android.widget.TextView");
    CHECK(strcmp(cpx_a11y_node_class_name(idSave), "android.widget.Button") == 0,
          "btnSave -> android.widget.Button");
    CHECK(strcmp(cpx_a11y_node_class_name(idEmail), "android.widget.EditText") == 0,
          "email -> android.widget.EditText");

    printf("\n== Case 1b: flags (disabled button, focusable edit text) ==\n");
    uint32_t fSave   = cpx_a11y_node_flags(idSave);
    uint32_t fDelete = cpx_a11y_node_flags(idDelete);
    uint32_t fEmail  = cpx_a11y_node_flags(idEmail);
    uint32_t fTitle  = cpx_a11y_node_flags(idTitle);

    CHECK((fSave & CPX_A11Y_FLAG_ENABLED) && (fSave & CPX_A11Y_FLAG_CLICKABLE) &&
          (fSave & CPX_A11Y_FLAG_FOCUSABLE) && (fSave & CPX_A11Y_FLAG_VISIBLE),
          "btnSave: enabled + clickable + focusable + visible");
    CHECK(!(fDelete & CPX_A11Y_FLAG_ENABLED) &&
          !(fDelete & CPX_A11Y_FLAG_CLICKABLE),
          "btnDelete (disabled): NOT enabled, NOT clickable");
    CHECK((fEmail & CPX_A11Y_FLAG_FOCUSABLE) && (fEmail & CPX_A11Y_FLAG_ENABLED),
          "email: focusable + enabled (EDIT_TEXT)");
    CHECK(!(fTitle & CPX_A11Y_FLAG_CLICKABLE) && !(fTitle & CPX_A11Y_FLAG_FOCUSABLE),
          "title (plain text): not clickable, not focusable");

    printf("\n== Case 2: perform_action ACTION_CLICK fires the handler ==\n");
    g_click_hits = 0;
    int handled = cpx_a11y_perform_action(idSave, 0x10 /* ACTION_CLICK */);
    CHECK(handled == 1, "ACTION_CLICK on btnSave reported handled");
    CHECK(g_click_hits == 1, "btnSave click handler fired exactly once (via sg_node_emit)");

    g_click_hits = 0;
    int handledDisabled = cpx_a11y_perform_action(idDelete, 0x10);
    CHECK(handledDisabled == 0 && g_click_hits == 0,
          "ACTION_CLICK on disabled btnDelete: not handled, handler not fired");

    printf("\n== Case 3: hidden node excluded, children re-parent to nearest visible ==\n");
    /* 'form' has an id but must NOT appear in enumeration. */
    int form_enumerated = 0;
    int32_t cnt = cpx_a11y_node_count();
    for (int32_t i = 0; i < cnt; i++) {
        if (cpx_a11y_node_id_at(i) == (int32_t)form->id) form_enumerated = 1;
    }
    CHECK(!form_enumerated, "hidden 'form' container is NOT in node enumeration");
    CHECK(cpx_a11y_node_child_count((int32_t)form->id) == 0,
          "querying children of the hidden form directly -> 0 (it isn't surfaced)");
    /* Its 4 children still surface (title + 4 = 5 total, already checked). */
    CHECK(cpx_a11y_node_count() == 5,
          "form's 4 children still surface despite the hidden wrapper");

    /* Now make a *surfaced* parent and confirm real child enumeration works. */
    SGNode* card = sg_node_create(SG_NODE_CONTAINER);
    sg_node_set_a11y_role(card, SG_A11Y_ROLE_NONE);
    sg_node_set_a11y_label(card, "Account");     /* label -> becomes surfaced? */
    /* label alone doesn't surface a NONE-role, non-interactive container; give
     * it a role so it's a real group. */
    sg_node_set_a11y_role(card, SG_A11Y_ROLE_TEXT);
    set_bounds(card, 0, 500, 400, 120);
    SGNode* line1 = widget_text("Name: Ada", NULL, 0xFF000000);
    SGNode* line2 = widget_text("Email: ada@x", NULL, 0xFF000000);
    sg_node_add_child(card, line1);
    sg_node_add_child(card, line2);
    sg_node_add_child(root, card);

    int32_t idCard = -1;
    cnt = cpx_a11y_node_count();
    for (int32_t i = 0; i < cnt; i++) {
        int32_t id = cpx_a11y_node_id_at(i);
        if (strcmp(cpx_a11y_node_label(id), "Account") == 0) idCard = id;
    }
    CHECK(idCard > 0, "surfaced 'card' group is enumerated (explicit label used)");
    CHECK(cpx_a11y_node_child_count(idCard) == 2,
          "card has 2 surfaced children (its two text lines)");
    int32_t c0 = cpx_a11y_node_child_at(idCard, 0);
    int32_t c1 = cpx_a11y_node_child_at(idCard, 1);
    CHECK(cpx_a11y_node_parent(c0) == idCard && cpx_a11y_node_parent(c1) == idCard,
          "card's children report card as their a11y parent");
    CHECK(strcmp(cpx_a11y_node_label(c0), "Name: Ada") == 0 &&
          strcmp(cpx_a11y_node_label(c1), "Email: ada@x") == 0,
          "card child labels in tree order");

    printf("\n== Case 4: structural-change notification ==\n");
    int before = g_change_calls;
    SGNode* extra = widget_text("Toast!", NULL, 0xFF000000);
    sg_node_add_child(root, extra);            /* structural mutation */
    CHECK(g_change_calls > before,
          "sg_node_add_child fired the structural-change callback");

    before = g_change_calls;
    widget_text_set(extra, "Toast updated");   /* label content change */
    CHECK(g_change_calls > before,
          "widget_text_set (label change) fired the structural-change callback");

    before = g_change_calls;
    sg_node_mark_dirty(extra, SG_DIRTY_PAINT); /* paint-only: must NOT fire */
    CHECK(g_change_calls == before,
          "sg_node_mark_dirty(PAINT) did NOT fire content-changed (no spam)");

    printf("\n== Case 5: hit-test via sg_hit_test + origin ==\n");
    /* email is at local {16,260,336,46} -> screen {16,360,352,406}. */
    int32_t hit = cpx_a11y_hit_test(100, 380);
    CHECK(hit == idEmail, "hit-test inside email -> email id");
    int32_t miss = cpx_a11y_hit_test(-5, -5);
    CHECK(miss == -1, "hit-test off-surface -> -1");

    printf("\n== Case 6: fallback when no root registered (v1a behaviour) ==\n");
    cpx_a11y_set_root(NULL);
    cpx_a11y_set_surface_size(1080, 2400);
    CHECK(cpx_a11y_node_count() == 1, "no root: node_count == 1 (fallback root)");
    CHECK(cpx_a11y_node_id_at(0) == 0, "no root: id_at(0) == 0 (fallback id)");
    CHECK(cpx_a11y_node_bounds(0, &r) && r.right == 1080 && r.bottom == 2400,
          "no root: fallback bounds == full surface");
    CHECK(strlen(cpx_a11y_node_label(0)) > 0, "no root: fallback label non-empty");
    CHECK(cpx_a11y_hit_test(10, 10) == 0, "no root: in-bounds hit -> fallback root");
    CHECK(cpx_a11y_perform_action(0, 0x10) == 0, "no root: no action handled");

    printf("\n== Case 7: content-changed consumer wiring (non-JNI half of #11) ==\n");
    /* cpx_a11y_install_scene_hook() connects the scene graph's structural-change
     * hook to the bridge; cpx_a11y_set_content_changed_cb() registers the
     * downstream consumer. On Android that consumer is jni_content_changed ->
     * A11yNodeProvider.onNativeContentChanged (cross-compile verified; the
     * posted TYPE_WINDOW_CONTENT_CHANGED needs a device to observe). Here a
     * plain C consumer proves native -> consumer flow end to end. */
    cpx_a11y_set_root(root);                 /* re-attach after the fallback case */
    g_consumer_hits = 0;
    cpx_a11y_install_scene_hook();           /* what nativeSetSurfaceSize() does */
    cpx_a11y_set_content_changed_cb(a11y_test_consumer);
    {
        SGNode* n7 = widget_text("Later", NULL, 0xFF000000);
        sg_node_add_child(root, n7);         /* structural mutation */
        CHECK(g_consumer_hits >= 1,
              "structural mutation -> sg hook -> bridge -> content-changed consumer");

        int hits_before_paint = g_consumer_hits;
        sg_node_mark_dirty(n7, SG_DIRTY_PAINT | SG_DIRTY_LAYOUT);
        CHECK(g_consumer_hits == hits_before_paint,
              "paint/layout dirty does NOT reach the content-changed consumer");

        int hits_before_label = g_consumer_hits;
        sg_node_set_a11y_label(n7, "Later, updated");
        CHECK(g_consumer_hits > hits_before_label,
              "a11y label change -> content-changed consumer");
    }

    (void)change_after_build;

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "ALL PASS",
           g_fail, g_fail == 1 ? "" : "s");
    sg_node_destroy_tree(root);
    return g_fail ? 1 : 0;
}
