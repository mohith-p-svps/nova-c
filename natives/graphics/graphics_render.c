// The only file in the `graphics` module that includes raylib.h/raygui.h
// — everything Nova-facing (module_native_graphics.c) talks to *this*
// file, never to raylib directly, so swapping the rendering backend
// later (see the roadmap's §4) means changing this one file, not the
// module's whole surface.
//
// Implements gfxRunEventLoop(), the blocking loop behind graphics.show():
// applies the default theme, recursively lays out and draws the
// window's widget tree every frame (row()/column() containers nest
// arbitrarily — see layoutChildren/measureHeight), and calls back into
// the Nova function a button's onClick or a checkbox's onChange was set
// to, via vm.h's callFunctionValue, when that happens.

#include <stdbool.h>

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "vendor/raygui.h"

#include "graphics_internal.h"
#include "../../vm.h"

#define GFX_ROW_HEIGHT     34  // preferred height of any leaf widget except images
#define GFX_IMAGE_HEIGHT   160 // preferred height of an image widget — see the
                                // "known limitations" note near drawImage()
#define GFX_CHECKBOX_SIZE  22  // the checkbox square itself; its label is
                                // drawn to the right of it by GuiCheckBox
#define GFX_TABS_BAR_GAP   6   // vertical gap between a tabs widget's tab
                                // bar and whichever panel is currently active
#define GFX_MAX_TAB_LABEL  64  // plenty for a tab label; longer ones are truncated

// The window currently inside gfxRunEventLoop's frame loop, if any —
// needed by the GFX_WIDGET_INPUT case so it can read/update
// focusedInput regardless of how deeply the input is nested inside
// row()/column() containers. Safe as a single static: only one
// gfxRunEventLoop call is ever on the stack at a time (graphics.show()
// blocks until its window closes, and Nova has no concurrency that
// could call it again before that), so there's never more than one
// "current" window to track.
static GfxWindow* gCurrentWindow = NULL;

// Set by fireCallback() when a Nova onClick/onChange handler raises an
// error, checked by layoutChildren() so the rest of that frame's widgets
// (siblings, deeper recursion) are skipped, and by the main loop so
// gfxRunEventLoop stops rather than carrying on as if nothing happened —
// same "let the error propagate like any other native call's would"
// reasoning as Phase 0/1.
static int gCallbackFailed = 0;

// One slot per widget registry index — GFX_WIDGET_IMAGE textures,
// preloaded once per gfxRunEventLoop call (see preloadImages) rather
// than at graphics.image() time, since no window/OpenGL context
// necessarily exists yet when a Nova script constructs its widgets.
static Texture2D gTextures[GFX_MAX_WIDGETS];
static int       gTextureState[GFX_MAX_WIDGETS]; // 0 = not loaded, 1 = ok, 2 = failed

// Which GFX_WIDGET_DROPDOWN, if any, needs its expanded list drawn this
// frame, and where — set while walking the tree (see layoutAndDraw's
// GFX_WIDGET_DROPDOWN case), consumed once, after everything else has
// drawn, in gfxRunEventLoop. See that function's comment for why an
// open dropdown can't just draw inline like every other widget.
static int      gOpenDropdownHandle = -1;
static Rectangle gOpenDropdownBounds;

// True only while a canvas's onDraw callback is actually executing —
// see gfxIsDrawingActive/gfxCanvasLine et al. gCanvasOrigin* is that
// canvas's top-left corner in real screen coordinates, used to translate
// the canvas-relative coordinates graphics.canvasLine/etc. receive.
static int   gDrawingActive = 0;
static float gCanvasOriginX = 0, gCanvasOriginY = 0;

// Deliberately defined this early (rather than down with the rest of
// the styling code that also uses it) since gfxCanvasLine and friends,
// right below, need it too.
static Color toRaylibColor(GfxColor c) { return (Color){ c.r, c.g, c.b, c.a }; }

int gfxIsDrawingActive(void) { return gDrawingActive; }

