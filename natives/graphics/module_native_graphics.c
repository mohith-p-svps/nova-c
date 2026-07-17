// The `graphics` module — Phase 2 complete: the full basic widget set
// (label, button, input, checkbox, image), row()/column() layout
// containers, and handle validation that's actually safe against
// tampered/out-of-range handles. See the project roadmap doc for the
// full multi-phase design; Phase 3 (CSS-flavored styling on top of
// what's here) is next.
//
// Nova has no "object" value type (see value.h's ValueType — just
// bool/int/float/string/array/map/function), so a window or widget is
// represented as an ordinary Nova map carrying two reserved keys:
//     {"__type": "window" | "label" | "button" | "input" | "checkbox"
//               | "image" | "row" | "column", "__handle": N}
// where N indexes into one of the registries below. Nova code can hold
// these maps in variables, pass them around, etc. like any other value —
// the reserved keys just aren't meant to be read or written by hand, the
// same way arr/map internals aren't meant to be poked at directly
// elsewhere in this codebase. Since Nova maps are plain data, nothing
// stops a script from hand-building something that *looks* like a
// handle ({"__type": "window", "__handle": 999}); every check in this
// file range-checks the handle number against the registry it claims to
// belong to, not just its shape, so a bogus handle is a clean Nova
// error rather than an out-of-bounds C array read.
//
// This file only ever *creates* and *validates* handles. The actual
// raylib/raygui drawing, layout, and event loop lives in
// graphics_render.c — the one file in this module that includes
// raylib.h/raygui.h — so this file (and everything Nova-facing about
// the module) compiles and can be unit-tested with zero dependency on
// raylib/raygui being installed at all.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "module_native_graphics.h"
#include "graphics_internal.h"
#include "../../vm.h"
#include "../../error.h"

GfxWindow gfxWindows[GFX_MAX_WINDOWS];
int       gfxWindowCount = 0;
GfxWidget gfxWidgets[GFX_MAX_WIDGETS];
int       gfxWidgetCount = 0;
GfxNamedStyle gfxNamedStyles[GFX_MAX_NAMED_STYLES];
int           gfxNamedStyleCount = 0;

// --- small generic helpers ----------------------------------------------

// True if `v` is a VAL_STRING whose content exactly equals `lit`.
static int stringValueIs(Value v, const char* lit) {
    return v.type == VAL_STRING &&
           (int)strlen(lit) == v.as.string->length &&
           memcmp(lit, v.as.string->data, v.as.string->length) == 0;
}

static void copyTextInto(char* dest, int destSize, NovaString* src) {
    int n = src->length;
    if (n >= destSize) n = destSize - 1;
    memcpy(dest, src->data, n);
    dest[n] = '\0';
}

static int checkInt(Value v, const char* what, const char* fnName, int line, int64_t* out) {
    Value p = promoteToInt64(v);
    if (p.type != VAL_INT64) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: expected an int for %s (got %s)", fnName, what, typeName(v));
        return 0;
    }
    *out = p.as.i64;
    return 1;
}

// Accepts either an int or a float64 Nova value (sliders/progress bars
// take plain numbers, and there's no reason to force a script to write
// "0.0" instead of "0" just because the range happens to be whole
// numbers). Doesn't accept bigint/bigdecimal — a slider range being
// arbitrary-precision isn't a real use case, and supporting it would
// mean lossy narrowing to a float anyway.
static int checkFloat(Value v, const char* what, const char* fnName, int line, float* out) {
    if (v.type == VAL_FLOAT64) { *out = (float)v.as.f64; return 1; }
    Value p = promoteToInt64(v);
    if (p.type == VAL_INT64) { *out = (float)p.as.i64; return 1; }
    novaError(ERR_ARGUMENT, line, "graphics.%s: expected a number for %s (got %s)", fnName, what, typeName(v));
    return 0;
}

