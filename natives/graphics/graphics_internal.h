#ifndef GRAPHICS_INTERNAL_H
#define GRAPHICS_INTERNAL_H

// Shared between module_native_graphics.c (creates and validates handles,
// no raylib knowledge) and graphics_render.c (the only file that touches
// raylib/raygui — see its header comment for why that split exists).
// Deliberately contains no raylib types, so module_native_graphics.c can
// be compiled and unit-tested without raylib/raygui installed at all.

#include "../../value.h"

#define GFX_MAX_WINDOWS   8
#define GFX_MAX_WIDGETS   256
#define GFX_MAX_TEXT      256
#define GFX_MAX_CHILDREN  64  // per window, and per row()/column() container
#define GFX_MAX_DEPTH     32  // recursion guard for nested row()/column() — see graphics_render.c

typedef enum {
    GFX_WIDGET_LABEL,
    GFX_WIDGET_BUTTON,
    GFX_WIDGET_INPUT,
    GFX_WIDGET_CHECKBOX,
    GFX_WIDGET_IMAGE,
    GFX_WIDGET_SLIDER,
    GFX_WIDGET_PROGRESSBAR,
    GFX_WIDGET_RADIOGROUP,
    GFX_WIDGET_DROPDOWN,
    GFX_WIDGET_TABS,
    GFX_WIDGET_CANVAS,
    GFX_WIDGET_ROW,
    GFX_WIDGET_COLUMN
} GfxWidgetKind;

// Plain r/g/b/a bytes — deliberately the same field layout raylib's
// Color struct uses, so graphics_render.c (the only file that knows
// what a real Color is) can build one directly from a GfxColor with a
// plain struct literal. Keeping the color type itself raylib-agnostic
// is what lets module_native_graphics.c parse "#4F46E5"-style strings
// (see parseColorString) without any raylib dependency.
typedef struct {
    unsigned char r, g, b, a;
} GfxColor;

// One widget's (or one named style's — see GfxNamedStyle) style
// overrides. Every field is paired with a `has*` flag rather than using
// a sentinel value, since 0 is a perfectly valid color channel or
// padding value — "unset" has to be tracked explicitly. A style with
// every `has*` flag false changes nothing; the widget just draws with
// the plain default theme, same as before Phase 3 existed. Only 5
// properties are supported — background, text color, border color,
// text padding, font size — because those are what raygui's per-control
// GuiSetStyle actually exposes; there's deliberately no `radius`
// (rounded corners aren't achievable through raygui's stock controls at
// all — see graphics_render.c's applyDefaultTheme comment).
typedef struct {
    int      hasBackground;  GfxColor backgroundColor;
    int      hasTextColor;   GfxColor textColor;
    int      hasBorderColor; GfxColor borderColor;
    int      hasTextPadding; int      textPadding;
    int      hasFontSize;    int      fontSize;
} GfxStyle;

#define GFX_MAX_NAMED_STYLES        64
#define GFX_MAX_CLASSES_PER_WIDGET  8
#define GFX_STYLE_NAME_LEN          64

// A style registered by name via graphics.defineStyle()/styleSheet(),
// referenced from a widget via graphics.addClass() — see
// graphics_render.c's effectiveStyle for how a widget's classes (in
// addClass order) and its own inline style (from graphics.style())
// combine: each later one overrides whichever properties it sets,
// inline always applying last. Not a real CSS cascade — no selectors,
// specificity, or multiple-selector precedence — just "later wins",
// which covers the common case without that complexity.
typedef struct {
    int      used;
    char     name[GFX_STYLE_NAME_LEN];
    GfxStyle style;
} GfxNamedStyle;

extern GfxNamedStyle gfxNamedStyles[GFX_MAX_NAMED_STYLES];
extern int           gfxNamedStyleCount;