void gfxCanvasLine(float x1, float y1, float x2, float y2, float thickness, GfxColor color) {
    DrawLineEx((Vector2){ gCanvasOriginX + x1, gCanvasOriginY + y1 },
               (Vector2){ gCanvasOriginX + x2, gCanvasOriginY + y2 }, thickness, toRaylibColor(color));
}
void gfxCanvasRect(float x, float y, float w, float h, GfxColor color) {
    DrawRectangle((int)(gCanvasOriginX + x), (int)(gCanvasOriginY + y), (int)w, (int)h, toRaylibColor(color));
}
void gfxCanvasCircle(float x, float y, float radius, GfxColor color) {
    DrawCircle((int)(gCanvasOriginX + x), (int)(gCanvasOriginY + y), radius, toRaylibColor(color));
}
void gfxCanvasText(const char* text, float x, float y, int fontSize, GfxColor color) {
    DrawText(text, (int)(gCanvasOriginX + x), (int)(gCanvasOriginY + y), fontSize, toRaylibColor(color));
}

// --- default theme ---------------------------------------------------------
//
// Baked-in NovaLang look, applied once per window before the frame loop
// starts — this is what "good-looking with a handful of lines" actually
// leans on: colors, text size, and flat (borderless) buttons, all via
// raygui's GuiSetStyle. Known limitation, not attempted here: raygui's
// stock controls don't support rounded corners (no border-radius
// equivalent) — genuinely rounded widgets would mean hand-drawing them
// instead of using GuiButton/GuiLabel/etc. directly, which is out of
// scope for this pass. Symbolic property names (BACKGROUND_COLOR,
// BASE_COLOR_NORMAL, ...) come from raygui.h itself, not hardcoded
// numbers, so this stays correct regardless of the exact integer values
// behind them in whatever raygui version gets linked.
static void applyDefaultTheme(void) {
    Color bg        = (Color){ 245, 245, 247, 255 };
    Color textColor = (Color){  31,  41,  55, 255 };
    Color accent    = (Color){  79,  70, 229, 255 };
    Color accentHi  = (Color){  99, 102, 241, 255 };
    Color accentLo  = (Color){  67,  56, 202, 255 };
    Color white     = (Color){ 255, 255, 255, 255 };

    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(bg));
    GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(textColor));

    GuiSetStyle(LABEL, TEXT_COLOR_NORMAL, ColorToInt(textColor));

    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(accent));
    GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, ColorToInt(accentHi));
    GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, ColorToInt(accentLo));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(white));
    GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, ColorToInt(white));
    GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, ColorToInt(white));
    GuiSetStyle(BUTTON, BORDER_WIDTH, 0);

    GuiSetStyle(CHECKBOX, TEXT_COLOR_NORMAL, ColorToInt(textColor));
    GuiSetStyle(CHECKBOX, BASE_COLOR_PRESSED, ColorToInt(accent));

    GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL, ColorToInt(textColor));
    GuiSetStyle(TEXTBOX, BASE_COLOR_NORMAL, ColorToInt(white));
    GuiSetStyle(TEXTBOX, BORDER_COLOR_FOCUSED, ColorToInt(accent));
}

// --- per-widget styling (Phase 3) -----------------------------------------
//
// raygui's GuiSetStyle works per *control group* (all buttons, all
// labels, ...) — there's no built-in notion of styling one specific
// button differently from every other button. Per-widget styling is
// faked on top of that: right before drawing one widget, whichever
// properties it has overrides for get temporarily written over the
// group's current values via GuiSetStyle, the widget gets drawn, and
// the previous values get written straight back — see
// applyStyleOverrides/restoreStyle. Every leaf case in layoutAndDraw
// does this around its own draw call.

// (toRaylibColor moved above, near the other early state/helpers — see
// gfxCanvasLine et al., which need it before this point in the file.)