// --- style parsing (Phase 3) ---------------------------------------------
//
// Everything in this section is pure string/data parsing with zero
// raylib dependency — the actual *drawing* of a computed GfxStyle
// happens in graphics_render.c, which is the only place that needs to
// know what a raylib Color actually is.

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int ciEquals(const char* a, int aLen, const char* b) {
    int bLen = (int)strlen(b);
    if (aLen != bLen) return 0;
    for (int i = 0; i < aLen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

typedef struct { const char* name; unsigned char r, g, b, a; } NamedColor;
static const NamedColor NAMED_COLORS[] = {
    {"white",       255, 255, 255, 255},
    {"black",         0,   0,   0, 255},
    {"red",         220,  38,  38, 255},
    {"green",        22, 163,  74, 255},
    {"blue",         37,  99, 235, 255},
    {"gray",        107, 114, 128, 255},
    {"grey",        107, 114, 128, 255},
    {"transparent",   0,   0,   0,   0},
};
#define NAMED_COLOR_COUNT (int)(sizeof(NAMED_COLORS) / sizeof(NAMED_COLORS[0]))

// Parses a CSS-ish color: "#RGB", "#RRGGBB", "#RRGGBBAA" (case
// insensitive hex digits), or one of a small set of named colors
// (white, black, red, green, blue, gray/grey, transparent). Not the
// full CSS color grammar — no rgb()/hsl()/etc. — just enough for a
// style's background/color/border values. Returns 1 and fills *out on
// success, 0 otherwise (raising nothing itself; callers decide how to
// report a bad color, since the right property name to mention varies).
static int parseColorString(const char* s, int len, GfxColor* out) {
    if (len > 0 && s[0] == '#') {
        if (len == 4) { // #RGB — each digit doubled, e.g. "#4F6" -> "#44FF66"
            int r = hexDigit(s[1]), g = hexDigit(s[2]), b = hexDigit(s[3]);
            if (r < 0 || g < 0 || b < 0) return 0;
            out->r = (unsigned char)(r * 17);
            out->g = (unsigned char)(g * 17);
            out->b = (unsigned char)(b * 17);
            out->a = 255;
            return 1;
        }
        if (len == 7 || len == 9) { // #RRGGBB or #RRGGBBAA
            int vals[4] = { 255, 255, 255, 255 };
            int n = (len - 1) / 2;
            for (int i = 0; i < n; i++) {
                int hi = hexDigit(s[1 + i * 2]), lo = hexDigit(s[2 + i * 2]);
                if (hi < 0 || lo < 0) return 0;
                vals[i] = hi * 16 + lo;
            }
            out->r = (unsigned char)vals[0];
            out->g = (unsigned char)vals[1];
            out->b = (unsigned char)vals[2];
            out->a = (unsigned char)vals[3];
            return 1;
        }
        return 0;
    }
    for (int i = 0; i < NAMED_COLOR_COUNT; i++) {
        if (ciEquals(s, len, NAMED_COLORS[i].name)) {
            out->r = NAMED_COLORS[i].r; out->g = NAMED_COLORS[i].g;
            out->b = NAMED_COLORS[i].b; out->a = NAMED_COLORS[i].a;
            return 1;
        }
    }
    return 0;
}

static const char* STYLE_KEYS[] = { "background", "color", "padding", "border", "fontSize" };
#define STYLE_KEY_COUNT (int)(sizeof(STYLE_KEYS) / sizeof(STYLE_KEYS[0]))

static int isKnownStyleKey(Value keyVal) {
    for (int i = 0; i < STYLE_KEY_COUNT; i++)
        if (stringValueIs(keyVal, STYLE_KEYS[i])) return 1;
    return 0;
}

// Reads background/color/border/padding/fontSize out of a Nova style
// map into `style`, merging onto whatever it already holds (a property
// the map doesn't mention is left untouched — this is what lets
// graphics.style() be called more than once and have the calls add up
// rather than the later one wiping out the earlier one). Validates every
// key up front before applying anything, so a map with one bad property
// doesn't leave a style half-applied. Shared by graphics.style() (writes
// straight into a widget) and graphics.defineStyle() (writes into a new
// named style) — see their comments.
static int fillStyleFromMap(GfxStyle* style, NovaMap* m, const char* fnName, int line) {
    for (int i = 0; i < m->count; i++) {
        if (m->keys[i].type != VAL_STRING) {
            novaError(ERR_ARGUMENT, line, "graphics.%s: style map keys must be strings", fnName);
            return 0;
        }
        if (!isKnownStyleKey(m->keys[i])) {
            novaError(ERR_ARGUMENT, line,
                      "graphics.%s: '%.*s' is not a supported style property — supported: background, color, padding, border, fontSize "
                      "(rounded corners aren't supported by the underlying renderer)",
                      fnName, m->keys[i].as.string->length, m->keys[i].as.string->data);
            return 0;
        }
    }

    Value v;
    if (mapGet(m, makeString("background", 10), &v)) {
        if (v.type != VAL_STRING || !parseColorString(v.as.string->data, v.as.string->length, &style->backgroundColor)) {
            novaError(ERR_ARGUMENT, line, "graphics.%s: 'background' must be a color like \"#4F46E5\" or a name like \"white\"", fnName);
            return 0;
        }
        style->hasBackground = 1;
    }
    if (mapGet(m, makeString("color", 5), &v)) {
        if (v.type != VAL_STRING || !parseColorString(v.as.string->data, v.as.string->length, &style->textColor)) {
            novaError(ERR_ARGUMENT, line, "graphics.%s: 'color' must be a color like \"#4F46E5\" or a name like \"white\"", fnName);
            return 0;
        }
        style->hasTextColor = 1;
    }
    if (mapGet(m, makeString("border", 6), &v)) {
        if (v.type != VAL_STRING || !parseColorString(v.as.string->data, v.as.string->length, &style->borderColor)) {
            novaError(ERR_ARGUMENT, line, "graphics.%s: 'border' must be a color like \"#4F46E5\" or a name like \"white\"", fnName);
            return 0;
        }
        style->hasBorderColor = 1;
    }
    if (mapGet(m, makeString("padding", 7), &v)) {
        int64_t p;
        if (!checkInt(v, "padding", fnName, line, &p)) return 0;
        style->textPadding = (int)p;
        style->hasTextPadding = 1;
    }
    if (mapGet(m, makeString("fontSize", 8), &v)) {
        int64_t fs;
        if (!checkInt(v, "fontSize", fnName, line, &fs)) return 0;
        style->fontSize = (int)fs;
        style->hasFontSize = 1;
    }
    return 1;
}

// Registers `style` under `name` in the shared named-style registry —
// updates it in place if that name is already defined (re-running
// styleSheet()/defineStyle() with the same class name is expected to
// redefine it, not error out or pile up duplicates).
static int registerNamedStyle(const char* name, const GfxStyle* style, const char* fnName, int line) {
    for (int i = 0; i < gfxNamedStyleCount; i++) {
        if (gfxNamedStyles[i].used && strcmp(gfxNamedStyles[i].name, name) == 0) {
            gfxNamedStyles[i].style = *style;
            return 1;
        }
    }
    if (gfxNamedStyleCount >= GFX_MAX_NAMED_STYLES) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: too many named styles defined (max %d)", fnName, GFX_MAX_NAMED_STYLES);
        return 0;
    }
    int idx = gfxNamedStyleCount++;
    gfxNamedStyles[idx].used = 1;
    snprintf(gfxNamedStyles[idx].name, GFX_STYLE_NAME_LEN, "%s", name);
    gfxNamedStyles[idx].style = *style;
    return 1;
}

// --- handle map helpers -----------------------------------------------

Value gfxMakeHandle(const char* type, int handle) {
    Value m = makeMap();
    mapSet(m.as.map, makeString("__type", 6), makeString(type, (int)strlen(type)));
    mapSet(m.as.map, makeString("__handle", 8), makeInt64(handle));
    return m;
}

// Silent check: does `v` look like a graphics handle map of the given
// `type`, AND does its handle number actually refer to a slot the
// graphics module created (in range, and not — hypothetically, once a
// close/destroy operation exists — a freed one)? No error is raised
// either way — this is a pure predicate, used so callers that accept
// more than one handle type can try each possibility without printing
// spurious "wrong type" noise for the ones that don't match (novaError()
// prints to stderr the moment it's called, not lazily).
static int handleIsType(Value v, const char* type, int* outHandle) {
    if (v.type != VAL_MAP) return 0;
    Value typeVal, handleVal;
    if (!mapGet(v.as.map, makeString("__type", 6), &typeVal))     return 0;
    if (!mapGet(v.as.map, makeString("__handle", 8), &handleVal)) return 0;
    if (!stringValueIs(typeVal, type)) return 0;
    if (handleVal.type != VAL_INT64) return 0;

    int h = (int)handleVal.as.i64;
    if (strcmp(type, "window") == 0) {
        if (h < 0 || h >= gfxWindowCount || !gfxWindows[h].used) return 0;
    } else {
        if (h < 0 || h >= gfxWidgetCount || !gfxWidgets[h].used) return 0;
    }
    *outHandle = h;
    return 1;
}

static int handleIsAnyOf(Value v, const char** types, int typeCount, int* outHandle) {
    for (int i = 0; i < typeCount; i++)
        if (handleIsType(v, types[i], outHandle)) return 1;
    return 0;
}

static int isAnyOfTag(Value typeVal, const char** types, int typeCount) {
    for (int i = 0; i < typeCount; i++)
        if (stringValueIs(typeVal, types[i])) return 1;
    return 0;
}

// Composes the error for a failed handle check against one or more
// acceptable `types` — distinguishes "wrong kind of value entirely"
// from "carries a __type tag we recognize, but the handle number
// doesn't refer to anything real" (a hand-built or corrupted handle
// map), since those deserve different messages. `expectedDesc` is what
// goes in the "expected a ___" part of the message (e.g. "window", or
// "widget" when types has more than one entry).
static void reportBadHandle(Value v, const char** types, int typeCount, const char* expectedDesc, const char* fnName, int line) {
    Value typeVal;
    if (v.type == VAL_MAP && mapGet(v.as.map, makeString("__type", 6), &typeVal) && typeVal.type == VAL_STRING) {
        if (isAnyOfTag(typeVal, types, typeCount)) {
            novaError(ERR_ARGUMENT, line, "graphics.%s: not a valid %s (this handle wasn't created by the graphics module)", fnName, expectedDesc);
        } else {
            novaError(ERR_ARGUMENT, line, "graphics.%s: expected a %s, got a %.*s",
                      fnName, expectedDesc, typeVal.as.string->length, typeVal.as.string->data);
        }
        return;
    }
    novaError(ERR_ARGUMENT, line, "graphics.%s: expected a %s (got %s)", fnName, expectedDesc, typeName(v));
}

static const char* ANY_WIDGET_TYPES[]     = {"label", "button", "input", "checkbox", "image", "slider",
                                              "progressBar", "radioGroup", "dropdown", "tabs", "canvas", "row", "column"};
#define ANY_WIDGET_TYPE_COUNT 13
static const char* TEXT_BEARING_TYPES[]   = {"label", "button", "input", "checkbox"};
#define TEXT_BEARING_TYPE_COUNT 4
static const char* VALUE_BEARING_TYPES[]  = {"slider", "progressBar"};
#define VALUE_BEARING_TYPE_COUNT 2
static const char* CHANGE_CAPABLE_TYPES[] = {"checkbox", "slider", "radioGroup", "dropdown", "tabs"};
#define CHANGE_CAPABLE_TYPE_COUNT 5
static const char* SELECTABLE_TYPES[]     = {"radioGroup", "dropdown", "tabs"};
#define SELECTABLE_TYPE_COUNT 3

// radioGroup/tabs join their options with '\n' (matching GuiToggleGroup's
// own expected format); dropdown joins with ';' (matching GuiDropdownBox's).
// Centralized here so gfx_radioGroup/gfx_dropdown/gfx_tabs (construction)
// and gfx_getSelected/gfx_setSelected (reading the joined text back apart)
// can never disagree about which one a given widget uses.
static char optionSeparatorFor(GfxWidgetKind kind) {
    return (kind == GFX_WIDGET_DROPDOWN) ? ';' : '\n';
}

static int checkHandle(Value v, const char* type, const char* fnName, int line, int* outHandle) {
    if (handleIsType(v, type, outHandle)) return 1;
    const char* single[1] = { type };
    reportBadHandle(v, single, 1, type, fnName, line);
    return 0;
}

// Any of the 7 widget/container kinds — used where anything that can be
// a child of a window or another container is acceptable (add(), and
// row()/column()'s own non-array-element argument checks).
static int checkAnyWidgetHandle(Value v, const char* fnName, int line, int* outHandle) {
    if (handleIsAnyOf(v, ANY_WIDGET_TYPES, ANY_WIDGET_TYPE_COUNT, outHandle)) return 1;
    reportBadHandle(v, ANY_WIDGET_TYPES, ANY_WIDGET_TYPE_COUNT, "widget", fnName, line);
    return 0;
}

// label/button/input/checkbox — the kinds that have meaningful display
// text — used by getText/setText. Deliberately excludes image (its
// "text" is a file path fixed at creation time — see gfx_image) and
// row/column (no display text of their own).
static int checkTextBearingHandle(Value v, const char* fnName, int line, int* outHandle) {
    if (handleIsAnyOf(v, TEXT_BEARING_TYPES, TEXT_BEARING_TYPE_COUNT, outHandle)) return 1;
    reportBadHandle(v, TEXT_BEARING_TYPES, TEXT_BEARING_TYPE_COUNT, "label, button, input, or checkbox", fnName, line);
    return 0;
}

// slider/progressBar — used by getValue/setValue.
static int checkValueBearingHandle(Value v, const char* fnName, int line, int* outHandle) {
    if (handleIsAnyOf(v, VALUE_BEARING_TYPES, VALUE_BEARING_TYPE_COUNT, outHandle)) return 1;
    reportBadHandle(v, VALUE_BEARING_TYPES, VALUE_BEARING_TYPE_COUNT, "slider or progressBar", fnName, line);
    return 0;
}

// checkbox/slider/radioGroup/dropdown/tabs — the kinds that can fire onChange.
static int checkChangeCapableHandle(Value v, const char* fnName, int line, int* outHandle) {
    if (handleIsAnyOf(v, CHANGE_CAPABLE_TYPES, CHANGE_CAPABLE_TYPE_COUNT, outHandle)) return 1;
    reportBadHandle(v, CHANGE_CAPABLE_TYPES, CHANGE_CAPABLE_TYPE_COUNT, "checkbox, slider, radioGroup, dropdown, or tabs", fnName, line);
    return 0;
}

// radioGroup/dropdown/tabs — used by getSelected/setSelected.
static int checkSelectableHandle(Value v, const char* fnName, int line, int* outHandle) {
    if (handleIsAnyOf(v, SELECTABLE_TYPES, SELECTABLE_TYPE_COUNT, outHandle)) return 1;
    reportBadHandle(v, SELECTABLE_TYPES, SELECTABLE_TYPE_COUNT, "radioGroup, dropdown, or tabs", fnName, line);
    return 0;
}

// --- Nova-facing functions ----------------------------------------------

static Value gfx_window(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.window: expected a string title (got %s)", typeName(args[0]));
        return makeNull();
    }
    int64_t width, height;
    if (!checkInt(args[1], "width", "window", line, &width))  return makeNull();
    if (!checkInt(args[2], "height", "window", line, &height)) return makeNull();

    if (gfxWindowCount >= GFX_MAX_WINDOWS) {
        novaError(ERR_ARGUMENT, line, "graphics.window: too many windows open (max %d)", GFX_MAX_WINDOWS);
        return makeNull();
    }

    int idx = gfxWindowCount++;
    GfxWindow* w = &gfxWindows[idx];
    w->used = 1;
    copyTextInto(w->title, GFX_MAX_TEXT, args[0].as.string);
    w->width        = (int)width;
    w->height       = (int)height;
    w->childCount   = 0;
    w->gap          = 10;
    w->padding      = 16;
    w->focusedInput = -1;
    w->openDropdown = -1;

    return gfxMakeHandle("window", idx);
}

