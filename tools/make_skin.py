#!/usr/bin/env python3
"""make_skin.py - draws the XP-08 touch UI for the MPC into skins/ (and preview/*.png).

    python3 tools/make_skin.py        (after a native build: it reads build/params.json)

The PLAY page is the synth's screen: a backlit yellow-green LCD in a dark panel, as on the XP-80
and its JV-generation siblings, showing the bank and its board, the patch number and name, the
octave and the four tone switches. The MPC draws that text live (Value labels bound to the
plugin's parameters); this script draws the glass, the bezel and the fixed captions. Below it,
and on the other pages, the controls use the MPC Plugin SDK's UI kit (film-strip knobs, toggles,
buttons). No maker's name or logo is drawn.

The edit pages follow the XP-80's edit sections, one function key each, their pages as sub-tabs
(press the key again for the next page): COMMON, WAVE / PITCH, TVF, TVA, LFO / CTRL. Every tone
page has a strip on top, as the XP-80's panel: a small screen with the tone being edited and the
patch, the TONE SELECT buttons (which tone the page edits) and the TONE SWITCH buttons.

Needs Pillow. The JSON writer follows the Ju-08 skin script (same author, MIT); format reference:
docs/ui-reference.md of the MPC Plugin SDK.
"""

import json
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.abspath(os.environ.get("SDK", os.path.join(ROOT, "..", "mpc-plugin-builder-sdk")))
OUT = os.path.join(ROOT, "skins")
PREVIEW = os.path.join(ROOT, "preview")
SS = 2

W, H = 1280, 628
COL_X = [8, 326, 644, 962]
COL_W = 310
CELL_H = 140

PANEL = (34, 36, 40)
PANEL_HI = (52, 55, 60)
SILK = (214, 214, 206)
SILK_DIM = (130, 132, 132)
ACCENT = (150, 210, 70)
LCD_HI = (196, 214, 92)
LCD_LO = (150, 172, 54)
LCD_INK = (28, 40, 18)
LCD_GRID = (0, 0, 0, 18)
VALUE = (236, 232, 214)


def argb(rgb, a=255):
    return "%02x%02x%02x%02x" % (a, rgb[0], rgb[1], rgb[2])