// Combines a widget's classes (in graphics.addClass() order) and then
// its own inline style (from graphics.style()) into one GfxStyle —
// later sources override whichever properties they set, inline always
// applying last. This is the entire "cascade": no selectors, no
// specificity, just "later wins" — see GfxNamedStyle's comment.
static GfxStyle effectiveStyle(GfxWidget* wd) {
    GfxStyle merged;
    memset(&merged, 0, sizeof(merged));
    for (int i = 0; i < wd->classCount; i++) {
        GfxStyle* s = &gfxNamedStyles[wd->classHandles[i]].style;
        if (s->hasBackground)  { merged.hasBackground = 1;  merged.backgroundColor = s->backgroundColor; }
        if (s->hasTextColor)   { merged.hasTextColor = 1;   merged.textColor = s->textColor; }
        if (s->hasBorderColor) { merged.hasBorderColor = 1; merged.borderColor = s->borderColor; }
        if (s->hasTextPadding) { merged.hasTextPadding = 1; merged.textPadding = s->textPadding; }
        if (s->hasFontSize)    { merged.hasFontSize = 1;    merged.fontSize = s->fontSize; }
    }
    GfxStyle* own = &wd->style;
    if (own->hasBackground)  { merged.hasBackground = 1;  merged.backgroundColor = own->backgroundColor; }
    if (own->hasTextColor)   { merged.hasTextColor = 1;   merged.textColor = own->textColor; }
    if (own->hasBorderColor) { merged.hasBorderColor = 1; merged.borderColor = own->borderColor; }
    if (own->hasTextPadding) { merged.hasTextPadding = 1; merged.textPadding = own->textPadding; }
    if (own->hasFontSize)    { merged.hasFontSize = 1;    merged.fontSize = own->fontSize; }
    return merged;
}

// What applyStyleOverrides changed, so restoreStyle can put it back —
// only the properties actually overridden get touched either way.
typedef struct {
    int hadBackground,  prevBackground;
    int hadTextColor,   prevTextColor;
    int hadBorderWidth, prevBorderWidth; // forced open whenever hasBorderColor is set — see below
    int hadBorderColor, prevBorderColor;
    int hadTextPadding, prevTextPadding;
    int hadFontSize,    prevFontSize;
} StyleSnapshot;

static StyleSnapshot applyStyleOverrides(const GfxStyle* style, int control) {
    StyleSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    if (style->hasBackground) {
        snap.hadBackground = 1;
        snap.prevBackground = GuiGetStyle(control, BASE_COLOR_NORMAL);
        GuiSetStyle(control, BASE_COLOR_NORMAL, ColorToInt(toRaylibColor(style->backgroundColor)));
    }
    if (style->hasTextColor) {
        snap.hadTextColor = 1;
        snap.prevTextColor = GuiGetStyle(control, TEXT_COLOR_NORMAL);
        GuiSetStyle(control, TEXT_COLOR_NORMAL, ColorToInt(toRaylibColor(style->textColor)));
    }
    if (style->hasBorderColor) {
        // A border color with BORDER_WIDTH still at the theme's default
        // of 0 would be invisible, which isn't what setting a border
        // color is for — force a visible width for the duration of this
        // one widget's draw, same as the color itself.
        snap.hadBorderWidth = 1;
        snap.prevBorderWidth = GuiGetStyle(control, BORDER_WIDTH);
        GuiSetStyle(control, BORDER_WIDTH, 2);

        snap.hadBorderColor = 1;
        snap.prevBorderColor = GuiGetStyle(control, BORDER_COLOR_NORMAL);
        GuiSetStyle(control, BORDER_COLOR_NORMAL, ColorToInt(toRaylibColor(style->borderColor)));
    }
    if (style->hasTextPadding) {
        snap.hadTextPadding = 1;
        snap.prevTextPadding = GuiGetStyle(control, TEXT_PADDING);
        GuiSetStyle(control, TEXT_PADDING, style->textPadding);
    }
    if (style->hasFontSize) {
        snap.hadFontSize = 1;
        snap.prevFontSize = GuiGetStyle(control, TEXT_SIZE);
        GuiSetStyle(control, TEXT_SIZE, style->fontSize);
    }
    return snap;
}