// Shared constructor for every leaf widget kind (label/button/input/
// checkbox/image) — they differ only in what `text` means (display
// text vs. a file path) and which extra fields get initialized, all
// handled by the specific gfx_label/gfx_button/etc. wrappers below.
static Value gfx_makeWidget(GfxWidgetKind kind, Value textArg, const char* fnName, int line) {
    if (textArg.type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: expected a string (got %s)", fnName, typeName(textArg));
        return makeNull();
    }
    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: too many widgets created (max %d)", fnName, GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = kind;
    copyTextInto(wd->text, GFX_MAX_TEXT, textArg.as.string);
    wd->hasOnClick  = 0; wd->onClick  = makeNull();
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked     = 0;
    wd->childCount  = 0;
    wd->gap = 0; wd->padding = 0;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;

    const char* tag =
        kind == GFX_WIDGET_LABEL    ? "label"    :
        kind == GFX_WIDGET_BUTTON   ? "button"   :
        kind == GFX_WIDGET_INPUT    ? "input"    :
        kind == GFX_WIDGET_CHECKBOX ? "checkbox" : "image";
    return gfxMakeHandle(tag, idx);
}

static Value gfx_label(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    return gfx_makeWidget(GFX_WIDGET_LABEL, args[0], "label", line);
}

static Value gfx_button(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    return gfx_makeWidget(GFX_WIDGET_BUTTON, args[0], "button", line);
}

static Value gfx_input(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    return gfx_makeWidget(GFX_WIDGET_INPUT, args[0], "input", line);
}

static Value gfx_checkbox(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    return gfx_makeWidget(GFX_WIDGET_CHECKBOX, args[0], "checkbox", line);
}

