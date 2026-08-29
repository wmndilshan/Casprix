/*
 * test_text_input_states.c — disabled + error visual/behavioural states for
 * widget_text_input, and their reporting through the accessibility bridge.
 *
 * Mirrors the SGNode-direct harness used by test_a11y_bridge.c: real
 * scene_graph.o + widgets.o + layout.o + events.o + style.o linked against a
 * tiny skia stub. No Android, no JNI — the bridge is driven through its
 * cpx_a11y_* entry points directly.
 *
 * VERIFY (from the task spec):
 *   1. Disabled input differs visually from enabled, rejects focus / key /
 *      text / click state mutation, and reports NOT enabled / NOT focusable
 *      through cpx_a11y_node_flags (SG_STATE_DISABLED + SG_INTERACTIVE cleared).
 *   2. Error input shows a distinct visual (not the disabled visual) and stays
 *      editable — typing and cursor movement still mutate state.
 */
#include "a11y_bridge.h"
#include "scene_graph.h"
#include "widgets.h"
#include "events.h"
#include "style.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", (msg)); } \
    else      { printf("  FAIL  %s\n", (msg)); g_fail++; } \
} while (0)

static void set_bounds(SGNode* n, float x, float y, float w, float h) {
    n->bounds.x = x; n->bounds.y = y; n->bounds.w = w; n->bounds.h = h;
}

static TextInputWidgetState* st(SGNode* n) {
    return (TextInputWidgetState*)n->user_data;
}

/* Synthesise the events the runtime dispatchers would deliver to a focused /
 * clicked input, straight to the node (same path sg_dispatch_* ends at). */
static void emit_focus_in(SGNode* n) {
    SGEvent e; memset(&e, 0, sizeof e);
    e.type = SG_EVENT_FOCUS_IN; e.target = n; e.current = n;
    sg_node_emit(n, SG_EVENT_FOCUS_IN, &e);
}
static void emit_click(SGNode* n, float local_x) {
    SGEvent e; memset(&e, 0, sizeof e);
    e.type = SG_EVENT_CLICK; e.target = n; e.current = n;
    e.data.mouse.local_x = local_x; e.data.mouse.local_y = 10.0f;
    sg_node_emit(n, SG_EVENT_CLICK, &e);
}
static void emit_text(SGNode* n, const char* s) {
    SGEvent e; memset(&e, 0, sizeof e);
    e.type = SG_EVENT_TEXT_INPUT; e.target = n; e.current = n;
    strncpy(e.data.text_input.text, s, sizeof e.data.text_input.text - 1);
    sg_node_emit(n, SG_EVENT_TEXT_INPUT, &e);
}
static void emit_key(SGNode* n, int keycode) {
    SGEvent e; memset(&e, 0, sizeof e);
    e.type = SG_EVENT_KEY_DOWN; e.target = n; e.current = n;
    e.data.key.keycode = keycode;
    sg_node_emit(n, SG_EVENT_KEY_DOWN, &e);
}