static void restoreStyle(const StyleSnapshot* snap, int control) {
    if (snap->hadBackground)   GuiSetStyle(control, BASE_COLOR_NORMAL, snap->prevBackground);
    if (snap->hadTextColor)    GuiSetStyle(control, TEXT_COLOR_NORMAL, snap->prevTextColor);
    if (snap->hadBorderColor)  GuiSetStyle(control, BORDER_COLOR_NORMAL, snap->prevBorderColor);
    if (snap->hadBorderWidth)  GuiSetStyle(control, BORDER_WIDTH, snap->prevBorderWidth);
    if (snap->hadTextPadding)  GuiSetStyle(control, TEXT_PADDING, snap->prevTextPadding);
    if (snap->hadFontSize)     GuiSetStyle(control, TEXT_SIZE, snap->prevFontSize);
}

// --- image loading -----------------------------------------------------

static void preloadImages(int handle, int depth) {
    if (depth >= GFX_MAX_DEPTH) return;
    GfxWidget* wd = &gfxWidgets[handle];
    if (wd->kind == GFX_WIDGET_IMAGE) {
        if (!gTextureState[handle]) {
            Texture2D tex = LoadTexture(wd->text);
            gTextures[handle] = tex;
            gTextureState[handle] = (tex.id != 0) ? 1 : 2; // raylib returns id==0 on a failed load
        }
    } else if (wd->kind == GFX_WIDGET_ROW || wd->kind == GFX_WIDGET_COLUMN) {
        for (int i = 0; i < wd->childCount; i++) preloadImages(wd->childHandles[i], depth + 1);
    }
}

static void unloadAllImages(void) {
    for (int i = 0; i < GFX_MAX_WIDGETS; i++) {
        if (gTextureState[i] == 1) UnloadTexture(gTextures[i]);
        gTextureState[i] = 0;
    }
}

// Known limitation: the texture is stretched to exactly fill `bounds`
// (via DrawTexturePro), not scaled to preserve its own aspect ratio —
// simpler and consistent with how every other widget already fills
// whatever rectangle the layout gives it, at the cost of distorting
// images whose proportions don't match their allocated box.
static void drawImage(int handle, Rectangle bounds) {
    if (gTextureState[handle] == 1) {
        Texture2D tex = gTextures[handle];
        Rectangle src = { 0, 0, (float)tex.width, (float)tex.height };
        DrawTexturePro(tex, src, bounds, (Vector2){ 0, 0 }, 0.0f, WHITE);
    } else {
        // Missing/unreadable file — a visible placeholder beats a silent
        // gap, so a bad path is obvious rather than mysterious.
        DrawRectangleRec(bounds, (Color){ 230, 230, 230, 255 });
        GuiLabel(bounds, "image not found");
    }
}

// --- callbacks -----------------------------------------------------------

// Invokes a Nova onClick/onChange handler with the widget's own handle
// as its one argument (see module_native_graphics.c's header comment on
// why: Nova functions can't see any outer variable, including the very
// widget they're attached to). Returns 1 on success; on failure, the
// error has already been raised (and printed) by callFunctionValue
// itself, and gCallbackFailed is set so the rest of this frame gets
// skipped and gfxRunEventLoop's main loop stops.
static void fireCallback(VM* vm, Value fn, const char* selfType, int selfHandle) {
    Value callArgs[1] = { gfxMakeHandle(selfType, selfHandle) };
    Value out;
    if (!callFunctionValue(vm, fn, callArgs, 1, &out)) {
        gCallbackFailed = 1;
        return;
    }
    freeValue(out);
}

// --- layout + drawing ----------------------------------------------------
//
// A minimal flexbox-like model: column() stacks children top-to-bottom,
// giving each its full available width and a height driven by its own
// content; row() places children left-to-right, splitting the available
// width evenly and giving each the row's full height. Both add `gap`
// between children and `padding` inset from their own bounds. A
// window's top-level children are laid out exactly like a column (see
// gfxRunEventLoop) — row()/column() containers can nest inside each
// other and inside a window to any depth, up to GFX_MAX_DEPTH as a
// safety net against runaway recursion (never expected to matter for a
// hand-written UI, cheap to guard against regardless).