// The image file itself is NOT loaded here — no window/OpenGL context
// necessarily exists yet at the point a Nova script constructs widgets
// (graphics.image() typically runs before graphics.show()). The path is
// just stored; graphics_render.c loads the actual texture once, right
// after InitWindow(), the first time the window that contains it is shown.
static Value gfx_image(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    return gfx_makeWidget(GFX_WIDGET_IMAGE, args[0], "image", line);
}

static Value gfx_slider(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    float minV, maxV, initV;
    if (!checkFloat(args[0], "min", "slider", line, &minV)) return makeNull();
    if (!checkFloat(args[1], "max", "slider", line, &maxV)) return makeNull();
    if (!checkFloat(args[2], "initial", "slider", line, &initV)) return makeNull();
    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.slider: too many widgets created (max %d)", GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = GFX_WIDGET_SLIDER;
    wd->text[0] = '\0';
    wd->hasOnClick = 0; wd->onClick = makeNull();
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked = 0;
    wd->childCount = 0; wd->gap = 0; wd->padding = 0;
    wd->minValue = minV; wd->maxValue = maxV; wd->currentValue = initV;
    wd->selectedIndex = 0;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;
    return gfxMakeHandle("slider", idx);
}

static Value gfx_progressBar(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    float minV, maxV, initV;
    if (!checkFloat(args[0], "min", "progressBar", line, &minV)) return makeNull();
    if (!checkFloat(args[1], "max", "progressBar", line, &maxV)) return makeNull();
    if (!checkFloat(args[2], "initial", "progressBar", line, &initV)) return makeNull();
    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.progressBar: too many widgets created (max %d)", GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = GFX_WIDGET_PROGRESSBAR;
    wd->text[0] = '\0';
    wd->hasOnClick = 0; wd->onClick = makeNull();
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked = 0;
    wd->childCount = 0; wd->gap = 0; wd->padding = 0;
    wd->minValue = minV; wd->maxValue = maxV; wd->currentValue = initV;
    wd->selectedIndex = 0;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;
    return gfxMakeHandle("progressBar", idx);
}

// Joins `arr`'s string elements with `sep` into `outBuf` (max
// `outBufSize` bytes including the terminator). If `initial` is
// non-NULL, also finds which joined element matches it exactly (a full
// string match, not a substring one) and reports that index — used by
// gfx_radioGroup/gfx_dropdown, which need to validate their initial
// selection against the options given; gfx_tabs passes NULL since its
// initial selection is validated differently (against a separate array
// of panel handles the same length as the labels, not by matching text).
// Returns 0 (raising a novaError itself, naming `fnName`) on any
// problem: a non-string element, or the joined text not fitting.
static int joinOptions(NovaArray* arr, char sep, Value* initial, char* outBuf, int outBufSize,
                        int* outLen, int* outInitialIndex, const char* fnName, int line) {
    int pos = 0;
    int initialIndex = -1;
    for (int i = 0; i < arr->count; i++) {
        if (arr->items[i].type != VAL_STRING) {
            novaError(ERR_ARGUMENT, line, "graphics.%s: option %d is not a string (got %s)", fnName, i, typeName(arr->items[i]));
            return 0;
        }
        NovaString* opt = arr->items[i].as.string;
        if (initial && opt->length == initial->as.string->length &&
            memcmp(opt->data, initial->as.string->data, opt->length) == 0) {
            initialIndex = i;
        }
        int n = opt->length;
        if (pos + n + 1 >= outBufSize) {
            novaError(ERR_ARGUMENT, line, "graphics.%s: options are too long to fit (max %d characters combined)", fnName, outBufSize);
            return 0;
        }
        memcpy(outBuf + pos, opt->data, n);
        pos += n;
        if (i < arr->count - 1) outBuf[pos++] = sep;
    }
    outBuf[pos] = '\0';
    *outLen = pos;
    if (outInitialIndex) *outInitialIndex = initialIndex;
    return 1;
}

// Options are stored newline-joined in the widget's `text` field — the
// exact format raygui's GuiToggleGroup itself expects (see
// graphics_render.c), so there's nothing extra to convert at draw time.
static Value gfx_radioGroup(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_ARRAY) {
        novaError(ERR_ARGUMENT, line, "graphics.radioGroup: expected an array of option strings (got %s)", typeName(args[0]));
        return makeNull();
    }
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.radioGroup: expected a string for the initial selection (got %s)", typeName(args[1]));
        return makeNull();
    }
    NovaArray* arr = args[0].as.array;
    if (arr->count == 0) {
        novaError(ERR_ARGUMENT, line, "graphics.radioGroup: needs at least one option");
        return makeNull();
    }

    char joined[GFX_MAX_TEXT];
    int joinedLen, initialIndex;
    if (!joinOptions(arr, '\n', &args[1], joined, sizeof(joined), &joinedLen, &initialIndex, "radioGroup", line)) return makeNull();
    if (initialIndex == -1) {
        novaError(ERR_ARGUMENT, line, "graphics.radioGroup: the initial selection doesn't match any of the given options");
        return makeNull();
    }

    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.radioGroup: too many widgets created (max %d)", GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = GFX_WIDGET_RADIOGROUP;
    memcpy(wd->text, joined, joinedLen + 1);
    wd->hasOnClick = 0; wd->onClick = makeNull();
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked = 0;
    wd->childCount = 0; wd->gap = 0; wd->padding = 0;
    wd->selectedIndex = initialIndex;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;
    return gfxMakeHandle("radioGroup", idx);
}

// Same idea as radioGroup, but options are semicolon-joined — GuiDropdownBox's
// expected format, not GuiToggleGroup's (see optionSeparatorFor).
static Value gfx_dropdown(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_ARRAY) {
        novaError(ERR_ARGUMENT, line, "graphics.dropdown: expected an array of option strings (got %s)", typeName(args[0]));
        return makeNull();
    }
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.dropdown: expected a string for the initial selection (got %s)", typeName(args[1]));
        return makeNull();
    }
    NovaArray* arr = args[0].as.array;
    if (arr->count == 0) {
        novaError(ERR_ARGUMENT, line, "graphics.dropdown: needs at least one option");
        return makeNull();
    }

    char joined[GFX_MAX_TEXT];
    int joinedLen, initialIndex;
    if (!joinOptions(arr, ';', &args[1], joined, sizeof(joined), &joinedLen, &initialIndex, "dropdown", line)) return makeNull();
    if (initialIndex == -1) {
        novaError(ERR_ARGUMENT, line, "graphics.dropdown: the initial selection doesn't match any of the given options");
        return makeNull();
    }

    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.dropdown: too many widgets created (max %d)", GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = GFX_WIDGET_DROPDOWN;
    memcpy(wd->text, joined, joinedLen + 1);
    wd->hasOnClick = 0; wd->onClick = makeNull();
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked = 0;
    wd->childCount = 0; wd->gap = 0; wd->padding = 0;
    wd->selectedIndex = initialIndex;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;
    return gfxMakeHandle("dropdown", idx);
}