// A single node in the widget tree — a leaf control (label/button/input/
// checkbox/image) or a layout container (row/column). One flat struct
// covers both rather than a tagged union: at this scale (a few hundred
// widgets, max) the handful of fields that only apply to one kind or the
// other cost nothing real, and it keeps every registry access uniform
// rather than needing a second array type. Which fields matter depends
// on `kind` — see the per-field comments.
typedef struct {
    int           used;
    GfxWidgetKind kind;

    // label/button/input/checkbox: their displayed text (checkbox's is
    // the label drawn next to the box). image: its file path.
    // radioGroup/dropdown: their options, newline-joined for radioGroup
    // (matching GuiToggleGroup's own expected format) or semicolon-joined
    // for dropdown (matching GuiDropdownBox's). tabs: its tab labels,
    // newline-joined. Unused by row/column/canvas.
    char          text[GFX_MAX_TEXT];

    // button only. Also doubles as canvas's onDraw callback — a canvas
    // has no click to speak of, so there's no ambiguity in reusing the
    // same pair of fields for "the one callback this kind of widget
    // has", same reasoning as onChange below.
    int           hasOnClick;
    Value         onClick;

    // checkbox only. Also doubles as slider/radioGroup/dropdown/tabs'
    // onChange — same "one callback, no ambiguity in sharing the field"
    // reasoning as onClick/canvas above.
    int           checked;
    int           hasOnChange;
    Value         onChange;

    // slider/progressBar only. Also doubles as canvas's fixed
    // width/height (minValue/maxValue unused there) — a canvas needs an
    // explicit pixel size for its coordinate space to mean anything, the
    // same way a slider needs an explicit numeric range.
    float         minValue, maxValue, currentValue;

    // radioGroup/dropdown/tabs: `selectedIndex` is which option/tab is
    // currently active; `onChange` (shared with checkbox, above) fires
    // when it changes. Options/labels are stored in `text` above (see
    // its comment for which separator each kind uses).
    int           selectedIndex;

    // row/column: children, in add-order, plus this container's own
    // spacing. tabs: reuses childHandles/childCount identically, but for
    // *panels* (one per tab, index-corresponding to the labels in
    // `text`) rather than a flat stack — only the panel at
    // childHandles[selectedIndex] is ever drawn. Windows have the
    // identical shape (childHandles/childCount/gap/padding) but as plain
    // GfxWindow fields rather than a GfxWidget, since a window isn't
    // itself a node any other container can hold — see GfxWindow below
    // and graphics_render.c's layoutChildren, which both a window's
    // top-level children and a real GFX_WIDGET_COLUMN/GFX_WIDGET_ROW's
    // children go through.
    int           childHandles[GFX_MAX_CHILDREN];
    int           childCount;
    int           gap;
    int           padding;

    // Phase 3 styling — see GfxStyle's comment. `style` is this widget's
    // own inline overrides (graphics.style()); `classHandles` are
    // indices into gfxNamedStyles this widget has via graphics.addClass(),
    // in the order they were added.
    GfxStyle      style;
    int           classHandles[GFX_MAX_CLASSES_PER_WIDGET];
    int           classCount;
} GfxWidget;

// A window and the top-level nodes added to it, in add() order — laid
// out exactly like a column (see graphics_render.c's layoutChildren),
// using gap/padding same as any other column would.
typedef struct {
    int  used;
    char title[GFX_MAX_TEXT];
    int  width, height;
    int  childHandles[GFX_MAX_CHILDREN];
    int  childCount;
    int  gap;     // spacing between top-level children
    int  padding; // inner margin from the window's edges
    int  focusedInput; // index into gfxWidgets of the currently-focused
                       // GFX_WIDGET_INPUT, or -1 — see graphics_render.c;
                       // only one input can hold keyboard focus at a time
    int  openDropdown; // index into gfxWidgets of the currently-open
                       // GFX_WIDGET_DROPDOWN, or -1 — see
                       // graphics_render.c's gfxRunEventLoop for why an
                       // open dropdown needs special handling (it has to
                       // draw after, and on top of, everything else in
                       // the window, and lock every other control while
                       // open so a click landing "through" it doesn't
                       // also activate whatever's visually underneath)
} GfxWindow;

extern GfxWindow gfxWindows[GFX_MAX_WINDOWS];
extern int       gfxWindowCount;
extern GfxWidget gfxWidgets[GFX_MAX_WIDGETS];
extern int       gfxWidgetCount;

// Builds a handle map {"__type": type, "__handle": handle} — defined in
// module_native_graphics.c, exposed here because graphics_render.c needs
// it too: an onClick/onChange callback is invoked with the widget's own
// handle as its one argument (see graphics_render.c's gfxRunEventLoop),
// so the handler has *something* to act on — Nova functions can't see
// any outer variable, including the very widget they're attached to, so
// without this a handler would have no way to touch anything.
Value gfxMakeHandle(const char* type, int handle);

// Runs the blocking raylib event loop for gfxWindows[windowHandle] —
// implemented in graphics_render.c. Called by the graphics.show() native
// in module_native_graphics.c once the handle has been validated, so
// this can assume windowHandle is valid. Needs `vm` to call back into
// Nova (via callFunctionValue, see vm.h) when a button's onClick or a
// checkbox's onChange fires.
// Forward-declared as `struct VM*` rather than the `VM` typedef here to
// avoid redeclaring that typedef in a second header — every caller
// already has vm.h's real declaration in scope too.
struct VM;
void gfxRunEventLoop(struct VM* vm, int windowHandle);

// --- canvas drawing primitives ---------------------------------------
//
// graphics.canvasLine/Rect/Circle/Text (module_native_graphics.c) are
// thin, raylib-agnostic wrappers: they validate arguments, then call
// straight through to these — the only place in the module besides
// graphics_render.c's own internals that touches anything resembling
// drawing. Coordinates are relative to the calling canvas's own
// top-left corner (0,0), not the screen — see graphics_render.c's
// gfxCanvasDraw for how that translation happens. Every one of these is
// only valid to call while gfxIsDrawingActive() is true (i.e. from
// inside a canvas's onDraw callback, mid-frame) — module_native_graphics.c
// checks that before ever calling through to one of these, so a script
// calling graphics.canvasLine() outside of onDraw gets a clean Nova
// error instead of this silently doing nothing or crashing.
int  gfxIsDrawingActive(void);
void gfxCanvasLine(float x1, float y1, float x2, float y2, float thickness, GfxColor color);
void gfxCanvasRect(float x, float y, float w, float h, GfxColor color);
void gfxCanvasCircle(float x, float y, float radius, GfxColor color);
void gfxCanvasText(const char* text, float x, float y, int fontSize, GfxColor color);

#endif