static float measureHeight(int handle, int depth);

static float measureContainerHeight(GfxWidget* wd, int isColumn, int depth) {
    if (depth >= GFX_MAX_DEPTH) return GFX_ROW_HEIGHT;
    if (wd->childCount == 0) return 2.0f * wd->padding;
    if (isColumn) {
        float h = 2.0f * wd->padding + (float)(wd->childCount - 1) * wd->gap;
        for (int i = 0; i < wd->childCount; i++) h += measureHeight(wd->childHandles[i], depth + 1);
        return h;
    }
    float maxH = 0;
    for (int i = 0; i < wd->childCount; i++) {
        float h = measureHeight(wd->childHandles[i], depth + 1);
        if (h > maxH) maxH = h;
    }
    return 2.0f * wd->padding + maxH;
}

static float measureHeight(int handle, int depth) {
    GfxWidget* wd = &gfxWidgets[handle];
    switch (wd->kind) {
        case GFX_WIDGET_IMAGE:  return GFX_IMAGE_HEIGHT;
        case GFX_WIDGET_CANVAS: return wd->maxValue; // fixed height, set at construction — see gfx_canvas
        case GFX_WIDGET_TABS: {
            // Bar height plus whichever panel is currently active —
            // different panels can have different natural heights, so
            // this genuinely depends on selectedIndex, not just on the
            // tabs widget itself.
            if (wd->childCount == 0) return GFX_ROW_HEIGHT;
            float panelH = measureHeight(wd->childHandles[wd->selectedIndex], depth + 1);
            return GFX_ROW_HEIGHT + GFX_TABS_BAR_GAP + panelH;
        }
        case GFX_WIDGET_ROW:    return measureContainerHeight(wd, 0, depth);
        case GFX_WIDGET_COLUMN: return measureContainerHeight(wd, 1, depth);
        default:                return GFX_ROW_HEIGHT; // label/button/input/checkbox/slider/progressBar/radioGroup/dropdown
    }
}

static void layoutAndDraw(VM* vm, int handle, float x, float y, float w, float h, int depth);

// Shared by a window's top-level children (an implicit column — see
// gfxRunEventLoop) and real GFX_WIDGET_ROW/GFX_WIDGET_COLUMN nodes, so
// there's exactly one place that implements the row/column split.
static void layoutChildren(VM* vm, int isColumn, const int* childHandles, int childCount,
                            int gap, int padding, float x, float y, float w, float h, int depth) {
    if (gCallbackFailed || depth >= GFX_MAX_DEPTH || childCount == 0) return;

    float innerX = x + padding, innerY = y + padding;
    float innerW = w - 2.0f * padding, innerH = h - 2.0f * padding;

    if (isColumn) {
        float cy = innerY;
        for (int i = 0; i < childCount && !gCallbackFailed; i++) {
            float childH = measureHeight(childHandles[i], depth + 1);
            layoutAndDraw(vm, childHandles[i], innerX, cy, innerW, childH, depth + 1);
            cy += childH + gap;
        }
    } else {
        float totalGap = (float)(childCount - 1) * gap;
        float childW = (innerW - totalGap) / childCount;
        float cx = innerX;
        for (int i = 0; i < childCount && !gCallbackFailed; i++) {
            layoutAndDraw(vm, childHandles[i], cx, innerY, childW, innerH, depth + 1);
            cx += childW + gap;
        }
    }
}