// `panels` must be the same length as `labels` — panels[i] is what's
// shown when labels[i] is the active tab. Only ever one panel is drawn
// at a time (see graphics_render.c) — this is what makes tabs different
// from a plain row of buttons: switching tabs swaps out entire subtrees
// of content, not just which button looks pressed.
static Value gfx_tabs(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_ARRAY) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: expected an array of label strings (got %s)", typeName(args[0]));
        return makeNull();
    }
    if (args[1].type != VAL_ARRAY) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: expected an array of panel widgets (got %s)", typeName(args[1]));
        return makeNull();
    }
    if (args[2].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: expected a string for the initial tab (got %s)", typeName(args[2]));
        return makeNull();
    }
    NovaArray* labels = args[0].as.array;
    NovaArray* panels = args[1].as.array;
    if (labels->count == 0) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: needs at least one tab");
        return makeNull();
    }
    if (labels->count != panels->count) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: labels and panels must be the same length (got %d labels, %d panels)",
                  labels->count, panels->count);
        return makeNull();
    }
    if (labels->count > GFX_MAX_CHILDREN) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: too many tabs (max %d)", GFX_MAX_CHILDREN);
        return makeNull();
    }

    char joined[GFX_MAX_TEXT];
    int joinedLen, initialIndex;
    if (!joinOptions(labels, '\n', &args[2], joined, sizeof(joined), &joinedLen, &initialIndex, "tabs", line)) return makeNull();
    if (initialIndex == -1) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: the initial tab doesn't match any of the given labels");
        return makeNull();
    }

    int panelHandles[GFX_MAX_CHILDREN];
    for (int i = 0; i < panels->count; i++) {
        if (!checkAnyWidgetHandle(panels->items[i], "tabs", line, &panelHandles[i])) return makeNull();
    }

    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.tabs: too many widgets created (max %d)", GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = GFX_WIDGET_TABS;
    memcpy(wd->text, joined, joinedLen + 1);
    wd->hasOnClick = 0; wd->onClick = makeNull();
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked = 0;
    wd->childCount = panels->count;
    memcpy(wd->childHandles, panelHandles, sizeof(int) * panels->count);
    wd->gap = 0; wd->padding = 0;
    wd->selectedIndex = initialIndex;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;
    return gfxMakeHandle("tabs", idx);
}

static Value gfx_onClick(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkHandle(args[0], "button", "onClick", line, &handle)) return makeNull();
    if (args[1].type != VAL_FUNCTION) {
        novaError(ERR_ARGUMENT, line, "graphics.onClick: expected a function (got %s)", typeName(args[1]));
        return makeNull();
    }
    GfxWidget* wd = &gfxWidgets[handle];
    if (wd->hasOnClick) freeValue(wd->onClick);
    wd->onClick = copyValue(args[1]);
    wd->hasOnClick = 1;
    return makeNull();
}

static Value gfx_onChange(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkChangeCapableHandle(args[0], "onChange", line, &handle)) return makeNull();
    if (args[1].type != VAL_FUNCTION) {
        novaError(ERR_ARGUMENT, line, "graphics.onChange: expected a function (got %s)", typeName(args[1]));
        return makeNull();
    }
    GfxWidget* wd = &gfxWidgets[handle];
    if (wd->hasOnChange) freeValue(wd->onChange);
    wd->onChange = copyValue(args[1]);
    wd->hasOnChange = 1;
    return makeNull();
}

static Value gfx_add(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int winHandle, widgetHandle;
    if (!checkHandle(args[0], "window", "add", line, &winHandle)) return makeNull();
    if (!checkAnyWidgetHandle(args[1], "add", line, &widgetHandle)) return makeNull();

    GfxWindow* w = &gfxWindows[winHandle];
    if (w->childCount >= GFX_MAX_CHILDREN) {
        novaError(ERR_ARGUMENT, line, "graphics.add: too many widgets in one window (max %d)", GFX_MAX_CHILDREN);
        return makeNull();
    }
    w->childHandles[w->childCount++] = widgetHandle;
    return makeNull();
}

// Updates a label/button/input/checkbox's displayed text after creation.
static Value gfx_setText(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkTextBearingHandle(args[0], "setText", line, &handle)) return makeNull();
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.setText: expected a string (got %s)", typeName(args[1]));
        return makeNull();
    }
    copyTextInto(gfxWidgets[handle].text, GFX_MAX_TEXT, args[1].as.string);
    return makeNull();
}

// Reads a label/button/input/checkbox's current displayed text — the
// counterpart to setText. A handler can't remember anything between
// clicks (no closures, and every call starts with fresh locals), so
// reading a widget's own current text back is how it finds out what
// state it's in — e.g. a toggle button deciding what to switch *to*
// based on what it currently says.
static Value gfx_getText(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkTextBearingHandle(args[0], "getText", line, &handle)) return makeNull();
    GfxWidget* wd = &gfxWidgets[handle];
    return makeString(wd->text, (int)strlen(wd->text));
}

static Value gfx_isChecked(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkHandle(args[0], "checkbox", "isChecked", line, &handle)) return makeNull();
    return makeBool(gfxWidgets[handle].checked);
}

static Value gfx_setChecked(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkHandle(args[0], "checkbox", "setChecked", line, &handle)) return makeNull();
    if (args[1].type != VAL_BOOL) {
        novaError(ERR_ARGUMENT, line, "graphics.setChecked: expected a bool (got %s)", typeName(args[1]));
        return makeNull();
    }
    gfxWidgets[handle].checked = args[1].as.boolean;
    return makeNull();
}

// slider/progressBar's current value.
static Value gfx_getValue(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkValueBearingHandle(args[0], "getValue", line, &handle)) return makeNull();
    return makeFloat64((double)gfxWidgets[handle].currentValue);
}

static Value gfx_setValue(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkValueBearingHandle(args[0], "setValue", line, &handle)) return makeNull();
    float v;
    if (!checkFloat(args[1], "value", "setValue", line, &v)) return makeNull();
    GfxWidget* wd = &gfxWidgets[handle];
    if (v < wd->minValue) v = wd->minValue;
    if (v > wd->maxValue) v = wd->maxValue;
    wd->currentValue = v;
    return makeNull();
}

// radioGroup/dropdown's currently-selected option, or tabs' active
// tab's label — as the text itself, not its index — consistent with
// getText/setText elsewhere: reading a widget's own current state back
// as the same kind of value you'd use to set it.
static Value gfx_getSelected(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkSelectableHandle(args[0], "getSelected", line, &handle)) return makeNull();
    GfxWidget* wd = &gfxWidgets[handle];
    char sep = optionSeparatorFor(wd->kind);
    // wd->text is the options/labels joined by `sep` — walk to the
    // selected one.
    const char* p = wd->text;
    int idx = 0;
    while (idx < wd->selectedIndex) {
        while (*p && *p != sep) p++;
        if (*p == sep) p++;
        idx++;
    }
    const char* end = p;
    while (*end && *end != sep) end++;
    return makeString(p, (int)(end - p));
}

static Value gfx_setSelected(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkSelectableHandle(args[0], "setSelected", line, &handle)) return makeNull();
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.setSelected: expected a string (got %s)", typeName(args[1]));
        return makeNull();
    }
    GfxWidget* wd = &gfxWidgets[handle];
    char sep = optionSeparatorFor(wd->kind);
    const char* p = wd->text;
    int idx = 0;
    while (*p) {
        const char* end = p;
        while (*end && *end != sep) end++;
        int len = (int)(end - p);
        if (len == args[1].as.string->length && memcmp(p, args[1].as.string->data, len) == 0) {
            wd->selectedIndex = idx;
            return makeNull();
        }
        p = (*end == sep) ? end + 1 : end;
        idx++;
    }
    novaError(ERR_ARGUMENT, line, "graphics.setSelected: '%.*s' doesn't match any of this widget's options",
              args[1].as.string->length, args[1].as.string->data);
    return makeNull();
}

