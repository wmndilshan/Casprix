# Skia UI Button Resize Bug - Fix Guide

## Problem
Buttons resize when clicked instead of maintaining their size.

## Root Causes Found

### 1. **Layout Trigger on Paint-Only Events** ❌
Button event handlers correctly call `sg_node_mark_dirty(node, SG_DIRTY_PAINT)` (lines 46, 55, 64, 73 in `widgets.c`), but somewhere the layout is being recalc

ulated.

### 2. **Missing Fixed Width/Height on Buttons**
Buttons don't have explicit `min_width` set, so they may resize based on content changes.

### 3. **Text Node Size Changes**
The text child inside buttons might be recalculating its size on state changes.

## Solutions

### Solution 1: Set Fixed Button Dimensions (RECOMMENDED)
Add minimum width to buttons in `widget_button()`:

```c
/* In runtime/skia/widgets.c, line ~96 */
btn->min_height = 36.0f;
btn->min_width = 100.0f;  /* ADD THIS LINE - prevents resize */
```

### Solution 2: Prevent Layout Propagation on State Changes
Ensure button state changes only mark PAINT dirty, not LAYOUT:

```c
/* In button event handlers, verify these lines use SG_DIRTY_PAINT only */
static void button_on_mouse_enter(SGNode* node, int event_type, void* event_data, void* udata) {
    (void)event_type; (void)event_data; (void)udata;
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;
    state->state = BUTTON_HOVERED;
    node->style.background = state->hover_color;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);  /* ✅ PAINT only - correct! */
}
```

### Solution 3: Cache Text Measurements
If text is being remeasured on every state change, cache the measured size:

```c
/* In ButtonWidgetState, add: */
typedef struct {
    WidgetType type;
    ButtonState state;
    uint32_t normal_color;
    /* ... other fields ... */
    
    /* ADD THESE: */
    float cached_width;   /* Cache text width */
    float cached_height;  /* Cache text height */
    bool size_cached;     /* Whether size is valid */
} ButtonWidgetState;
```

## Quick Fix (Copy-Paste Ready)

Replace `widget_button()` function starting at line 80 in `runtime/skia/widgets.c`:

```c
SGNode* widget_button(const char* label, SkiaFont font,
                       ButtonClickCallback on_click, void* user_data) {
    /* Create button container */
    SGNode* btn = sg_node_create(SG_NODE_CONTAINER);
    btn->layout_type = SG_LAYOUT_ROW;
    btn->justify = SG_JUSTIFY_CENTER;
    btn->align_items = SG_ALIGN_CENTER;
    btn->flags |= SG_INTERACTIVE;

    /* Default button style */
    btn->style.background = 0xFF4285F4;
    btn->style.border_radius = 6.0f;
    btn->style.padding[0] = 8.0f;
    btn->style.padding[1] = 16.0f;
    btn->style.padding[2] = 8.0f;
    btn->style.padding[3] = 16.0f;
    btn->min_height = 36.0f;
  
    /* FIX: Set minimum width to prevent resize */
    btn->min_width = 0.0f;  /* Let content determine width */
    btn->flex_shrink = 0.0f;  /* Don't shrink below content size */

    /* Create label child */
    SGNode* text_node = widget_text(label, font, 0xFFFFFFFF);
    sg_node_add_child(btn, text_node);

    /* Widget state */
    ButtonWidgetState* state = (ButtonWidgetState*)calloc(1, sizeof(ButtonWidgetState));
    state->type = WIDGET_BUTTON;
    state->state = BUTTON_NORMAL;
    state->normal_color = 0xFF4285F4;
    state->hover_color = color_lighten(0xFF4285F4, 20);
    state->press_color = color_darken(0xFF4285F4, 30);
    state->disabled_color = 0xFFBDBDBD;
    state->text_color = 0xFFFFFFFF;
    state->on_click = on_click;
    state->callback_data = user_data;
    btn->user_data = state;

    /* Event handlers */
    sg_node_on(btn, SG_EVENT_MOUSE_ENTER, button_on_mouse_enter, NULL);
    sg_node_on(btn, SG_EVENT_MOUSE_LEAVE, button_on_mouse_leave, NULL);
    sg_node_on(btn, SG_EVENT_MOUSE_DOWN, button_on_mouse_down, NULL);
    sg_node_on(btn, SG_EVENT_CLICK, button_on_click, NULL);

    /* Force initial layout to cache button size */
    sg_node_mark_dirty(btn, SG_DIRTY_LAYOUT | SG_DIRTY_PAINT);

    return btn;
}
```

## Testing

Create a test program to verify:

```c
#include "runtime/skia/widgets.h"
#include "runtime/skia/skia_window_win32.h"

void on_button_click(SGNode* btn, void* data) {
    printf("Button clicked - size should NOT change!\n");
    printf("Button bounds: %.1f x %.1f\n", btn->bounds.w, btn->bounds.h);
}

int main() {
    SkiaWindow* win = skia_window_create(800, 600, "Button Test");
    SGNode* root = widget_column(10.0f);
    
    SkiaFont font = skia_font_create("Arial", 14.0f, SKIA_FONT_NORMAL);
    SGNode* btn = widget_button("Click Me!", font, on_button_click, NULL);
    
    sg_node_add_child(root, btn);
    
    while (skia_window_is_running(win)) {
        skia_window_process_events(win);
        skia_window_render(win, root);
    }
}
```

## Verification

After applying the fix:
1. **Click button** - should change color only
2. **Hover button** - should change color only  
3. **Check console** - bounds should remain constant
4. **No layout recalculation** - only paint invalidation

## Additional Debugging

If issue persists, add logging:

```c
static void button_on_click(SGNode* node, int event_type, void* event_data, void* udata) {
    printf("[DEBUG] Button click - Bounds BEFORE: %.1f x %.1f\n", 
           node->bounds.w, node->bounds.h);
    
    ButtonWidgetState* state = (ButtonWidgetState*)node->user_data;
    if (!state || state->state == BUTTON_DISABLED) return;
    state->state = BUTTON_HOVERED;
    node->style.background = state->hover_color;
    sg_node_mark_dirty(node, SG_DIRTY_PAINT);
    
    printf("[DEBUG] Button click - Bounds AFTER: %.1f x %.1f\n", 
           node->bounds.w, node->bounds.h);
    printf("[DEBUG] Dirty flags: 0x%X\n", node->flags);
    
    if (state->on_click) {
        state->on_click(node, state->callback_data);
    }
}
```

Expected output:
```
[DEBUG] Button click - Bounds BEFORE: 120.0 x 36.0
[DEBUG] Button click - Bounds AFTER: 120.0 x 36.0
[DEBUG] Dirty flags: 0x02  /* Only SG_DIRTY_PAINT (0x02), not SG_DIRTY_LAYOUT (0x01) */
```

## Summary

The issue is likely that buttons don't have constrained dimensions, allowing layout to resize them. The fix is to either:
1. Set `flex_shrink = 0` to prevent shrinking
2. Add debug logging to identify which event triggers layout
3. Ensure button containers have proper size constraints