static void layoutAndDraw(VM* vm, int handle, float x, float y, float w, float h, int depth) {
    if (gCallbackFailed) return;
    GfxWidget* wd = &gfxWidgets[handle];
    Rectangle bounds = { x, y, w, h };
    GfxStyle style = effectiveStyle(wd);

    switch (wd->kind) {
        case GFX_WIDGET_LABEL: {
            // GuiLabel only ever draws text, never a fill behind it —
            // unlike a button/checkbox/textbox, which raygui always
            // gives some kind of background. A "background" style on a
            // label would otherwise silently do nothing, so it's drawn
            // by hand here rather than through GuiSetStyle.
            if (style.hasBackground) DrawRectangleRec(bounds, toRaylibColor(style.backgroundColor));
            StyleSnapshot snap = applyStyleOverrides(&style, LABEL);
            GuiLabel(bounds, wd->text);
            restoreStyle(&snap, LABEL);
            break;
        }

        case GFX_WIDGET_BUTTON: {
            StyleSnapshot snap = applyStyleOverrides(&style, BUTTON);
            int clicked = GuiButton(bounds, wd->text);
            restoreStyle(&snap, BUTTON);
            if (clicked && wd->hasOnClick) fireCallback(vm, wd->onClick, "button", handle);
            break;
        }

        case GFX_WIDGET_INPUT: {
            int isFocused = gCurrentWindow && gCurrentWindow->focusedInput == handle;
            StyleSnapshot snap = applyStyleOverrides(&style, TEXTBOX);
            int toggled = GuiTextBox(bounds, wd->text, GFX_MAX_TEXT, isFocused);
            restoreStyle(&snap, TEXTBOX);
            if (toggled && gCurrentWindow) {
                gCurrentWindow->focusedInput = isFocused ? -1 : handle;
            }
            break;
        }

        case GFX_WIDGET_CHECKBOX: {
            Rectangle cb = { x, y + (h - GFX_CHECKBOX_SIZE) / 2.0f, GFX_CHECKBOX_SIZE, GFX_CHECKBOX_SIZE };
            bool checked = wd->checked ? true : false;
            int wasChecked = wd->checked;
            StyleSnapshot snap = applyStyleOverrides(&style, CHECKBOX);
            GuiCheckBox(cb, wd->text, &checked);
            restoreStyle(&snap, CHECKBOX);
            wd->checked = checked ? 1 : 0;
            if (wd->checked != wasChecked && wd->hasOnChange) {
                fireCallback(vm, wd->onChange, "checkbox", handle);
            }
            break;
        }

        case GFX_WIDGET_SLIDER: {
            float prevValue = wd->currentValue;
            StyleSnapshot snap = applyStyleOverrides(&style, SLIDER);
            GuiSlider(bounds, NULL, TextFormat("%.1f", wd->currentValue), &wd->currentValue, wd->minValue, wd->maxValue);
            restoreStyle(&snap, SLIDER);
            if (wd->currentValue != prevValue && wd->hasOnChange) {
                fireCallback(vm, wd->onChange, "slider", handle);
            }
            break;
        }

        case GFX_WIDGET_PROGRESSBAR: {
            float range = wd->maxValue - wd->minValue;
            float pct = (range != 0.0f) ? 100.0f * (wd->currentValue - wd->minValue) / range : 0.0f;
            StyleSnapshot snap = applyStyleOverrides(&style, PROGRESSBAR);
            GuiProgressBar(bounds, NULL, TextFormat("%.0f%%", pct), &wd->currentValue, wd->minValue, wd->maxValue);
            restoreStyle(&snap, PROGRESSBAR);
            break;
        }

        case GFX_WIDGET_RADIOGROUP: {
            // GuiToggleGroup's `bounds` is the size of ONE item, not the
            // whole group — it advances bounds.x by (bounds.width +
            // padding) per item internally, so the per-item width has to
            // be our allocated width divided by the option count, or
            // the group would overflow far past what layoutChildren
            // gave this widget.
            int optionCount = 1;
            for (const char* p = wd->text; *p; p++) if (*p == '\n') optionCount++;
            float itemWidth = w / (float)optionCount;
            Rectangle itemBounds = { x, y, itemWidth, h };

            int prevSelected = wd->selectedIndex;
            StyleSnapshot snap = applyStyleOverrides(&style, TOGGLE);
            GuiToggleGroup(itemBounds, wd->text, &wd->selectedIndex);
            restoreStyle(&snap, TOGGLE);
            // Comparing before/after rather than trusting GuiToggleGroup's
            // own return value: some raygui versions always return 0
            // there (see raygui issue #439), so this is the reliable way
            // to detect a real selection change regardless of version.
            if (wd->selectedIndex != prevSelected && wd->hasOnChange) {
                fireCallback(vm, wd->onChange, "radioGroup", handle);
            }
            break;
        }

        case GFX_WIDGET_DROPDOWN: {
            int isOpen = gCurrentWindow && gCurrentWindow->openDropdown == handle;
            if (isOpen) {
                // Defer actual drawing to after the whole tree is
                // walked, so it draws — and receives input — on top of
                // everything else. See gfxRunEventLoop for why:
                // GuiDropdownBox's own expanded list has to be the last
                // thing drawn in the frame, with every other control
                // locked while it's open, or a click landing "through"
                // it could also activate whatever's visually underneath.
                gOpenDropdownHandle = handle;
                gOpenDropdownBounds = bounds;
            } else {
                StyleSnapshot snap = applyStyleOverrides(&style, DROPDOWNBOX);
                if (GuiDropdownBox(bounds, wd->text, &wd->selectedIndex, false) && gCurrentWindow) {
                    gCurrentWindow->openDropdown = handle;
                }
                restoreStyle(&snap, DROPDOWNBOX);
            }
            break;
        }

        case GFX_WIDGET_TABS: {
            int tabCount = wd->childCount;
            if (tabCount == 0) break;
            float tabW = w / (float)tabCount;
            int prevSelected = wd->selectedIndex;

            const char* p = wd->text;
            for (int i = 0; i < tabCount; i++) {
                const char* end = p;
                while (*end && *end != '\n') end++;
                char labelBuf[GFX_MAX_TAB_LABEL];
                int labelLen = (int)(end - p);
                if (labelLen >= (int)sizeof(labelBuf)) labelLen = sizeof(labelBuf) - 1;
                memcpy(labelBuf, p, labelLen);
                labelBuf[labelLen] = '\0';

                Rectangle tabBounds = { x + i * tabW, y, tabW, GFX_ROW_HEIGHT };
                int isActive = (i == wd->selectedIndex);

                GfxStyle tabStyle;
                memset(&tabStyle, 0, sizeof(tabStyle));
                if (isActive) {
                    // Reuse whatever the theme already set for a
                    // button's "focused" state as the active tab's
                    // color, rather than hard-coding a second copy of
                    // the accent color here — if the theme ever
                    // changes, the active tab stays consistent with it
                    // automatically instead of drifting out of sync.
                    int accentPacked = GuiGetStyle(BUTTON, BASE_COLOR_FOCUSED);
                    tabStyle.hasBackground = 1;
                    tabStyle.backgroundColor = (GfxColor){
                        (unsigned char)((accentPacked >> 24) & 0xFF), (unsigned char)((accentPacked >> 16) & 0xFF),
                        (unsigned char)((accentPacked >> 8) & 0xFF), (unsigned char)(accentPacked & 0xFF)
                    };
                    tabStyle.hasTextColor = 1;
                    tabStyle.textColor = (GfxColor){ 255, 255, 255, 255 };
                }
                StyleSnapshot snap = applyStyleOverrides(&tabStyle, BUTTON);
                if (GuiButton(tabBounds, labelBuf)) wd->selectedIndex = i;
                restoreStyle(&snap, BUTTON);

                p = (*end == '\n') ? end + 1 : end;
            }

            if (wd->selectedIndex != prevSelected && wd->hasOnChange) {
                fireCallback(vm, wd->onChange, "tabs", handle);
            }

            // Only the active tab's panel is ever drawn — switching
            // tabs swaps out entire subtrees of content, not just which
            // button looks pressed, which is what makes this different
            // from a plain row of buttons.
            if (!gCallbackFailed && wd->childCount > 0) {
                float panelY = y + GFX_ROW_HEIGHT + GFX_TABS_BAR_GAP;
                float panelH = h - GFX_ROW_HEIGHT - GFX_TABS_BAR_GAP;
                layoutAndDraw(vm, wd->childHandles[wd->selectedIndex], x, panelY, w, panelH, depth + 1);
            }
            break;
        }

        case GFX_WIDGET_CANVAS: {
            // Clipped to its own bounds so a script can't accidentally
            // (or deliberately) draw outside the space it was given and
            // paint over sibling widgets.
            BeginScissorMode((int)x, (int)y, (int)w, (int)h);
            gDrawingActive = 1;
            gCanvasOriginX = x;
            gCanvasOriginY = y;
            if (wd->hasOnClick) { // reused as "has onDraw" — see the struct comment; always true for a real canvas
                fireCallback(vm, wd->onClick, "canvas", handle);
            }
            gDrawingActive = 0;
            EndScissorMode();
            break;
        }

        case GFX_WIDGET_IMAGE:
            // Styling doesn't apply to images — there's no GuiSetStyle
            // control group for a raw texture draw, and "background
            // color behind a photo" isn't a property CSS itself gives
            // <img> either. graphics.style() still accepts the call (an
            // image is a valid target for the *other* natives that
            // check any-widget-handle), it just has nothing to change here.
            drawImage(handle, bounds);
            break;

        case GFX_WIDGET_ROW:
            layoutChildren(vm, 0, wd->childHandles, wd->childCount, wd->gap, wd->padding, x, y, w, h, depth);
            break;

        case GFX_WIDGET_COLUMN:
            layoutChildren(vm, 1, wd->childHandles, wd->childCount, wd->gap, wd->padding, x, y, w, h, depth);
            break;
    }
}