// Shared constructor for row()/column() — identical shape, differing
// only in which layout axis graphics_render.c splits along (see its
// layoutChildren), so both natives just call this with a
// different `kind`.
static Value gfx_makeContainer(GfxWidgetKind kind, Value childrenArg, Value optionsArg, const char* fnName, int line) {
    if (childrenArg.type != VAL_ARRAY) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: expected an array of children (got %s)", fnName, typeName(childrenArg));
        return makeNull();
    }
    if (optionsArg.type != VAL_MAP) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: expected an options map (got %s) — pass {} if you don't need gap/padding", fnName, typeName(optionsArg));
        return makeNull();
    }

    NovaArray* arr = childrenArg.as.array;
    if (arr->count > GFX_MAX_CHILDREN) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: too many children (max %d)", fnName, GFX_MAX_CHILDREN);
        return makeNull();
    }

    int childHandles[GFX_MAX_CHILDREN];
    for (int i = 0; i < arr->count; i++) {
        Value cv = arr->items[i];
        int h;
        if (handleIsAnyOf(cv, ANY_WIDGET_TYPES, ANY_WIDGET_TYPE_COUNT, &h)) {
            childHandles[i] = h;
            continue;
        }
        // Same diagnosis reportBadHandle does, with the child's index
        // folded into the message — worth the small duplication here
        // rather than bolting an optional index parameter onto every
        // reportBadHandle call site for the sake of this one caller.
        Value typeVal;
        if (cv.type == VAL_MAP && mapGet(cv.as.map, makeString("__type", 6), &typeVal) && typeVal.type == VAL_STRING) {
            if (isAnyOfTag(typeVal, ANY_WIDGET_TYPES, ANY_WIDGET_TYPE_COUNT)) {
                novaError(ERR_ARGUMENT, line, "graphics.%s: child %d is not a valid widget (this handle wasn't created by the graphics module)", fnName, i);
            } else {
                novaError(ERR_ARGUMENT, line, "graphics.%s: child %d — expected a widget, got a %.*s", fnName, i, typeVal.as.string->length, typeVal.as.string->data);
            }
        } else {
            novaError(ERR_ARGUMENT, line, "graphics.%s: child %d is not a widget (got %s)", fnName, i, typeName(cv));
        }
        return makeNull();
    }

    int gap = 8, padding = 0; // sensible defaults when the options map doesn't set them
    Value gapVal, paddingVal;
    if (mapGet(optionsArg.as.map, makeString("gap", 3), &gapVal)) {
        int64_t g;
        if (!checkInt(gapVal, "gap", fnName, line, &g)) return makeNull();
        gap = (int)g;
    }
    if (mapGet(optionsArg.as.map, makeString("padding", 7), &paddingVal)) {
        int64_t p;
        if (!checkInt(paddingVal, "padding", fnName, line, &p)) return makeNull();
        padding = (int)p;
    }

    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: too many widgets created (max %d)", fnName, GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = kind;
    wd->text[0] = '\0';
    wd->hasOnClick = 0; wd->onClick = makeNull();
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked = 0;
    wd->childCount = arr->count;
    memcpy(wd->childHandles, childHandles, sizeof(int) * arr->count);
    wd->gap = gap;
    wd->padding = padding;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;

    return gfxMakeHandle(kind == GFX_WIDGET_ROW ? "row" : "column", idx);
}

static Value gfx_row(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    return gfx_makeContainer(GFX_WIDGET_ROW, args[0], args[1], "row", line);
}

static Value gfx_column(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    return gfx_makeContainer(GFX_WIDGET_COLUMN, args[0], args[1], "column", line);
}

// Sets style overrides directly on one widget. Merges onto whatever
// that widget already has — calling this more than once adds up rather
// than the later call replacing the earlier one entirely.
static Value gfx_style(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkAnyWidgetHandle(args[0], "style", line, &handle)) return makeNull();
    if (args[1].type != VAL_MAP) {
        novaError(ERR_ARGUMENT, line, "graphics.style: expected a style map (got %s)", typeName(args[1]));
        return makeNull();
    }
    if (!fillStyleFromMap(&gfxWidgets[handle].style, args[1].as.map, "style", line)) return makeNull();
    return makeNull();
}

// Registers a reusable named style from a Nova map — see
// graphics.addClass() for applying it to widgets, and graphics.style()
// for setting overrides directly on one widget without a name.
static Value gfx_defineStyle(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.defineStyle: expected a string name (got %s)", typeName(args[0]));
        return makeNull();
    }
    if (args[1].type != VAL_MAP) {
        novaError(ERR_ARGUMENT, line, "graphics.defineStyle: expected a style map (got %s)", typeName(args[1]));
        return makeNull();
    }
    char name[GFX_STYLE_NAME_LEN];
    copyTextInto(name, sizeof(name), args[0].as.string);

    GfxStyle style;
    memset(&style, 0, sizeof(style));
    if (!fillStyleFromMap(&style, args[1].as.map, "defineStyle", line)) return makeNull();
    if (!registerNamedStyle(name, &style, "defineStyle", line)) return makeNull();
    return makeNull();
}

// Attaches a named style (defined via defineStyle or styleSheet) to a
// widget. A widget can have several classes; see graphics_render.c's
// effectiveStyle for how multiple classes and a widget's own inline
// style (from graphics.style()) combine.
static Value gfx_addClass(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkAnyWidgetHandle(args[0], "addClass", line, &handle)) return makeNull();
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.addClass: expected a string class name (got %s)", typeName(args[1]));
        return makeNull();
    }
    char name[GFX_STYLE_NAME_LEN];
    copyTextInto(name, sizeof(name), args[1].as.string);

    int styleIdx = -1;
    for (int i = 0; i < gfxNamedStyleCount; i++) {
        if (gfxNamedStyles[i].used && strcmp(gfxNamedStyles[i].name, name) == 0) { styleIdx = i; break; }
    }
    if (styleIdx == -1) {
        novaError(ERR_ARGUMENT, line, "graphics.addClass: no style named '%s' — define one first with graphics.defineStyle or graphics.styleSheet", name);
        return makeNull();
    }

    GfxWidget* wd = &gfxWidgets[handle];
    if (wd->classCount >= GFX_MAX_CLASSES_PER_WIDGET) {
        novaError(ERR_ARGUMENT, line, "graphics.addClass: too many classes on one widget (max %d)", GFX_MAX_CLASSES_PER_WIDGET);
        return makeNull();
    }
    wd->classHandles[wd->classCount++] = styleIdx;
    return makeNull();
}

// --- CSS-text style sheets (Phase 3c) -------------------------------------
//
// Parses a small, deliberately narrow subset of CSS: zero or more
// blocks of the form
//     .className { property: value; property: value; ... }
// registering each as a named style — a text front-end over the exact
// same registerNamedStyle() defineStyle() uses, not a second styling
// system. NOT a real CSS parser: one class-name selector only (no
// combinators, no pseudo-classes like :hover, no cascade/specificity,
// no !important, no comments, no nesting). An unrecognized property
// name inside a block is a parse error — same as fillStyleFromMap's map
// form — rather than being silently accepted and ignored, so CSS-
// looking text that only partially works never happens quietly.

static int isSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int isNameChar(char c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }

// Parses a plain decimal integer, optionally followed by a "px" unit
// (accepted and ignored — there's no unit system here, just a courtesy
// for CSS-looking numbers like "12px"). No other units are recognized.
static int parseIntLiteral(const char* s, int len, int* out) {
    int i = 0, sign = 1;
    if (i < len && s[i] == '-') { sign = -1; i++; }
    if (i >= len || s[i] < '0' || s[i] > '9') return 0;
    long v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (i + 1 < len && (s[i] == 'p' || s[i] == 'P') && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;
    while (i < len && isSpaceChar(s[i])) i++;
    if (i != len) return 0; // trailing garbage after the number/unit
    *out = (int)(sign * v);
    return 1;
}

static int applyParsedProperty(GfxStyle* style, const char* prop, int propLen, const char* val, int valLen,
                                const char* className, int line) {
    if (propLen == 10 && memcmp(prop, "background", 10) == 0) {
        if (!parseColorString(val, valLen, &style->backgroundColor)) {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: '.%s' — 'background' must be a color like #4F46E5 or a name like white", className);
            return 0;
        }
        style->hasBackground = 1;
        return 1;
    }
    if (propLen == 5 && memcmp(prop, "color", 5) == 0) {
        if (!parseColorString(val, valLen, &style->textColor)) {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: '.%s' — 'color' must be a color like #4F46E5 or a name like white", className);
            return 0;
        }
        style->hasTextColor = 1;
        return 1;
    }
    if (propLen == 6 && memcmp(prop, "border", 6) == 0) {
        if (!parseColorString(val, valLen, &style->borderColor)) {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: '.%s' — 'border' must be a color like #4F46E5 or a name like white", className);
            return 0;
        }
        style->hasBorderColor = 1;
        return 1;
    }
    if (propLen == 7 && memcmp(prop, "padding", 7) == 0) {
        int p;
        if (!parseIntLiteral(val, valLen, &p)) {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: '.%s' — 'padding' must be a number like 12 or 12px", className);
            return 0;
        }
        style->textPadding = p;
        style->hasTextPadding = 1;
        return 1;
    }
    if (propLen == 8 && memcmp(prop, "fontSize", 8) == 0) {
        int fs;
        if (!parseIntLiteral(val, valLen, &fs)) {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: '.%s' — 'fontSize' must be a number like 16", className);
            return 0;
        }
        style->fontSize = fs;
        style->hasFontSize = 1;
        return 1;
    }
    novaError(ERR_ARGUMENT, line,
              "graphics.styleSheet: '.%s' — '%.*s' is not a supported style property — supported: background, color, padding, border, fontSize "
              "(rounded corners aren't supported by the underlying renderer)",
              className, propLen, prop);
    return 0;
}

static Value gfx_styleSheet(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.styleSheet: expected a string (got %s)", typeName(args[0]));
        return makeNull();
    }
    const char* s = args[0].as.string->data;
    int len = args[0].as.string->length;
    int i = 0;

    while (i < len) {
        while (i < len && isSpaceChar(s[i])) i++;
        if (i >= len) break;

        if (s[i] != '.') {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: expected a class selector starting with '.' (e.g. \".primary\")");
            return makeNull();
        }
        i++;
        int nameStart = i;
        while (i < len && isNameChar(s[i])) i++;
        int nameLen = i - nameStart;
        if (nameLen == 0) {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: expected a class name after '.'");
            return makeNull();
        }
        char className[GFX_STYLE_NAME_LEN];
        int cn = nameLen; if (cn >= (int)sizeof(className)) cn = sizeof(className) - 1;
        memcpy(className, s + nameStart, cn);
        className[cn] = '\0';

        while (i < len && isSpaceChar(s[i])) i++;
        if (i >= len || s[i] != '{') {
            novaError(ERR_ARGUMENT, line, "graphics.styleSheet: expected '{' after '.%s'", className);
            return makeNull();
        }
        i++;

        GfxStyle style;
        memset(&style, 0, sizeof(style));

        for (;;) {
            while (i < len && isSpaceChar(s[i])) i++;
            if (i < len && s[i] == '}') { i++; break; }
            if (i >= len) {
                novaError(ERR_ARGUMENT, line, "graphics.styleSheet: unterminated block for '.%s' (missing '}')", className);
                return makeNull();
            }

            int propStart = i;
            while (i < len && isNameChar(s[i])) i++;
            int propLen = i - propStart;
            if (propLen == 0) {
                novaError(ERR_ARGUMENT, line, "graphics.styleSheet: expected a property name in '.%s'", className);
                return makeNull();
            }

            while (i < len && isSpaceChar(s[i])) i++;
            if (i >= len || s[i] != ':') {
                novaError(ERR_ARGUMENT, line, "graphics.styleSheet: expected ':' after property name in '.%s'", className);
                return makeNull();
            }
            i++;
            while (i < len && isSpaceChar(s[i])) i++;

            int valStart = i;
            while (i < len && s[i] != ';' && s[i] != '}') i++;
            int valEnd = i;
            while (valEnd > valStart && isSpaceChar(s[valEnd - 1])) valEnd--;
            int valLen = valEnd - valStart;

            if (!applyParsedProperty(&style, s + propStart, propLen, s + valStart, valLen, className, line)) {
                return makeNull(); // applyParsedProperty already raised a novaError
            }

            if (i < len && s[i] == ';') i++;
        }

        if (!registerNamedStyle(className, &style, "styleSheet", line)) return makeNull();
    }

    return makeNull();
}

// A canvas has a fixed pixel size (unlike other widgets, which fill
// whatever width the layout gives them) — drawing coordinates need a
// stable space to mean anything, the same reason an <img> or a chart
// needs explicit dimensions. `onDraw` is called every single frame
// (not just on some state change, unlike onClick/onChange), with the
// canvas's own handle as its one argument — inside it, a script calls
// graphics.canvasLine/Rect/Circle/Text using coordinates relative to
// the canvas's own top-left corner (0,0), which get translated to
// actual screen position and clipped to the canvas's own bounds by
// graphics_render.c. This is what makes animation and live-updating
// charts possible: onDraw genuinely re-runs every frame, so values it
// reads (from Nova variables, other widgets' state, etc.) can change
// over time — unlike graphics.image(), which loads once.
static Value gfx_canvas(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int64_t width, height;
    if (!checkInt(args[0], "width", "canvas", line, &width)) return makeNull();
    if (!checkInt(args[1], "height", "canvas", line, &height)) return makeNull();
    if (args[2].type != VAL_FUNCTION) {
        novaError(ERR_ARGUMENT, line, "graphics.canvas: expected a function for onDraw (got %s)", typeName(args[2]));
        return makeNull();
    }
    if (gfxWidgetCount >= GFX_MAX_WIDGETS) {
        novaError(ERR_ARGUMENT, line, "graphics.canvas: too many widgets created (max %d)", GFX_MAX_WIDGETS);
        return makeNull();
    }
    int idx = gfxWidgetCount++;
    GfxWidget* wd = &gfxWidgets[idx];
    wd->used = 1;
    wd->kind = GFX_WIDGET_CANVAS;
    wd->text[0] = '\0';
    wd->hasOnClick = 1; // reused as "has onDraw" (see the struct comment) — always set, onDraw isn't optional
    wd->onClick = copyValue(args[2]);
    wd->hasOnChange = 0; wd->onChange = makeNull();
    wd->checked = 0;
    wd->childCount = 0; wd->gap = 0; wd->padding = 0;
    wd->minValue = (float)width; wd->maxValue = (float)height; wd->currentValue = 0;
    wd->selectedIndex = 0;
    memset(&wd->style, 0, sizeof(wd->style));
    wd->classCount = 0;
    return gfxMakeHandle("canvas", idx);
}