def find_font(bold=True):
    for p in ("/usr/share/fonts/truetype/roboto/unhinted/RobotoCondensed-Bold.ttf" if bold else
              "/usr/share/fonts/truetype/roboto/unhinted/RobotoCondensed-Regular.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf" if bold else
              "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if os.path.isfile(p):
            return p
    raise SystemExit("no usable font found")


FONT_B, FONT_R = find_font(True), find_font(False)


def font(size, bold=True):
    return ImageFont.truetype(FONT_B if bold else FONT_R, int(size * SS))


# ------------------------------------------------------------------ the TUI.json writer
class O(list):
    """An object as an ordered list of (key, value) pairs: the overlay widgets need two "version" keys."""


def obj(*pairs):
    return O(pairs)


def dumps(o):
    if isinstance(o, O):
        return "{" + ", ".join(json.dumps(k) + ": " + dumps(v) for k, v in o) + "}"
    if isinstance(o, (list, tuple)):
        return "[" + ", ".join(dumps(v) for v in o) + "]"
    return json.dumps(o)


TRANSPARENT = obj(("version", 1), ("focussed", obj(("version", 1), ("colour", "0"), ("image", ""))),
                  ("unfocussed", obj(("version", 1), ("colour", "0"), ("image", ""))))


def bounds(rect, focus="No", invalid="Show", when="Always", only_when=None):
    """only_when: a handle such as "IndexedEnabling/1/5/Parameter 70": the entry shows only while it is valid"""
    if only_when:
        return obj(("version", 2), ("acceptsHWFocus", focus), ("showWhenDataModelInvalid", "Hide"),
                   ("whenVisible", when), ("boundsType", "Absolute"), ("bounds", "%g %g %g %g" % tuple(rect)),
                   ("additionalInvalidatingHandles", [only_when]))
    return obj(("version", 1), ("acceptsHWFocus", focus), ("showWhenDataModelInvalid", invalid),
               ("whenVisible", when), ("boundsType", "Absolute"), ("bounds", "%g %g %g %g" % tuple(rect)))


def entry(name, typ, data, rect, remap=None, **kw):
    return obj(("version", 2),
               ("componentData", obj(("version", 1), ("name", name), ("type", typ), ("data", data))),
               ("handle remapping", obj(("version", 1), ("map", [obj(("key", k), ("value", v)) for k, v in (remap or [])]))),
               ("bounds", bounds(rect, **kw)))


def action(on, handler, extra=""):
    return obj(("version", 1), ("onAction", on), ("handler", handler), ("handleName", "Data"), ("additionalData", extra))


def component(children, overlay=None, qlink=True):
    acts = [action("Mouse Down", "Q-Link")] if qlink else []
    if overlay:
        acts += [action("Double Click", "Show Overlay", overlay), action("Enter Pressed", "Show Overlay", overlay)]
    return obj(("version", 2), ("actions", acts), ("backgroundData", TRANSPARENT), ("ignoreMousePresses", not qlink),
               ("disableCoarseDataWheel", False), ("componentsData", children))


def text_style(height, colour, just, style="SemiBold", case="Original", name="Titillium Web"):
    return obj(("version", 1), ("font", obj(("version", 1), ("name", name), ("style", style), ("height", float(height)))),
               ("colour", colour), ("justification", just), ("case", case))


def label(kind, rect, height, colour, just, style="SemiBold", case="Original", text=None):
    data = [("version", 1), ("textStyle", text_style(height, colour, just, style, case)), ("type", kind),
            ("handleName", "Invalid" if kind == "Text" else "Data")]
    if text is not None:
        data.append(("text", text))
    return entry(kind.lower(), "Label", O(data), rect)


def focus_ring(w, h):
    return entry("focus", "Focus", obj(("version", 1), ("backgroundColour", "0cffffff"), ("outlineColour", argb(ACCENT)),
                                       ("backgroundInset", 3.0), ("outlineThickness", 2.0)),
                 (0, 0, w, h), when="WhenFocussed")


def knob_overlay(polarity):
    focus_lost = [obj(("version", 1), ("onAction", "Focus Lost"), ("handler", "Focus Lost"), ("handleName", "Focus Lost"), ("additionalData", ""))]
    data = O([("version", 2), ("version", 1), ("actions", focus_lost), ("overlayType", "Knob With Header"), ("scaleType", "Linear"),
              ("polarityType", polarity), ("showMinusSign", True), ("handleName", "Data")])
    dark = obj(("version", 1), ("focussed", obj(("version", 1), ("colour", "ff1e1d1e"), ("image", ""))),
               ("unfocussed", obj(("version", 1), ("colour", "ff1e1d1e"), ("image", ""))))
    return obj(("version", 2), ("actions", []), ("backgroundData", dark), ("ignoreMousePresses", True), ("disableCoarseDataWheel", False),
               ("componentsData", [entry("overlay", "KnobOverlay", data, (0, 0, 1280, 800))]))


# ------------------------------------------------------------------ pages
# (function key, page name, layout, rows). Layouts: "play" (the big screen, then 2 rows), "plain"
# (up to 4 rows), "tone" (the tone strip, then 3 rows). A cell is a parameter name (the widget
# follows the parameter: a choice of 2 is a switch, a signed value a bipolar knob), None for an
# empty place, or (kind, name, caption) for the PLAY / SLOTS pages' special cells.
def env_rows(p, levels, extra):
    return [["%s T%d" % (p, i) for i in range(1, 5)], ["%s L%d" % (p, i) for i in range(1, levels + 1)] + [None] * (4 - levels), extra]


def lfo_rows(n):
    l = "LFO%d" % n
    return [[l + " Wave", l + " Rate", l + " Offset", l + " KeySync"], [l + " Delay", l + " Fade", l + " FadeT", None],
            [l + " Pitch", l + " TVF", l + " TVA", l + " Pan"]]


def ctrl_rows(n, src):
    return [["Ctl%d Dest%d" % (n, i) for i in range(1, 5)], ["Ctl%d Sens%d" % (n, i) for i in range(1, 5)], [src, None, None, None]]


# Five function keys. TONE is the everyday editing (with an ADSR graph for the amp and the filter,
# and the filter's response); PRO 1 / PRO 2 hold every XP-80 parameter, as before.
# (function key, page name, layout, rows[, graphs]). Graphs: ("filter", rect) the filter's response
# for its type and cutoff; ("env", rect, prefix, title) an ADSR graph bound to prefix Attack..Release.
GRAPH_Y, GRAPH_H = 118, 200
PAGES = [
    (0, "PLAY", "play", [
        [("knob", "Bank", "BANK"), ("knob_step", "Patch", "PATCH"), ("knob", "Octave", "OCTAVE"), ("knob", "Volume", "VOLUME")],
        [("tone", "Tone 1", "TONE 1"), ("tone", "Tone 2", "TONE 2"), ("tone", "Tone 3", "TONE 3"), ("tone", "Tone 4", "TONE 4")],
    ]),
    (0, "SLOTS", "plain", [
        [("lcd", "Slot A", "SLOT A"), ("lcd", "Slot B", "SLOT B"), ("lcd", "Slot C", "SLOT C"), ("lcd", "Slot D", "SLOT D")],
        [("knob", "Slot A", "BOARD"), ("knob", "Slot B", "BOARD"), ("knob", "Slot C", "BOARD"), ("knob", "Slot D", "BOARD")],
    ]),
    (0, "SETUP", "plain", [
        ["Cutoff", "Resonance", "Attack", "Release"],          # quick edit, on top of every patch
        ["Prog Change", "Voices", "Filter Model", "Filter Drive"],
    ]),
    (1, "WAVE", "tone", [
        ["Wave Group", "Wave", "Tone Level", "Tone Pan"],
        ["Coarse Tune", "Fine Tune", "Key Low", "Key High"],
        ["Velo Low", "Velo High", "Velo Xfade", "Wave Gain"],
    ]),
    (1, "FILTER", "graph", [
        ["Filter Type", "TVF Cutoff", "TVF Reso", "F-Env Depth"],
        ["Flt Attack", "Flt Decay", "Flt Sustain", "Flt Release"],
    ], [("filter", (16, GRAPH_Y, 608, GRAPH_H)), ("env", (656, GRAPH_Y, 608, GRAPH_H), "Flt", "FILTER ENVELOPE")]),
    (1, "AMP", "graph", [
        ["Amp Attack", "Amp Decay", "Amp Sustain", "Amp Release"],
        ["Tone Level", "A-Env Velo", "A-Env Curve", "Tone Pan"],
    ], [("env", (16, GRAPH_Y, 1248, GRAPH_H), "Amp", "AMP ENVELOPE")]),
    (1, "LFO", "tone", [
        ["LFO1 Wave", "LFO1 Rate", "LFO1 Pitch", "LFO1 TVF"],
        ["LFO2 Wave", "LFO2 Rate", "LFO2 Pitch", "LFO2 TVF"],
        ["LFO1 TVA", "LFO2 TVA", "LFO1 Delay", "LFO2 Delay"],
    ]),
    (2, "EFX", "plain", [
        ["EFX Type", "EFX Out Lvl", "EFX>Chorus", "EFX>Reverb"],
        ["Tone 1 > EFX", "Tone 2 > EFX", "Tone 3 > EFX", "Tone 4 > EFX"],   # only tones sent to the EFX go through it
        ["EFX", "Chorus", "Reverb", None],
    ]),
    (2, "EFX EDIT", "plain", [   # the type's own parameters: each knob's caption is its name for the type
        [("knob_named", "EFX %d" % k, "EFX %d Name" % k) for k in (1, 2, 3, 4)],
        [("knob_named", "EFX %d" % k, "EFX %d Name" % k) for k in (5, 6, 7, 8)],
        [("knob_named", "EFX %d" % k, "EFX %d Name" % k) for k in (9, 10, 11, 12)],
        ["EFX Type", "EFX Out Lvl", None, None],
    ]),
    (2, "CHO/REV", "plain", [
        ["Chorus Lvl", "Chorus Rate", "Chorus Dpth", "Chorus PreD"],
        ["Chorus Fbk", "Chorus Out", None, None],
        ["Reverb Type", "Reverb Lvl", "Reverb Time", "HF Damp"],
        ["Delay Fbk", None, None, None],
    ]),
    (3, "COMMON", "plain", [
        ["Patch Level", "Patch Pan", "Patch Oct", "Velo Range"],
        ["Bend Up", "Bend Down", "Key Assign", "Solo Legato"],
        ["Portamento", "Porta Mode", "Porta Time", None],
        ["Ctrl 2 Src", "Ctrl 3 Src", None, None],
    ]),
    (3, "WG", "tone", [
        ["Wave Group", "Wave", "Wave Gain", "Delay Mode"],
        ["Key Low", "Key High", "Velo Low", "Velo High"],
        ["Velo Xfade", "Delay Time", "Hold Sw", "Bend Sw"],
    ]),
    (3, "PITCH", "tone", [
        ["Coarse Tune", "Fine Tune", "Rnd Pitch", "Pitch KF"],
        ["P-Env Depth", "P-Env Velo", "P-Env T1 Vel", "P-Env T4 Vel"],
        ["P-Env T KF", None, None, None],
    ]),
    (3, "P-ENV", "tone", env_rows("P-Env", 4, ["P-Env Depth", "P-Env Velo", "P-Env T KF", None])),
    (3, "TVF", "tone", [
        ["Filter Type", "TVF Cutoff", "TVF Reso", "Cutoff KF"],
        ["F-Env Depth", "F-Env Curve", "F-Env Velo", "F-Env T KF"],
        ["F-Env T1 Vel", "F-Env T4 Vel", "Filter Model", "Filter Drive"],
    ]),
    (3, "F-ENV", "tone", env_rows("F-Env", 4, ["F-Env Depth", "F-Env Velo", "F-Env T KF", None])),
    (3, "TVA", "tone", [
        ["Tone Level", "A-Env Curve", "A-Env Velo", "A-Env T KF"],
        ["Bias Dir", "Bias Point", "Bias Level", "Volume Sw"],
        ["A-Env T1 Vel", "A-Env T4 Vel", None, None],
    ]),
    (3, "A-ENV", "tone", env_rows("A-Env", 3, ["Tone Level", "A-Env Velo", "A-Env T KF", None])),
    (4, "PAN/OUT", "tone", [
        ["Tone Pan", "Pan KF", "Random Pan", "Alt Pan"],
        ["Output", "Output Lvl", "Chorus Send", "Reverb Send"],
        ["Pan Sw", None, None, None],
    ]),
    (4, "LFO1", "tone", lfo_rows(1)),
    (4, "LFO2", "tone", lfo_rows(2)),
    (4, "CTRL1", "tone", ctrl_rows(1, None)),
    (4, "CTRL2", "tone", ctrl_rows(2, "Ctrl 2 Src")),
    (4, "CTRL3", "tone", ctrl_rows(3, "Ctrl 3 Src")),
]

# knobs with DEC / INC buttons (the knobs and the data wheel skip values on these 256-step parameters)
STEPS = {"Patch": ("Patch -", "Patch +"), "Wave": ("Wave -", "Wave +")}
STEP_W, STEP_H = 64, 38

# parameters shown on a bipolar knob (a signed value: the knob's centre is 0)
BIPOLAR = {"Cutoff", "Resonance", "Attack", "Release", "Patch Pan", "Patch Oct", "Coarse Tune", "Fine Tune", "Pitch KF",
           "Cutoff KF", "Tone Pan", "Pan KF", "Alt Pan", "Bias Level", "P-Env Depth", "F-Env Depth"}
BIPOLAR_PREFIX = ("P-Env L", "LFO1 Pitch", "LFO1 TVF", "LFO1 TVA", "LFO1 Pan", "LFO2 Pitch", "LFO2 TVF", "LFO2 TVA", "LFO2 Pan", "Ctl")
BIPOLAR_SUFFIX = (" Velo", " Vel", " T KF")

# choices whose names the plugin makes from what it finds in roms/ (the boards' names): the MPC must
# ask the plugin for their text, so they get no replacementStrings (which the MPC would show
# instead, with the names of the build machine's roms/)
DYNAMIC = {"Slot A", "Slot B", "Slot C", "Slot D", "Bank"}

SCREEN = (40, 14, 1200, 290)          # the PLAY page's bezel
ROWS = {"play": [322, 474], "plain": [14, 164, 314, 464], "tone": [124, 280, 436], "graph": [330, 478]}
STRIP_H = 108                         # the tone strip


def is_bipolar(name):
    if name in BIPOLAR:
        return True
    if name.startswith("Ctl") and "Dest" in name:
        return False
    return name.startswith(BIPOLAR_PREFIX) or name.endswith(BIPOLAR_SUFFIX)


def lcd_glass(w, h, big=False):
    """A backlit STN glass: a soft gradient, a faint dot grid, an inner shadow."""
    img = Image.new("RGBA", (w * SS, h * SS))
    d = ImageDraw.Draw(img)
    for y in range(h * SS):
        t = y / max(1, h * SS - 1)
        c = tuple(int(LCD_HI[i] + (LCD_LO[i] - LCD_HI[i]) * (0.25 + 0.75 * t)) for i in range(3))
        d.line((0, y, w * SS, y), fill=c + (255,))
    step = (4 if big else 3) * SS
    grid = Image.new("RGBA", img.size)
    g = ImageDraw.Draw(grid)
    for x in range(0, w * SS, step):
        g.line((x, 0, x, h * SS), fill=LCD_GRID)
    for y in range(0, h * SS, step):
        g.line((0, y, w * SS, y), fill=LCD_GRID)
    img.alpha_composite(grid)
    for i in range(6 * SS):
        a = int(70 * (1 - i / (6 * SS)))
        d.rectangle((i, i, w * SS - 1 - i, h * SS - 1 - i), outline=(0, 0, 0, a))
    return img


def background():
    img = Image.new("RGBA", (W * SS, H * SS), PANEL + (255,))
    d = ImageDraw.Draw(img)
    for y in range(0, H * SS, 2 * SS):                  # a faint vertical gradient
        t = y / (H * SS)
        c = tuple(int(PANEL_HI[i] + (PANEL[i] - PANEL_HI[i]) * min(1, t * 1.6)) for i in range(3))
        d.line((0, y, W * SS, y + 2 * SS), fill=c + (255,))
    return img, d


def paste(img, part, x, y):
    img.alpha_composite(part, (int(x * SS), int(y * SS)))


def draw_screen(img, d):
    x, y, w, h = SCREEN
    d.rounded_rectangle((x * SS, y * SS, (x + w) * SS, (y + h) * SS), radius=14 * SS, fill=(14, 15, 17, 255), outline=(70, 72, 78, 255), width=2 * SS)
    gx, gy, gw, gh = x + 26, y + 22, w - 52, h - 44
    paste(img, lcd_glass(gw, gh, big=True), gx, gy)
    d = ImageDraw.Draw(img)
    ink = LCD_INK + (255,)
    d.text(((gx + 22) * SS, (gy + 12) * SS), "PATCH", font=font(22), fill=ink)
    d.line(((gx + 22) * SS, (gy + 44) * SS, (gx + gw - 22) * SS, (gy + 44) * SS), fill=ink, width=2 * SS)
    d.text(((gx + gw - 250) * SS, (gy + 12) * SS), "OCT", font=font(22), fill=ink)
    d.line(((gx + 22) * SS, (gy + 182) * SS, (gx + gw - 22) * SS, (gy + 182) * SS), fill=ink, width=2 * SS)
    d.text(((gx + 22) * SS, (gy + 196) * SS), "TONE SW", font=font(20), fill=ink)
    for t in range(4):
        tx = gx + 220 + t * 190
        d.text((tx * SS, (gy + 196) * SS), str(t + 1), font=font(20), fill=ink)
    return d


# the tone strip: a small screen (edit tone, patch), then TONE SELECT and TONE SWITCH, 4 buttons each
STRIP_LCD = (16, 10, 400, 88)
STRIP_BTN_X = [596 + 170 * i for i in range(4)]
STRIP_BTN_Y = [8, 58]


def draw_strip(img, d, page):
    x, y, w, h = STRIP_LCD
    d.rounded_rectangle(((x - 4) * SS, (y - 4) * SS, (x + w + 4) * SS, (y + h + 4) * SS), radius=8 * SS, fill=(14, 15, 17, 255),
                        outline=(70, 72, 78, 255), width=SS)
    paste(img, lcd_glass(w, h), x, y)
    d = ImageDraw.Draw(img)
    d.text(((x + 14) * SS, (y + 8) * SS), page, font=font(17), fill=LCD_INK + (255,))
    for r, cap in enumerate(("TONE SELECT", "TONE SWITCH")):
        d.text((436 * SS, (STRIP_BTN_Y[r] + 22) * SS), cap, font=font(17), fill=SILK + (255,), anchor="lm")
        for i in range(4):
            d.text(((STRIP_BTN_X[i] - 14) * SS, (STRIP_BTN_Y[r] + 22) * SS), str(i + 1), font=font(19), fill=SILK + (255,), anchor="rm")
    yl = STRIP_H + 4
    d.line((20 * SS, yl * SS, (W - 20) * SS, yl * SS), fill=PANEL_HI + (255,), width=SS)
    return d


def make_step_images():
    """The DEC / INC buttons: a dark key with a minus or plus, lit while pressed."""
    for sign in ("dec", "inc"):
        for lit in (False, True):
            img = Image.new("RGBA", (STEP_W * SS, STEP_H * SS))
            d = ImageDraw.Draw(img)
            fill = (70, 92, 40, 255) if lit else (44, 46, 52, 255)
            d.rounded_rectangle((SS, SS, (STEP_W - 1) * SS, (STEP_H - 1) * SS), radius=7 * SS, fill=fill,
                                outline=ACCENT + (255,) if lit else (86, 88, 96, 255), width=2 * SS)
            cx, cy, a = STEP_W * SS // 2, STEP_H * SS // 2, 8 * SS
            ink = ACCENT + (255,) if lit else SILK + (255,)
            d.line((cx - a, cy, cx + a, cy), fill=ink, width=3 * SS)
            if sign == "inc":
                d.line((cx, cy - a, cx, cy + a), fill=ink, width=3 * SS)
            img.resize((STEP_W, STEP_H), Image.LANCZOS).save(os.path.join(OUT, "step_%s_%s.png" % (sign, "on" if lit else "off")))


# ------------------------------------------------------------------ the filter's response
FILTER_TYPES = ["OFF", "LPF", "BPF", "HPF", "PKG"]
FILTER_FRAMES = 32                    # cutoff in steps of 4: the strips stay small once the MPC decodes them
FILTER_W, FILTER_H = 448, 150          # (each strip 448 x 150 x 32 frames: about 8.6 MB as pixels)


def filter_gain_db(kind, f, fc, k):
    """The engine's 2-pole state-variable filter (engine.cpp: Svf, and the TVF's outputs) at f for cutoff fc."""
    import cmath
    s = 1j * f / fc
    den = s * s + k * s + 1
    if kind == 1:
        h = 1 / den
    elif kind == 2:
        h = k * s / den
    elif kind == 3:
        h = s * s / den
    elif kind == 4:
        h = 1 + 3 * k * s / den
    else:
        h = 1
    return 20 * math.log10(max(abs(h), 1e-6))


def make_filter_strips(w, h):
    """For each filter type, a film strip of its response for every cutoff value (0..127), as the engine
    sets the cutoff (20 Hz * 2^(cutoff / 127 * 10.2), at C4) and with a moderate resonance; the OFF image."""
    k = 2.0 - 1.96 * 0.25                   # the curves' resonance: 0.25 of the range (the picture follows cutoff and type)
    fmin, fmax, dbmin, dbmax = 20.0, 20000.0, -36.0, 15.0
    def x_of(fr):
        return (math.log10(fr) - math.log10(fmin)) / (math.log10(fmax) - math.log10(fmin)) * (w - 1)
    def y_of(db):
        return (dbmax - max(dbmin, min(dbmax, db))) / (dbmax - dbmin) * (h - 1)
    def grid(d):
        for fr in (100, 1000, 10000):
            d.line((x_of(fr), 0, x_of(fr), h), fill=(255, 255, 255, 22), width=1)
        d.line((0, y_of(0), w, y_of(0)), fill=(255, 255, 255, 40), width=1)
        for fr, t in ((100, "100"), (1000, "1k"), (10000, "10k")):
            d.text((x_of(fr) + 3, h - 16), t, font=ImageFont.truetype(FONT_R, 12), fill=SILK_DIM + (255,))
    for kind in range(1, 5):
        strip = Image.new("RGBA", (w, h * FILTER_FRAMES), (0, 0, 0, 0))
        for i in range(FILTER_FRAMES):
            fc = 20.0 * 2 ** (min(i * 127.0 / (FILTER_FRAMES - 1), 127.0) / 127.0 * 10.2)
            frame = Image.new("RGBA", (w * 2, h * 2), (0, 0, 0, 0))
            fd = ImageDraw.Draw(frame)
            pts = []
            for px in range(0, w * 2, 2):
                fr = 10 ** (math.log10(fmin) + px / (w * 2 - 1) * (math.log10(fmax) - math.log10(fmin)))
                pts.append((px, y_of(filter_gain_db(kind, fr, fc, k)) * 2))
            fd.polygon(pts + [(w * 2, h * 2), (0, h * 2)], fill=ACCENT + (48,))
            fd.line(pts, fill=ACCENT + (255,), width=5)
            frame = frame.resize((w, h), Image.LANCZOS)
            grid(ImageDraw.Draw(frame))
            strip.alpha_composite(frame, (0, i * h))
        strip.save(os.path.join(OUT, "filter_%s.png" % FILTER_TYPES[kind].lower()), optimize=True)
    off = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    od = ImageDraw.Draw(off)
    grid(od)
    od.line((0, y_of(0), w, y_of(0)), fill=ACCENT + (255,), width=3)
    off.save(os.path.join(OUT, "filter_off.png"), optimize=True)


def envelope_data(name, handles):
    """The MPC's ADSR envelope widget: a curve with handles to drag, bound to four parameters."""
    a, d, s_, r = handles
    return O([("version", 1), ("envelopeType", "ADSR"), ("envelopeStyle", "WithHeader"), ("envelopeName", name),
              ("backgroundColour", argb((24, 26, 29))), ("scaleColour", argb((58, 60, 66))), ("lineColour", argb(ACCENT)),
              ("fillColour", argb(ACCENT, 0x40)), ("highlightColour", "ffffffff"), ("lineWidth", 3),
              ("handleWidth", 15), ("handleHeight", 10), ("handleShape", "Circle"), ("handleLineWidth", 2),
              ("handleLineColour", "ffffffff"), ("handleFillColour", argb((24, 26, 29))), ("handleTextColour", "ffffffff"),
              ("handleLineWidthOnPressed", 2), ("handleLineColourOnPressed", "ffffffff"), ("handleFillColourOnPressed", argb(ACCENT)),
              ("handleTextColourOnPressed", "ffffffff"), ("handleArrowLineColourOnPressed", "ffffffff"),
              ("handleArrowFillColourOnPressed", argb(ACCENT)),
              ("attackHandle", a), ("decayHandle", d), ("sustainHandle", s_), ("releaseHandle", r),
              ("attackShapeHandle", "Invalid"), ("decayShapeHandle", "Invalid"), ("releaseShapeHandle", "Invalid"),
              ("forceHideShapeHandles", True), ("forceCurveShape", 0.5), ("curveType", "Exponential")])


def main():
    params_path = os.path.join(ROOT, "build", "params.json")
    if not os.path.isfile(params_path):
        raise SystemExit("build/params.json is missing: run the SDK's tools/build.py . --target native first")
    info = json.load(open(params_path))
    params = {p["name"]: p for p in info["params"] if p["name"]}
    index = {n: p["index"] for n, p in params.items()}

    os.makedirs(OUT, exist_ok=True)
    os.makedirs(PREVIEW, exist_ok=True)
    for d_ in (OUT, PREVIEW):
        for f in os.listdir(d_):
            if f.endswith((".png", ".json")):
                os.remove(os.path.join(d_, f))
    kit = subprocess.run([sys.executable, os.path.join(SDK, "tools", "make_ui_kit.py"), "--out", OUT, "--theme", "lime", "--quiet",
                          "--only", "knob_uni,knob_bi,toggle_on,toggle_off,button_on,button_off"], capture_output=True, text=True)
    if kit.returncode != 0:
        raise SystemExit("make_ui_kit.py failed:\n" + kit.stdout + kit.stderr)
    for f in ("kit.json",):
        if os.path.isfile(os.path.join(OUT, f)):
            os.remove(os.path.join(OUT, f))

    make_step_images()
    defs = {}
    replacement = {}
    tabs, qmaps, previews = [], [], []

    def comp_knob(w, h, bi):
        name = "knob_%d_%s" % (w, "bi" if bi else "uni")
        if name not in defs:
            defs[name] = component([
                focus_ring(w, h),
                entry("knob", "Knob", obj(("version", 2), ("knobType", "FilmStrip"), ("filmStrip", "knob_%s.png" % ("bi" if bi else "uni")),
                                          ("numFrames", 127), ("invert", False), ("handleName", "Data")), (14, (h - 90) // 2, 90, 90)),
                label("Name", (116, 22, w - 122, 32), 20, argb(SILK_DIM), "left verticallyCentred", style="Bold", case="Upper Case"),
                label("Value", (116, 56, w - 122, 60), 24, argb(VALUE), "left verticallyCentred", style="SemiBold"),
            ], "knob overlay bipolar" if bi else "knob overlay")
        return name

    def comp_knob_step(w, h, bi):
        """A knob with DEC / INC buttons under its value (handles Data, Dec, Inc)."""
        name = "knob_step_%d_%s" % (w, "bi" if bi else "uni")
        if name not in defs:
            def step(handle, sign, x):
                return entry(sign, "Button", obj(("version", 2), ("onImage", "step_%s_on.png" % sign), ("offImage", "step_%s_off.png" % sign),
                                                 ("buttonId", 1), ("numButtonsInGroup", 1), ("handleName", handle), ("gestureBehaviour", "Instant")),
                             (x, h - STEP_H - 8, STEP_W, STEP_H))
            defs[name] = component([
                focus_ring(w, h),
                entry("knob", "Knob", obj(("version", 2), ("knobType", "FilmStrip"), ("filmStrip", "knob_%s.png" % ("bi" if bi else "uni")),
                                          ("numFrames", 127), ("invert", False), ("handleName", "Data")), (14, (h - 90) // 2, 90, 90)),
                label("Name", (116, 8, w - 122, 30), 20, argb(SILK_DIM), "left verticallyCentred", style="Bold", case="Upper Case"),
                label("Value", (116, 38, w - 122, 46), 24, argb(VALUE), "left verticallyCentred", style="SemiBold"),
                step("Dec", "dec", 116),
                step("Inc", "inc", 116 + STEP_W + 12),
            ], "knob overlay bipolar" if bi else "knob overlay")
        return name

    def comp_knob_named(w, h):
        """A knob whose caption is another parameter's value text (handle Title): a name that changes."""
        name = "knob_named_%d" % w
        if name not in defs:
            title = entry("title", "Label", O([("version", 1), ("textStyle", text_style(20, argb(SILK_DIM), "left verticallyCentred", "Bold", "Upper Case")),
                                               ("type", "Value"), ("handleName", "Title")]), (116, 22, w - 122, 32))
            defs[name] = component([
                focus_ring(w, h),
                entry("knob", "Knob", obj(("version", 2), ("knobType", "FilmStrip"), ("filmStrip", "knob_uni.png"),
                                          ("numFrames", 127), ("invert", False), ("handleName", "Data")), (14, (h - 90) // 2, 90, 90)),
                title,
                label("Value", (116, 56, w - 122, 60), 24, argb(VALUE), "left verticallyCentred", style="SemiBold"),
            ], "knob overlay")
        return name

    def comp_toggle(w, h):
        name = "toggle_%d" % w
        if name not in defs:
            defs[name] = component([
                focus_ring(w, h),
                entry("toggle", "Button", obj(("version", 2), ("onImage", "toggle_on.png"), ("offImage", "toggle_off.png"), ("buttonId", 1),
                                              ("numButtonsInGroup", 1), ("handleName", "Data"), ("gestureBehaviour", "Instant")),
                      (20, (h - 32) // 2, 64, 32)),
                label("Name", (100, 30, w - 106, 34), 20, argb(SILK_DIM), "left verticallyCentred", style="Bold", case="Upper Case"),
                label("Value", (100, 66, w - 106, 40), 24, argb(VALUE), "left verticallyCentred"),
            ], "knob overlay")
        return name

    def comp_tone(w, h, caption):
        name = "tone_%s" % caption.replace(" ", "").lower()
        if name not in defs:
            bx = (w - 120) // 2
            defs[name] = component([
                focus_ring(w, h),
                entry("button", "Button", obj(("version", 2), ("onImage", "button_on.png"), ("offImage", "button_off.png"), ("buttonId", 1),
                                              ("numButtonsInGroup", 1), ("handleName", "Data"), ("gestureBehaviour", "Instant")),
                      (bx, 52, 120, 44)),
                label("Text", (0, 12, w, 32), 21, argb(SILK), "horizontallyCentred verticallyCentred", style="Bold", text=caption),
            ], "knob overlay")
        return name

    def comp_strip_button(bid, group):
        """One button of the strip: TONE SELECT (a 4-way radio group) or a TONE SWITCH (a toggle)."""
        name = "strip_select_%d" % bid if group > 1 else "strip_switch"
        if name not in defs:
            defs[name] = component([
                entry("button", "Button", obj(("version", 2), ("onImage", "button_on.png"), ("offImage", "button_off.png"), ("buttonId", bid),
                                              ("numButtonsInGroup", group), ("handleName", "Data"), ("gestureBehaviour", "Instant")),
                      (0, 0, 120, 44)),
            ], qlink=False)
        return name

    def comp_lcd(w, h, size, just, style="Bold"):
        name = "lcd_%d_%d_%d_%s" % (w, h, size, just.split()[0])
        if name not in defs:
            defs[name] = component([label("Value", (0, 0, w, h), size, argb(LCD_INK), just, style=style)], qlink=False)
        return name

    def lcd_entry(pname, rect, size, just, style="Bold"):
        cname = comp_lcd(rect[2], rect[3], size, just, style)
        return entry("screen " + pname, cname, obj(("version", 1), ("handleName", "Data")), rect,
                     remap=[("Data", "Parameter %d" % index[pname])], focus="No", invalid="Hide")

    for pname in ("Tone 1", "Tone 2", "Tone 3", "Tone 4"):
        replacement[index[pname]] = ["--", "ON"]
    replacement[index["Edit Tone"]] = ["TONE 1", "TONE 2", "TONE 3", "TONE 4"]

    sub_of = {}
    make_filter_strips(FILTER_W, FILTER_H)
    for tab_i, page in enumerate(PAGES):
        fn, tab, layout, rows = page[:4]
        graphs = page[4] if len(page) > 4 else []
        sub = sub_of.get(fn, 0)
        sub_of[fn] = sub + 1
        img, d = background()
        children = []
        qlinks = {q: -1 for q in range(1, 17)}
        prev_items = []
        if layout == "play":
            d = draw_screen(img, d)
            gx, gy, gw, _gh = SCREEN[0] + 26, SCREEN[1] + 22, SCREEN[2] - 52, SCREEN[3] - 44
            screen_items = [
                ("Bank", (gx + 120, gy + 8, gw - 400, 36), 26, "left verticallyCentred", "SemiBold"),
                ("Octave", (gx + gw - 190, gy + 8, 170, 36), 26, "right verticallyCentred", "SemiBold"),
                ("Patch", (gx + 22, gy + 58, gw - 44, 112), 76, "left verticallyCentred", "Bold"),
            ] + [("Tone %d" % (t + 1), (gx + 250 + t * 190, gy + 190, 120, 34), 24, "left verticallyCentred", "Bold") for t in range(4)]
            for pname, rect, size, just, style in screen_items:
                children.append(lcd_entry(pname, rect, size, just, style))
                prev_items.append(("lcd", pname, rect, size, just))
        if layout in ("tone", "graph"):
            d = draw_strip(img, d, tab)
            x, y, w, h = STRIP_LCD
            for pname, rect, size, just in (("Edit Tone", (x + 14, y + 26, 150, 56), 34, "left verticallyCentred"),
                                            ("Patch", (x + 170, y + 26, w - 184, 56), 24, "left verticallyCentred")):
                children.append(lcd_entry(pname, rect, size, just))
                prev_items.append(("lcd", pname, rect, size, just))
            for i in range(4):
                for r, (pname, bid, group) in enumerate((("Edit Tone", i, 4), ("Tone %d" % (i + 1), 1, 1))):
                    rect = (STRIP_BTN_X[i], STRIP_BTN_Y[r], 120, 44)
                    children.append(entry("strip %s %d" % (pname, i), comp_strip_button(bid, group), obj(("version", 1), ("handleName", "Data")),
                                          rect, remap=[("Data", "Parameter %d" % index[pname])], focus="No", invalid="Show"))
                    prev_items.append(("strip", pname, rect, bid, group))
            qlinks[1] = index["Edit Tone"]
        for g in graphs:
            gx, gy, gw, gh = g[1]
            d.rounded_rectangle(((gx - 4) * SS, (gy - 4) * SS, (gx + gw + 4) * SS, (gy + gh + 4) * SS), radius=8 * SS,
                                fill=(24, 26, 29, 255), outline=(70, 72, 78, 255), width=SS)
            if g[0] == "filter":
                d.text(((gx + 10) * SS, (gy + 6) * SS), "FILTER", font=font(15), fill=SILK_DIM + (255,))
                gx, gy, gw, gh = gx + (gw - FILTER_W) // 2, gy + gh - FILTER_H - 12, FILTER_W, FILTER_H   # the curve's place
                ft = index["Filter Type"]
                for kind, name in enumerate(FILTER_TYPES):
                    when = "IndexedEnabling/%d/%d/Parameter %d" % (kind, len(FILTER_TYPES), ft)
                    if kind == 0:
                        children.append(entry("filter off", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"),
                                                                          ("image", "filter_off.png")), (gx, gy, gw, gh), only_when=when))
                        continue
                    cname = "filter_curve_%s" % name.lower()
                    if cname not in defs:
                        defs[cname] = component([entry("curve", "Knob", obj(("version", 2), ("knobType", "FilmStrip"),
                                                                            ("filmStrip", "filter_%s.png" % name.lower()),
                                                                            ("numFrames", FILTER_FRAMES), ("invert", False), ("handleName", "Data")),
                                                       (0, 0, gw, gh))], "knob overlay", qlink=False)
                    children.append(entry("filter " + name, cname, obj(("version", 1), ("handleName", "Data")), (gx, gy, gw, gh),
                                          remap=[("Data", "Parameter %d" % index["TVF Cutoff"])], only_when=when))
                prev_items.append(("filter", "TVF Cutoff", (gx, gy, gw, gh), None, None))
            else:
                pre, title = g[2], g[3]
                hs = ["Parameter %d" % index["%s %s" % (pre, st)] for st in ("Attack", "Decay", "Sustain", "Release")]
                children.append(entry("env " + pre, "Envelope", envelope_data(title, hs), (gx, gy, gw, gh)))
                prev_items.append(("env", pre + " Attack", g[1], title, None))
        ys = ROWS[layout]
        for r_i, row in enumerate(rows):
            for c_i, cell in enumerate(row):
                if cell is None:
                    continue
                if isinstance(cell, tuple):
                    kind, pname, caption = cell
                else:
                    pname, caption = cell, None
                    p = params[pname]
                    if p["kind"] == "bool" or (p["kind"] == "choice" and len(p["choices"]) == 2):
                        kind = "toggle"
                    elif pname in STEPS:
                        kind = "knob_step"
                    else:
                        kind = "knob_bi" if is_bipolar(pname) else "knob"
                x, y, w, h = COL_X[c_i], ys[r_i], COL_W, CELL_H
                pidx = index[pname]
                p = params[pname]
                if p["kind"] == "choice" and len(p["choices"]) > 1 and pname not in DYNAMIC:
                    replacement[pidx] = p["choices"]
                remap = [("Data", "Parameter %d" % pidx)]
                if kind == "knob_named":
                    cname = comp_knob_named(w, h)
                    remap += [("Title", "Parameter %d" % index[caption])]
                elif kind == "knob_step":
                    cname = comp_knob_step(w, h, False)
                    remap += [("Dec", "Parameter %d" % index[STEPS[pname][0]]), ("Inc", "Parameter %d" % index[STEPS[pname][1]])]
                elif kind in ("knob", "knob_bi"):
                    cname = comp_knob(w, h, kind == "knob_bi")
                elif kind == "toggle":
                    cname = comp_toggle(w, h)
                    if p["kind"] == "bool":
                        replacement[pidx] = ["Off", "On"]
                elif kind == "tone":
                    cname = comp_tone(w, h, caption)
                else:   # lcd: a small screen showing the value (no control)
                    gw2, gh2 = w - 24, 96
                    paste(img, lcd_glass(gw2, gh2), x + 12, y + 30)
                    d = ImageDraw.Draw(img)
                    d.text(((x + 14) * SS, (y + 2) * SS), caption, font=font(17), fill=SILK + (255,))
                    rect = (x + 22, y + 30, gw2 - 20, gh2)
                    children.append(lcd_entry(pname, rect, 26, "left verticallyCentred"))
                    prev_items.append(("lcd", pname, rect, 26, "left verticallyCentred"))
                    continue
                children.append(entry(pname, cname, obj(("version", 1), ("handleName", "Data")), (x, y, w, h),
                                      remap=remap, focus="Yes", invalid="Hide"))
                q = 13 + c_i - 4 * r_i
                if qlinks.get(q, -1) == -1:
                    qlinks[q] = pidx
                prev_items.append((kind, pname, (x, y, w, h), caption, None))
            if r_i < len(rows) - 1:
                yl = ys[r_i] + CELL_H + 5
                d.line((20 * SS, yl * SS, (W - 20) * SS, yl * SS), fill=PANEL_HI + (128,), width=SS)
        bg_name = "page_%02d.png" % (tab_i + 1)
        img.resize((W, H), Image.LANCZOS).convert("RGB").save(os.path.join(OUT, bg_name), optimize=True)
        root = obj(("version", 2), ("actions", []),
                   ("backgroundData", obj(("version", 1), ("focussed", obj(("version", 1), ("colour", argb(PANEL)), ("image", ""))),
                                          ("unfocussed", obj(("version", 1), ("colour", argb(PANEL)), ("image", ""))))),
                   ("ignoreMousePresses", False), ("disableCoarseDataWheel", False),
                   ("componentsData", [entry("panel", "Image", obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", bg_name)),
                                             (0, 0, W, H))] + children))
        y0 = ys[0] - 4
        tabs.append(obj(("version", 1), ("tabName", tab), ("fnKeyIndex", fn), ("fnKeySubIndex", sub), ("componentDefinition", root),
                        ("qlinkBoundsData", ["%d %d %d %d" % (cx - 3, y0, COL_W + 6, ys[len(rows) - 1] + CELL_H - y0 + 4) for cx in COL_X])))
        qmaps.append(obj(("Tab", fn + 1), ("SubTab", sub + 1), ("Bank Direction", "Column"),
                         ("Q-Links", O([("Q-Link %d" % q, qlinks[q]) for q in range(1, 17)]))))
        previews.append((tab_i, tab, bg_name, prev_items))

    defs["knob overlay"] = knob_overlay("Unipolar")
    defs["knob overlay bipolar"] = knob_overlay("Bipolar")
    tui = obj(("pageData", obj(
        ("version", 1),
        ("componentDefinitions", obj(("version", 2), ("importFiles", []),
                                     ("localComponentDefinitions", [obj(("key", k), ("value", v)) for k, v in defs.items()]))),
        ("replacementStrings", [obj(("key", "Parameter %d" % i), ("value", v)) for i, v in sorted(replacement.items())]),
        ("info", obj(("version", 1), ("type", "CompleteDescription"))),
        ("tabs", tabs))))
    open(os.path.join(OUT, "TUI.json"), "w").write(dumps(tui) + "\n")
    program_qlinks = ["Bank", "Patch", "Octave", "Volume", "Cutoff", "Resonance", "Attack", "Release",
                      "Tone 1", "Tone 2", "Tone 3", "Tone 4", "EFX", "Chorus", "Reverb", "Voices"]
    qfile = obj(("version", 4), ("info", obj(("version", 1), ("type", "CompleteDescription"))),
                ("Screen Mode Q-Links", obj(("version", 4), ("map", qmaps))),
                ("Program Mode Q-Links", O([("Q-Link %d" % (i + 1), index[n]) for i, n in enumerate(program_qlinks)])))
    open(os.path.join(OUT, "Q-Links.json"), "w").write(dumps(qfile) + "\n")
    make_previews(previews, params)
    n_png = len([f for f in os.listdir(OUT) if f.endswith(".png")])
    print("skins/: %d tabs on %d function keys, %d components, %d images; previews in preview/" % (len(tabs), len(sub_of), len(defs), n_png))


SAMPLE_SCREEN = {"Bank": "EXP-A  Orchestral", "Patch": "001 Fat Strings", "Octave": "+0", "Edit Tone": "TONE 1",
                 "Slot A": "Keys of 60s & 70s", "Slot B": "Special FX", "Slot C": "Empty", "Slot D": "Empty", "Wave": "052 Organ Bass 2"}


def make_previews(previews, params):
    """Each page with sample values (the MPC's own text is approximated with a local font)."""
    knob = {k: Image.open(os.path.join(OUT, "knob_%s.png" % k)).convert("RGBA") for k in ("uni", "bi")}
    imgs = {n: Image.open(os.path.join(OUT, n + ".png")).convert("RGBA") for n in ("toggle_on", "toggle_off", "button_on", "button_off")}
    f = lambda s, b=True: ImageFont.truetype(FONT_B if b else FONT_R, int(s))
    for tab_i, tab, bg_name, items in previews:
        img = Image.open(os.path.join(OUT, bg_name)).convert("RGBA")
        d = ImageDraw.Draw(img)
        for kind, pname, rect, extra, just in items:
            x, y, w, h = rect
            p = params[pname]
            disp = SAMPLE_SCREEN.get(pname, p["display"])
            if pname.startswith("Tone") and kind == "lcd":
                disp = "ON"
            if kind == "lcd":
                size = extra * 0.85
                ax = x + w if just.startswith("right") else x
                d.text((ax, y + h / 2), disp, font=f(size), fill=LCD_INK, anchor=("rm" if just.startswith("right") else "lm"))
            elif kind == "strip":
                bid, group = extra, just
                on = (bid == 0) if group > 1 else True
                img.alpha_composite(imgs["button_on" if on else "button_off"], (x, y))
            elif kind == "filter":
                strip = Image.open(os.path.join(OUT, "filter_lpf.png")).convert("RGBA")
                fr = 20
                img.alpha_composite(strip.crop((0, fr * h, w, fr * h + h)), (x, y))
            elif kind == "env":
                d.text((x + 10, y + 6), extra, font=f(15), fill=SILK_DIM)
                pts = [(x + 10, y + h - 12), (x + w * 0.12, y + 30), (x + w * 0.35, y + 30 + (h - 42) * 0.4),
                       (x + w * 0.7, y + 30 + (h - 42) * 0.4), (x + w - 10, y + h - 12)]
                d.polygon(pts, fill=ACCENT + (64,))
                d.line(pts, fill=ACCENT, width=3)
                for p_ in pts[1:4]:
                    d.ellipse((p_[0] - 7, p_[1] - 7, p_[0] + 7, p_[1] + 7), outline=(255, 255, 255), width=2)
            elif kind == "knob_named":
                fr = 40
                img.alpha_composite(knob["uni"].crop((0, fr * 90, 90, fr * 90 + 90)), (x + 14, y + (h - 90) // 2))
                caps = ["MODE", "DELAY L", "DELAY R", "PHASE L", "PHASE R", "FBK", "HF DAMP", "LOW GAIN", "HI GAIN", "BALANCE", "LEVEL", "---"]
                vals = ["NORMAL", "250 ms", "380 ms", "NORMAL", "INVERT", "+40%", "4000 Hz", "+0", "-3", "D70:30E", "127", ""]
                k = int(pname.split()[1]) - 1
                d.text((x + 116, y + 38), caps[k], font=f(18), fill=SILK_DIM, anchor="lm")
                d.text((x + 116, y + 86), vals[k], font=f(23, False), fill=VALUE, anchor="lm")
            elif kind == "knob_step":
                fr = int(round(p["default"] * 126))
                img.alpha_composite(knob["uni"].crop((0, fr * 90, 90, fr * 90 + 90)), (x + 14, y + (h - 90) // 2))
                d.text((x + 116, y + 23), pname.upper(), font=f(18), fill=SILK_DIM, anchor="lm")
                d.text((x + 116, y + 61), disp, font=f(23, False), fill=VALUE, anchor="lm")
                for k, sign in enumerate(("dec", "inc")):
                    img.alpha_composite(Image.open(os.path.join(OUT, "step_%s_off.png" % sign)).convert("RGBA"), (x + 116 + k * (STEP_W + 12), y + h - STEP_H - 8))
            elif kind in ("knob", "knob_bi"):
                strip = knob["bi" if kind == "knob_bi" else "uni"]
                fr = int(round(p["default"] * 126)) if kind == "knob" else 63
                img.alpha_composite(strip.crop((0, fr * 90, 90, fr * 90 + 90)), (x + 14, y + (h - 90) // 2))
                d.text((x + 116, y + 38), pname.upper(), font=f(18), fill=SILK_DIM, anchor="lm")
                d.text((x + 116, y + 86), disp, font=f(23, False), fill=VALUE, anchor="lm")
            elif kind == "toggle":
                on = p["default"] >= 0.5
                img.alpha_composite(imgs["toggle_on" if on else "toggle_off"], (x + 20, y + (h - 32) // 2))
                d.text((x + 100, y + 47), pname.upper(), font=f(18), fill=SILK_DIM, anchor="lm")
                d.text((x + 100, y + 86), disp, font=f(23, False), fill=VALUE, anchor="lm")
            elif kind == "tone":
                img.alpha_composite(imgs["button_on" if p["default"] >= 0.5 else "button_off"], (x + (w - 120) // 2, y + 52))
                d.text((x + w / 2, y + 28), extra, font=f(19), fill=SILK, anchor="mm")
        img.convert("RGB").save(os.path.join(PREVIEW, "%02d_%s.png" % (tab_i + 1, tab.replace("/", "-"))))


if __name__ == "__main__":
    main()