// --- entry point -----------------------------------------------------------

void gfxRunEventLoop(VM* vm, int windowHandle) {
    GfxWindow* w = &gfxWindows[windowHandle];
    gCurrentWindow = w;
    gCallbackFailed = 0;

    // Resizable by default — the layout below is computed fresh every
    // frame from the *current* screen size (GetScreenWidth/Height), not
    // the size the window was created with, so dragging to resize
    // reflows the whole widget tree smoothly rather than clipping it or
    // leaving dead space.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(w->width, w->height, w->title);
    SetTargetFPS(60);
    applyDefaultTheme();

    for (int i = 0; i < w->childCount; i++) preloadImages(w->childHandles[i], 0);

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));

        gOpenDropdownHandle = -1;

        // If a dropdown is open, every other control gets locked
        // (rendered, but inert to clicks) for this frame — otherwise a
        // click meant for the dropdown's expanded list could "land
        // through" onto whatever's visually underneath it. Matches the
        // official raygui example's own GuiLock()-before/GuiUnlock()-
        // right-before-the-dropdown-itself pattern.
        if (w->openDropdown != -1) GuiLock();

        layoutChildren(vm, 1 /* the window's own children stack like a column */,
                        w->childHandles, w->childCount, w->gap, w->padding,
                        0, 0, (float)GetScreenWidth(), (float)GetScreenHeight(), 0);

        if (w->openDropdown != -1) {
            GuiUnlock();
            // The open dropdown itself was skipped during the walk
            // above (see its case in layoutAndDraw) specifically so it
            // can be drawn here instead — last, on top of everything
            // else this frame drew, which is what makes its expanded
            // list actually visible and clickable rather than
            // (correctly rendered but) buried under later siblings.
            if (gOpenDropdownHandle != -1) {
                GfxWidget* dd = &gfxWidgets[gOpenDropdownHandle];
                int prevSelected = dd->selectedIndex;
                if (GuiDropdownBox(gOpenDropdownBounds, dd->text, &dd->selectedIndex, true)) {
                    w->openDropdown = -1; // clicked again (or picked an option) — close it
                }
                if (dd->selectedIndex != prevSelected && dd->hasOnChange && !gCallbackFailed) {
                    fireCallback(vm, dd->onChange, "dropdown", gOpenDropdownHandle);
                }
            }
        }

        EndDrawing();

        if (gCallbackFailed) break; // error already raised inside some handler — stop, don't carry on
    }

    unloadAllImages();
    gCurrentWindow = NULL;
    CloseWindow();
}