static int checkColorArg(Value v, const char* what, const char* fnName, int line, GfxColor* out) {
    if (v.type != VAL_STRING || !parseColorString(v.as.string->data, v.as.string->length, out)) {
        novaError(ERR_ARGUMENT, line, "graphics.%s: '%s' must be a color like \"#4F46E5\" or a name like \"white\"", fnName, what);
        return 0;
    }
    return 1;
}

// graphics.canvasLine/Rect/Circle/Text only make sense while a canvas's
// onDraw is actually running — see graphics_internal.h's comment on
// gfxIsDrawingActive. This file never includes raylib.h itself; these
// are validated wrappers that hand off to graphics_render.c's
// gfxCanvas* functions, which are the ones that actually know what a
// Color or a DrawLine call is.
static Value gfx_canvasLine(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkHandle(args[0], "canvas", "canvasLine", line, &handle)) return makeNull();
    if (!gfxIsDrawingActive()) {
        novaError(ERR_ARGUMENT, line, "graphics.canvasLine: can only be called from inside a canvas's onDraw callback");
        return makeNull();
    }
    float x1, y1, x2, y2, thickness;
    if (!checkFloat(args[1], "x1", "canvasLine", line, &x1)) return makeNull();
    if (!checkFloat(args[2], "y1", "canvasLine", line, &y1)) return makeNull();
    if (!checkFloat(args[3], "x2", "canvasLine", line, &x2)) return makeNull();
    if (!checkFloat(args[4], "y2", "canvasLine", line, &y2)) return makeNull();
    if (!checkFloat(args[5], "thickness", "canvasLine", line, &thickness)) return makeNull();
    GfxColor color;
    if (!checkColorArg(args[6], "color", "canvasLine", line, &color)) return makeNull();
    gfxCanvasLine(x1, y1, x2, y2, thickness, color);
    return makeNull();
}

static Value gfx_canvasRect(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkHandle(args[0], "canvas", "canvasRect", line, &handle)) return makeNull();
    if (!gfxIsDrawingActive()) {
        novaError(ERR_ARGUMENT, line, "graphics.canvasRect: can only be called from inside a canvas's onDraw callback");
        return makeNull();
    }
    float x, y, w, h;
    if (!checkFloat(args[1], "x", "canvasRect", line, &x)) return makeNull();
    if (!checkFloat(args[2], "y", "canvasRect", line, &y)) return makeNull();
    if (!checkFloat(args[3], "width", "canvasRect", line, &w)) return makeNull();
    if (!checkFloat(args[4], "height", "canvasRect", line, &h)) return makeNull();
    GfxColor color;
    if (!checkColorArg(args[5], "color", "canvasRect", line, &color)) return makeNull();
    gfxCanvasRect(x, y, w, h, color);
    return makeNull();
}

static Value gfx_canvasCircle(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkHandle(args[0], "canvas", "canvasCircle", line, &handle)) return makeNull();
    if (!gfxIsDrawingActive()) {
        novaError(ERR_ARGUMENT, line, "graphics.canvasCircle: can only be called from inside a canvas's onDraw callback");
        return makeNull();
    }
    float x, y, radius;
    if (!checkFloat(args[1], "x", "canvasCircle", line, &x)) return makeNull();
    if (!checkFloat(args[2], "y", "canvasCircle", line, &y)) return makeNull();
    if (!checkFloat(args[3], "radius", "canvasCircle", line, &radius)) return makeNull();
    GfxColor color;
    if (!checkColorArg(args[4], "color", "canvasCircle", line, &color)) return makeNull();
    gfxCanvasCircle(x, y, radius, color);
    return makeNull();
}

static Value gfx_canvasText(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    int handle;
    if (!checkHandle(args[0], "canvas", "canvasText", line, &handle)) return makeNull();
    if (!gfxIsDrawingActive()) {
        novaError(ERR_ARGUMENT, line, "graphics.canvasText: can only be called from inside a canvas's onDraw callback");
        return makeNull();
    }
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "graphics.canvasText: expected a string (got %s)", typeName(args[1]));
        return makeNull();
    }
    float x, y;
    if (!checkFloat(args[2], "x", "canvasText", line, &x)) return makeNull();
    if (!checkFloat(args[3], "y", "canvasText", line, &y)) return makeNull();
    int64_t fontSize;
    if (!checkInt(args[4], "fontSize", "canvasText", line, &fontSize)) return makeNull();
    GfxColor color;
    if (!checkColorArg(args[5], "color", "canvasText", line, &color)) return makeNull();

    char textBuf[GFX_MAX_TEXT];
    copyTextInto(textBuf, sizeof(textBuf), args[1].as.string);
    gfxCanvasText(textBuf, x, y, (int)fontSize, color);
    return makeNull();
}

static Value gfx_show(VM* vm, Value* args, int argCount, int line) {
    (void)argCount;
    int winHandle;
    if (!checkHandle(args[0], "window", "show", line, &winHandle)) return makeNull();
    gfxRunEventLoop(vm, winHandle); // blocks until the window closes — see graphics_render.c
    return makeNull();
}

static NativeFnEntry graphicsFunctions[] = {
    {"window",       gfx_window,       3},
    {"label",        gfx_label,        1},
    {"button",       gfx_button,       1},
    {"input",        gfx_input,        1},
    {"checkbox",     gfx_checkbox,     1},
    {"image",        gfx_image,        1},
    {"slider",       gfx_slider,       3},
    {"progressBar",  gfx_progressBar,  3},
    {"radioGroup",   gfx_radioGroup,   2},
    {"dropdown",     gfx_dropdown,     2},
    {"tabs",         gfx_tabs,         3},
    {"canvas",       gfx_canvas,       3},
    {"canvasLine",   gfx_canvasLine,   7},
    {"canvasRect",   gfx_canvasRect,   6},
    {"canvasCircle", gfx_canvasCircle, 5},
    {"canvasText",   gfx_canvasText,   6},
    {"row",          gfx_row,          2},
    {"column",       gfx_column,       2},
    {"onClick",      gfx_onClick,      2},
    {"onChange",     gfx_onChange,     2},
    {"add",          gfx_add,          2},
    {"setText",      gfx_setText,      2},
    {"getText",      gfx_getText,      1},
    {"isChecked",    gfx_isChecked,    1},
    {"setChecked",   gfx_setChecked,   2},
    {"getValue",     gfx_getValue,     1},
    {"setValue",     gfx_setValue,     2},
    {"getSelected",  gfx_getSelected,  1},
    {"setSelected",  gfx_setSelected,  2},
    {"style",        gfx_style,        2},
    {"defineStyle",  gfx_defineStyle,  2},
    {"addClass",     gfx_addClass,     2},
    {"styleSheet",   gfx_styleSheet,   1},
    {"show",         gfx_show,         1},
};

NativeModule graphicsModule = {
    "graphics",
    graphicsFunctions,
    sizeof(graphicsFunctions) / sizeof(graphicsFunctions[0])
};