int main(void) {
    SGNode* root = sg_node_create(SG_NODE_CONTAINER);
    set_bounds(root, 0, 0, 400, 800);

    SGNode* in = widget_text_input("Email", NULL);
    set_bounds(in, 16, 40, 300, 46);
    sg_node_add_child(root, in);

    uint32_t norm_bg     = in->style.background;
    uint32_t norm_border = in->style.border_color;

    printf("== Case A: baseline enabled input accepts focus + typing ==\n");
    CHECK(widget_text_input_is_enabled(in) == 1, "new input is enabled by default");
    CHECK(widget_text_input_has_error(in) == 0, "new input has no error by default");
    CHECK((in->flags & SG_INTERACTIVE) != 0, "enabled input keeps SG_INTERACTIVE");
    CHECK((in->state_flags & SG_STATE_DISABLED) == 0, "enabled input: no SG_STATE_DISABLED");

    emit_focus_in(in);
    CHECK(st(in)->focused == 1, "focus_in focuses an enabled input");
    emit_text(in, "a");
    emit_text(in, "b");
    CHECK(strcmp(widget_text_input_get_value(in), "ab") == 0, "typing into enabled input works");
    emit_key(in, SG_KEY_LEFT);
    CHECK(st(in)->cursor_pos == 1, "arrow-left moves cursor in enabled input");
    emit_key(in, SG_KEY_END);   /* park cursor at end for the following cases */

    printf("\n== Case B: disable -> distinct visual, input blocked ==\n");
    widget_text_input_set_enabled(in, 0);
    CHECK(widget_text_input_is_enabled(in) == 0, "is_enabled() reports 0 after disable");
    CHECK(st(in)->focused == 0, "disabling drops focus");
    CHECK((in->flags & SG_INTERACTIVE) == 0, "disabled input clears SG_INTERACTIVE");
    CHECK((in->state_flags & SG_STATE_DISABLED) != 0, "disabled input sets SG_STATE_DISABLED");
    CHECK(in->style.background == SG_COLOR_DISABLED_SURFACE,
          "disabled background == SG_COLOR_DISABLED_SURFACE (distinct from normal)");
    CHECK(in->style.border_color == SG_COLOR_DISABLED,
          "disabled border == SG_COLOR_DISABLED");
    CHECK(in->style.background != norm_bg && in->style.border_color != norm_border,
          "disabled visual differs from the enabled visual");

    /* Behaviour: none of focus / click / text / key may mutate cursor or text. */
    int cur0 = st(in)->cursor_pos;
    char txt0[64];
    strncpy(txt0, widget_text_input_get_value(in), sizeof txt0 - 1);
    txt0[sizeof txt0 - 1] = '\0';
    emit_focus_in(in);
    CHECK(st(in)->focused == 0, "focus_in on a disabled input does NOT focus it");
    emit_click(in, 120.0f);
    CHECK(st(in)->cursor_pos == cur0, "click on a disabled input does NOT move the cursor");
    emit_text(in, "z");
    CHECK(strcmp(widget_text_input_get_value(in), txt0) == 0,
          "text event on a disabled input does NOT change its value");
    emit_key(in, SG_KEY_BACKSPACE);
    CHECK(strcmp(widget_text_input_get_value(in), txt0) == 0,
          "key event on a disabled input does NOT change its value");

    printf("\n== Case C: re-enable restores the normal visual + editability ==\n");
    widget_text_input_set_enabled(in, 1);
    CHECK(widget_text_input_is_enabled(in) == 1, "is_enabled() reports 1 after re-enable");
    CHECK((in->flags & SG_INTERACTIVE) != 0, "re-enabled input restores SG_INTERACTIVE");
    CHECK((in->state_flags & SG_STATE_DISABLED) == 0, "re-enabled input clears SG_STATE_DISABLED");
    CHECK(in->style.background == norm_bg && in->style.border_color == norm_border,
          "re-enabled input restores the captured normal colors");
    emit_focus_in(in);
    emit_key(in, SG_KEY_END);
    emit_text(in, "c");
    CHECK(strcmp(widget_text_input_get_value(in), "abc") == 0, "typing works again after re-enable");

    printf("\n== Case D: error state -> distinct visual, still editable ==\n");
    widget_text_input_set_error(in, 1);
    CHECK(widget_text_input_has_error(in) == 1, "has_error() reports 1 after set_error(1)");
    CHECK(widget_text_input_is_enabled(in) == 1, "set_error does not disable the input");
    CHECK((in->flags & SG_INTERACTIVE) != 0, "errored input keeps SG_INTERACTIVE");
    CHECK((in->state_flags & SG_STATE_DISABLED) == 0, "errored input has no SG_STATE_DISABLED");
    CHECK(in->style.border_color == SG_COLOR_ERROR, "error border == SG_COLOR_ERROR");
    CHECK(in->style.border_color != SG_COLOR_DISABLED &&
          in->style.background != SG_COLOR_DISABLED_SURFACE,
          "error visual is distinct from the disabled visual");
    /* editability */
    emit_focus_in(in);
    CHECK(st(in)->focused == 1, "errored input still accepts focus");
    emit_key(in, SG_KEY_END);
    emit_text(in, "d");
    CHECK(strcmp(widget_text_input_get_value(in), "abcd") == 0, "typing works in an errored input");
    int curbefore = st(in)->cursor_pos;
    emit_key(in, SG_KEY_LEFT);
    CHECK(st(in)->cursor_pos == curbefore - 1, "cursor still moves in an errored input");

    printf("\n== Case E: clear error -> back to normal visual ==\n");
    widget_text_input_set_error(in, 0);
    CHECK(widget_text_input_has_error(in) == 0, "has_error() reports 0 after set_error(0)");
    CHECK(in->style.border_color == norm_border && in->style.background == norm_bg,
          "clearing the error restores the normal colors");

    printf("\n== Case F: disabled wins over error visually ==\n");
    widget_text_input_set_error(in, 1);
    widget_text_input_set_enabled(in, 0);
    CHECK(in->style.background == SG_COLOR_DISABLED_SURFACE &&
          in->style.border_color == SG_COLOR_DISABLED,
          "disabled+error shows the disabled visual (disabled wins)");
    CHECK(widget_text_input_has_error(in) == 1, "error flag is retained while disabled");
    widget_text_input_set_enabled(in, 1);
    CHECK(in->style.border_color == SG_COLOR_ERROR,
          "re-enabling a still-errored input re-applies the error visual");
    widget_text_input_set_error(in, 0);

    printf("\n== Case G: accessibility bridge reporting ==\n");
    cpx_a11y_set_root(root);
    cpx_a11y_set_root_origin(0, 0);

    int32_t idEmail = -1;
    int32_t cnt = cpx_a11y_node_count();
    for (int32_t i = 0; i < cnt; i++) {
        int32_t id = cpx_a11y_node_id_at(i);
        if (strcmp(cpx_a11y_node_class_name(id), "android.widget.EditText") == 0)
            idEmail = id;
    }
    CHECK(idEmail > 0, "text input surfaces as android.widget.EditText");

    uint32_t fEnabled = cpx_a11y_node_flags(idEmail);
    CHECK((fEnabled & CPX_A11Y_FLAG_ENABLED) && (fEnabled & CPX_A11Y_FLAG_FOCUSABLE),
          "enabled input: a11y ENABLED + FOCUSABLE");

    widget_text_input_set_enabled(in, 0);
    /* the tree structure is unchanged; the bridge re-reads node state live */
    uint32_t fDisabled = cpx_a11y_node_flags(idEmail);
    CHECK(!(fDisabled & CPX_A11Y_FLAG_ENABLED),
          "disabled input: a11y NOT ENABLED (SG_STATE_DISABLED honoured)");
    CHECK(!(fDisabled & CPX_A11Y_FLAG_FOCUSABLE),
          "disabled input: a11y NOT FOCUSABLE (SG_INTERACTIVE cleared)");
    CHECK(!(fDisabled & CPX_A11Y_FLAG_CLICKABLE),
          "disabled input: a11y NOT CLICKABLE");
    CHECK(cpx_a11y_perform_action(idEmail, 0x01 /* ACTION_FOCUS */) == 0,
          "disabled input: perform_action refused (SG_STATE_DISABLED)");

    widget_text_input_set_enabled(in, 1);
    uint32_t fReenabled = cpx_a11y_node_flags(idEmail);
    CHECK((fReenabled & CPX_A11Y_FLAG_ENABLED) && (fReenabled & CPX_A11Y_FLAG_FOCUSABLE),
          "re-enabled input: a11y ENABLED + FOCUSABLE again");

    /* an errored-but-enabled input is still fully accessible */
    widget_text_input_set_error(in, 1);
    uint32_t fError = cpx_a11y_node_flags(idEmail);
    CHECK((fError & CPX_A11Y_FLAG_ENABLED) && (fError & CPX_A11Y_FLAG_FOCUSABLE),
          "errored input: a11y still ENABLED + FOCUSABLE");

    printf("\n== Case H: widget_text_validate — the .cpx Field/Form rule engine ==\n");
    CHECK(widget_text_validate(WIDGET_VALIDATE_NONE, 0, "") == 1,
          "NONE: empty is valid");
    CHECK(widget_text_validate(WIDGET_VALIDATE_REQUIRED, 0, "") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_REQUIRED, 0, "   ") == 0,
          "REQUIRED: empty / whitespace-only is invalid");
    CHECK(widget_text_validate(WIDGET_VALIDATE_REQUIRED, 0, "  x ") == 1,
          "REQUIRED: a non-space character is valid");
    CHECK(widget_text_validate(WIDGET_VALIDATE_MIN_LEN, 3, "ab") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_MIN_LEN, 3, "abc") == 1,
          "MIN_LEN: trimmed length compared against param");
    CHECK(widget_text_validate(WIDGET_VALIDATE_MIN_LEN, 3, " abc ") == 1,
          "MIN_LEN: surrounding spaces are trimmed before the length check");
    CHECK(widget_text_validate(WIDGET_VALIDATE_MAX_LEN, 4, "abcd") == 1 &&
          widget_text_validate(WIDGET_VALIDATE_MAX_LEN, 4, "abcde") == 0,
          "MAX_LEN: trimmed length compared against param");
    CHECK(widget_text_validate(WIDGET_VALIDATE_EMAIL, 0, "ada@example.com") == 1,
          "EMAIL: a dotted domain with an interior '@' is valid");
    CHECK(widget_text_validate(WIDGET_VALIDATE_EMAIL, 0, "not-an-email") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_EMAIL, 0, "a@b") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_EMAIL, 0, "a@@b.com") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_EMAIL, 0, "@example.com") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_EMAIL, 0, "ada@.com") == 0,
          "EMAIL: rejects missing '@', missing dot, double '@', empty local/label");
    CHECK(widget_text_validate(WIDGET_VALIDATE_NUMERIC, 0, "41") == 1 &&
          widget_text_validate(WIDGET_VALIDATE_NUMERIC, 0, "-5") == 1 &&
          widget_text_validate(WIDGET_VALIDATE_NUMERIC, 0, "") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_NUMERIC, 0, "4a") == 0 &&
          widget_text_validate(WIDGET_VALIDATE_NUMERIC, 0, "-") == 0,
          "NUMERIC: ASCII digits with an optional leading '-'");
    CHECK(widget_text_validate(WIDGET_VALIDATE_REQUIRED, 0, NULL) == 0,
          "NULL text is treated as empty");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "ALL PASS",
           g_fail, g_fail == 1 ? "" : "s");
    sg_node_destroy_tree(root);
    return g_fail ? 1 : 0;
}
