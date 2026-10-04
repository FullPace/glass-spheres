#!/usr/bin/env python3
"""Writes ../layout.conf: the Glass Spheres skin, in the look of Overcast (same knobs, tags and background frame).

GLASS SPHERES: the T model as a selector at the top, a grid of T / X / Y / DEJA VU knobs, a right column of lists.
Y / CLOCK: the Y section, the clock options, gate length and the MIDI trigger.
MODULATION: two envelopes, two LFOs and an 8-slot matrix onto Marbles' CV inputs (as on Overcast).
OUTPUT: CV or MIDI; below it the jack assignment (when=output:cv) or the MIDI settings (when=output:midi).
MANUAL: a topic list and the topic's text (when=manual_page:<i>).

    python3 skin/gen_layout.py
"""
import json
import os

T_MODELS = ["COIN TOSS", "CLUSTERS", "DRUMS", "INDEPENDENT", "DIVIDER", "THREE STATES", "MARKOV"]

# The background (skin/glass_spheres_bg.jpg, the user's design) has a title band at the top and a border band at the
# bottom; controls stay in between: layout y 196..662 (image y 110..576).
BACKGROUND = "art file=skin/glass_spheres_bg.jpg"
GRID_X = [128, 384, 640, 896, 1152]   # five evenly spaced columns, as on Overcast
ROWS = [292, 422, 552]
R = 38
RIGHT = GRID_X[4]
COLS4 = [160, 480, 800, 1120]          # four columns for the list pages
TAG_W, TAG_H = 132, 26
NAME_GAP = 14

# GLASS SPHERES tab, laid out like the module's panel: the T section on the left (RATE big, BIAS, JITTER, T model),
# DEJA VU and LENGTH in the middle with the T / X range beside LENGTH, the X section on the right (SPREAD big, BIAS,
# STEPS, X mode); the module's t and X buttons (deja vu per section) at the top corners.
PC = [128, 330, 640, 950, 1152]          # panel columns
BIG_R = 50
QLINKS_MAIN = ["t_rate", "t_bias", "t_jitter", "t_model",
               "deja_vu", "length", "t_deja_vu", "x_deja_vu",
               "x_spread", "x_bias", "x_steps", "x_mode",
               "x_scale", "t_range", "x_range", "clock"]

# Y / CLOCK tab: (column x, title) and its controls
YC_KNOBS = [(GRID_X[0], [("y_spread", "Y SPREAD", "teal"), ("y_bias", "Y BIAS", None), ("y_steps", "Y STEPS", None)]),
            (GRID_X[3], [("t_pw", "GATE LENGTH", None), ("t_pw_random", "GATE RANDOM", None),
                         ("midi_note", "TRIGGER NOTE", None)])]
YC_LISTS = [(GRID_X[1], [("y_divider", "Y DIVIDER"), ("y_range", "Y RANGE"), ("x_clock", "X CLOCK")]),
            (GRID_X[2], [("clock", "CLOCK"), ("clock_div", "MPC CLOCK"), ("midi_trigger", "MIDI TRIGGER")])]
YC_LIST_ROWS = [292, 422, 552]
QLINKS_YC = ["y_spread", "y_bias", "y_steps", "y_divider",
             "y_range", "x_clock", "clock", "clock_div",
             "midi_trigger", "t_pw", "t_pw_random", "midi_note"]

MOD_KNOB_R = 24
SECTION_X, SECTION_W, SECTION_H = 55, 90, 34
QLINKS_MOD = ["env1_attack", "env1_decay", "env1_sustain", "env1_release",
              "env2_attack", "env2_decay", "env2_sustain", "env2_release",
              "lfo1_rate", "lfo1_shape", "lfo2_rate", "lfo2_shape",
              "mod1_amt", "mod2_amt", "mod3_amt", "mod4_amt"]

THEME = [
    "art_css=skin/glass.css",
    "theme_bg=e6e6e3", "theme_panel=e6e6e3", "theme_line=b4b5b4", "theme_ink=1a1919", "theme_ink_dim=4a4b4a",
    "theme_ink_faint=b4b5b4", "theme_accent=c83d58", "theme_accent_hi=c83d58", "theme_knob_face=e9e9e6",
    "theme_knob_ring=d7d8d6", "theme_knob_dot=1a1919", "theme_lcd=ffffff", "theme_seg_active=009797",
    "theme_seg_inactive=f4f4f2", "theme_seg_active_tx=ffffff", "theme_box=ffffff", "theme_btn_bg=c83d58",
    "theme_btn_text=ffffff", "theme_btn_text_plain=ffffff",
]

MANUAL = [
    ("BASICS", [
        "Glass Spheres is a random sampler after Mutable Instruments Marbles: it makes random gates (T section),",
        "random voltages or notes (X section) and a slow random modulation (Y), and it can repeat what it",
        "made (DEJA VU). It makes no sound itself: it plays synths over the MPC X's CV jacks or over MIDI.",
        "",
        "GLASS SPHERES tab: laid out like the module: T on the left, DEJA VU in the middle, X on the right.",
        "Y / CLOCK: the Y section, clock and gates. MODULATION: envelopes, LFOs, matrix. OUTPUT: CV or MIDI.",
        "",
        "Quick start (MIDI): OUTPUT = MIDI, make a MIDI track with input Glass Spheres 1 and an instrument,",
        "set its monitoring on. Quick start (CV): OUTPUT = CV, X1 to CV 1 (pitch), T1 to CV 2 (gate).",
    ]),
    ("T SECTION", [
        "Random gates on three outputs: t1, t2 (the clock itself) and t3.",
        "",
        "T RATE: the clock speed (with an external clock: multiplies or divides it).",
        "T BIAS: which of t1 / t3 gets more gates (Coin Toss), or the pattern's density in other models.",
        "JITTER: shifts the gates off the grid, up to fully random timing.",
        "GATE LENGTH / GATE RANDOM (Y / CLOCK tab): the gates' length and how much it varies.",
        "T MODEL: Coin Toss, Clusters, Drums, Independent, Divider, Three States, Markov:",
        "how t1 and t3 relate to the clock (e.g. Drums plays kick/snare-like patterns).",
        "T RANGE (beside LENGTH): the clock's range, 1/4x, 1x or 4x.",
    ]),
    ("X SECTION", [
        "Three random voltages X1, X2, X3: melodies when quantized.",
        "",
        "X SPREAD: how far the values spread (low: close to the bias, high: anywhere).",
        "X BIAS: the centre of the values (low notes / high notes).",
        "X STEPS: below 12 o'clock smooth glides, above it quantized to the SCALE; fully right = only the",
        "scale's most important notes.",
        "SCALE: Major, Minor, Pentatonic, Pelog, Bhairav, Shri.",
        "X MODE: Identical (all three alike), Bump (X2 in the middle), Tilt (X1 to X3 tilted).",
        "X CLOCK (Y / CLOCK tab): which gate updates X: T1+T2+T3 (each its own), or all from T1, T2 or T3.",
    ]),
    ("Y & DEJA VU", [
        "Y: a fourth random voltage, slower: for filters, levels, effect sends.",
        "Y SPREAD, Y BIAS, Y STEPS as for X. Y DIVIDER: Y changes every n clock pulses (1/1 .. 1/64).",
        "",
        "DEJA VU: 12 o'clock = the last LENGTH steps repeat exactly (a loop). Left of it: new random values",
        "mixed in more and more, fully left = always new. Right of it: the loop is shuffled.",
        "LENGTH: the loop length in steps (1 .. 16).",
        "t DEJA VU / X DEJA VU (top corners): Off, On (follows the knob) or Locked (always repeats) for the",
        "gates and the voltages separately: e.g. a fixed rhythm with new notes.",
    ]),
    ("CLOCK", [
        "CLOCK: Internal (T RATE alone), MPC (locked to the MPC's tempo and transport) or MIDI.",
        "",
        "MPC: the pattern restarts when the MPC starts playing; MPC CLOCK sets the note value of",
        "the clock (1/4 .. 1/32), T RATE then multiplies or divides it. Nothing plays while the MPC is stopped.",
        "MIDI: notes on Glass Spheres' own track clock it, one step per note: draw the rhythm in the piano",
        "roll. MIDI TRIGGER: Any Note, One Note (only TRIGGER NOTE), or Learn (the next note you play",
        "becomes the trigger note).",
    ]),
    ("MODULATION", [
        "Two envelopes, two LFOs and an 8-slot matrix, like patch cables into the module's CV inputs.",
        "",
        "Sources: LFO 1/2, Env 1/2, Glass Spheres' own outputs T1-T3, X1-X3 and Y (patched back into its",
        "inputs, as on the module), and the velocity of notes on its own track.",
        "Destinations: T Rate, T Bias, Jitter, Deja Vu, Steps, Spread, X Bias (added to the knobs), and",
        "T Clock / X Clock: a slot there clocks that section like a cable in the module's CLOCK jack (on above 50 %).",
        "ENV TRIGGER: MIDI notes, LFO 1 / LFO 2 or the gates T1-T3. LFO SYNC locks the rate to the MPC's tempo.",
        "Ideas: Y onto T Rate (a drifting tempo), Env 1 triggered by T1 onto Spread, X1 onto Deja Vu.",
    ]),
    ("CV OUTPUT", [
        "OUTPUT = CV: the seven outputs go to the MPC X's CV/Gate jacks (0 .. 5 V). Assign each one to a jack",
        "on the OUTPUT tab; Off leaves it unused. Several instances can share the jacks: the latest assignment",
        "of a jack wins.",
        "",
        "Gates (T1-T3): 5 V while open. X1-X3 and Y: 1 V/octave with X RANGE 0-2V or 0-5V; +/-5V is",
        "squeezed into 0-5 V (smooth, but no longer 1 V/octave).",
        "Don't use the same jacks on CV tracks: the MPC sets those to 0 V when the transport stops.",
        "Typical: X1 to a VCO's pitch, T1 to its envelope's gate, Y to the filter cutoff.",
    ]),
    ("MIDI OUTPUT", [
        "OUTPUT = MIDI: every Glass Spheres has its own MIDI output, Glass Spheres 1, Glass Spheres 2, ...",
        "On another track (plug-in, keygroup, drum...) pick it as MIDI input, the voice's channel, monitoring on.",
        "",
        "Like patching the module's outputs to different modules: three VOICES and two CC lanes.",
        "VOICE: GATE (T1-T3) opens a note, PITCH picks X1-X3 or Y at 1 V = 12 semitones above NOTE, or",
        "Fixed (always NOTE: drums, e.g. 36 kick on T2, 38 snare on T3). CHANNEL: one track per channel.",
        "CC: SOURCE X1-X3 or Y over its range as 0..127, on CHANNEL as controller CC (1 mod wheel, 74 cutoff).",
        "Use SCALE and STEPS above 12 o'clock for notes in key; X CLOCK = T1+T2+T3 gives each voice its own.",
    ]),
]


def tag(cx, cy, colour, w=TAG_W, h=TAG_H):
    """Pill images keep their aspect ratio on screen, so other sizes need their own image (pill_<colour>_section)."""
    return "art file=skin/pill_%s.png x=%d y=%d w=%d h=%d" % (colour, cx - w // 2, cy - h // 2, w, h)


def text(cx, cy, label, colour, size=1.5, when=None, weight=700, align=None):
    line = 'text cx=%d cy=%d label="%s" size=%s weight=%d color=%s' % (cx, cy, label, size, weight, colour)
    line += " align=%s" % align if align else ""
    return line + (" when=" + when if when else "")


def knob(key, cx, cy, r, label, colour=None, when=None):
    img = colour if colour in ("raspberry", "teal") else "white"
    out = [tag(cx, cy + r + NAME_GAP, colour) + (" when=" + when if when else "")] if colour else []
    out.append(text(cx, cy + r + NAME_GAP, label, "ffffff" if colour else "1a1919", when=when))
    line = 'knob cx=%d cy=%d r=%d label="" key=%s img=skin/knob_%s.png' % (cx, cy, r, key, img)
    return out + [line + (" when=" + when if when else "")]


def listbox(key, cx, cy, label, w=190, when=None):
    """A drop-down list with a small dark title above it."""
    out = [text(cx, cy - 30, label, "1a1919", size=1.2, when=when),
           'popup cx=%d cy=%d w=%d h=40 label="" key=%s' % (cx, cy, w, key)]
    if when:
        out[1] += " when=" + when
    return out


def main_page():
    L = ["[tab GLASS SPHERES]", BACKGROUND]
    # top corners: the module's t and X buttons (deja vu on / off / locked per section); DEJA VU knob in the middle
    L += listbox("t_deja_vu", PC[0], 236, "t DEJA VU", w=170)
    L += listbox("x_deja_vu", PC[4], 236, "X DEJA VU", w=170)
    L += knob("deja_vu", PC[2], 250, R, "DEJA VU", "raspberry")
    # second row: T model, RATE, T range | LENGTH | X range, SPREAD, X mode
    L += listbox("t_model", PC[0], 380, "T MODEL", w=170)
    L += knob("t_rate", PC[1], 360, BIG_R, "RATE", "raspberry")
    L += listbox("t_range", 488, 420, "T RANGE", w=110)
    L += listbox("length", PC[2], 420, "LENGTH", w=150)
    L += listbox("x_range", 792, 420, "X RANGE", w=110)
    L += knob("x_spread", PC[3], 360, BIG_R, "SPREAD", "teal")
    L += listbox("x_mode", PC[4], 380, "X MODE", w=170)
    # bottom row: BIAS, JITTER | SCALE | STEPS, BIAS
    L += knob("t_bias", PC[0], 540, R, "BIAS")
    L += knob("t_jitter", PC[1], 540, R, "JITTER")
    L += listbox("x_scale", PC[2], 540, "SCALE", w=150)
    L += knob("x_steps", PC[3], 540, R, "STEPS")
    L += knob("x_bias", PC[4], 540, R, "BIAS")
    L.append('qlinks "GLASS SPHERES" = ' + ",".join(QLINKS_MAIN))
    return L


def yc_page():
    L = ["[tab Y / CLOCK]", BACKGROUND]
    for x, title, colour in ((256, "Y", "teal"), (GRID_X[2], "CLOCK", "raspberry"), (GRID_X[3], "GATES", "raspberry")):
        L += [tag(x, 208, colour), text(x, 208, title, "ffffff")]
    for x, items in YC_KNOBS:
        for j, (key, label, colour) in enumerate(items):
            L += knob(key, x, YC_LIST_ROWS[j] + 10, R, label, colour)
    for x, items in YC_LISTS:
        for j, (key, label) in enumerate(items):
            L += listbox(key, x, YC_LIST_ROWS[j] + 10, label)
    L.append('qlinks "Y / CLOCK" = ' + ",".join(QLINKS_YC))
    return L


def mod_page():
    """As on Overcast: two envelopes and two LFOs on the left, the 8-slot matrix on the right."""
    L = ["[tab MODULATION]", BACKGROUND]
    rows = [238, 359]
    lfo_rows = [506, 611]
    ex = [170, 295, 420, 545]
    for e in (1, 2):
        cy = rows[e - 1]
        L += [tag(SECTION_X, cy, "raspberry_section", SECTION_W, SECTION_H),
              text(SECTION_X, cy, "ENV %d" % e, "ffffff", size=2)]
        for i, (p, lab) in enumerate((("attack", "ATTACK"), ("decay", "DECAY"), ("sustain", "SUSTAIN"),
                                      ("release", "RELEASE"))):
            L += knob("env%d_%s" % (e, p), ex[i], cy, MOD_KNOB_R, lab)
        L += [text(690, cy - 30, "TRIGGER", "1a1919", size=1.2),
              'popup cx=690 cy=%d w=124 h=36 label="" key=env%d_trig' % (cy, e)]
    for l in (1, 2):
        cy = lfo_rows[l - 1]
        L += [tag(SECTION_X, cy, "teal_section", SECTION_W, SECTION_H),
              text(SECTION_X, cy, "LFO %d" % l, "ffffff", size=2)]
        L.append('popup cx=205 cy=%d w=150 h=36 label="" key=lfo%d_shape' % (cy, l))
        L += [text(345, cy + 13 + 14, "SYNC", "1a1919"), 'toggle cx=345 cy=%d label="" key=lfo%d_sync' % (cy - 8, l)]
        L.append('knob cx=465 cy=%d r=%d label="" key=lfo%d_rate img=skin/knob_white.png' % (cy - 10, MOD_KNOB_R, l))
    x0 = 770
    L += [text(x0 + 100, 208, "SOURCE", "636463", size=1.2), text(x0 + 260, 208, "DESTINATION", "636463", size=1.2),
          text(x0 + 420, 208, "AMOUNT", "636463", size=1.2)]
    for k in range(1, 9):
        cy = 238 + (k - 1) * 54
        L += [text(x0 + 8, cy, str(k), "1a1919", size=1.4),
              'popup cx=%d cy=%d w=140 h=36 label="" key=mod%d_src' % (x0 + 100, cy, k),
              'popup cx=%d cy=%d w=150 h=36 label="" key=mod%d_dst' % (x0 + 260, cy, k),
              'slider_h cx=%d cy=%d w=100 h=20 label="" key=mod%d_amt' % (x0 + 420, cy - 8, k)]
    L.append('qlinks "MODULATION" = ' + ",".join(QLINKS_MOD))
    return L


def output_page():
    L = ["[tab OUTPUT]", BACKGROUND,
         'enum_h cx=640 cy=216 label="" key=output sw=200 options="CV,MIDI"']
    cv, midi = "output:cv", "output:midi"
    # CV: gates in the first row, voltages in the second
    for i, (key, label) in enumerate((("out_t1", "T1"), ("out_t2", "T2"), ("out_t3", "T3"))):
        L += listbox(key, COLS4[i], 330, label + " (GATE)", when=cv)
    for i, (key, label) in enumerate((("out_x1", "X1"), ("out_x2", "X2"), ("out_x3", "X3"), ("out_y", "Y"))):
        L += listbox(key, COLS4[i], 470, label, when=cv)
    L.append(text(640, 580, "CV / Gate jacks of the MPC X, 0 .. 5 V. The latest assignment of a jack wins.",
                  "4a4b4a", size=1.25, weight=600, when=cv))
    # MIDI: a routing table, three voices (gate, pitch source, channel, note) and two CC lanes (source, channel, CC)
    cols = [300, 500, 700, 900]
    for x, h in zip(cols, ("GATE", "PITCH", "CHANNEL", "NOTE")):
        L.append(text(x, 246, h, "636463", size=1.2, when=midi))
    for v in range(3):
        cy = 284 + v * 74
        L += [tag(130, cy, "raspberry_section", SECTION_W, SECTION_H) + " when=" + midi,
              text(130, cy, "VOICE %d" % (v + 1), "ffffff", size=1.5, when=midi)]
        L += ['popup cx=%d cy=%d w=150 h=36 label="" key=v%d_gate when=%s' % (cols[0], cy, v + 1, midi),
              'popup cx=%d cy=%d w=150 h=36 label="" key=v%d_pitch when=%s' % (cols[1], cy, v + 1, midi),
              'popup cx=%d cy=%d w=120 h=36 label="" key=v%d_channel when=%s' % (cols[2], cy, v + 1, midi),
              'slider_h cx=%d cy=%d w=100 h=20 label="" key=v%d_note when=%s' % (cols[3], cy - 8, v + 1, midi)]
    L += [text(cols[0], 494, "SOURCE", "636463", size=1.2, when=midi),
          text(cols[2], 494, "CHANNEL", "636463", size=1.2, when=midi),
          text(cols[3], 494, "CC", "636463", size=1.2, when=midi)]
    for c in range(2):
        cy = 532 + c * 74
        L += [tag(130, cy, "teal_section", SECTION_W, SECTION_H) + " when=" + midi,
              text(130, cy, "CC %d" % (c + 1), "ffffff", size=1.5, when=midi)]
        L += ['popup cx=%d cy=%d w=150 h=36 label="" key=cc%d_source when=%s' % (cols[0], cy, c + 1, midi),
              'popup cx=%d cy=%d w=120 h=36 label="" key=cc%d_channel when=%s' % (cols[2], cy, c + 1, midi),
              'slider_h cx=%d cy=%d w=100 h=20 label="" key=cc%d_number when=%s' % (cols[3], cy - 8, c + 1, midi)]
    L.append(text(1110, 400, "Port:", "636463", size=1.2, when=midi))
    L.append(text(1110, 428, "Glass Spheres 1, 2 ...", "4a4b4a", size=1.25, weight=600, when=midi))
    L.append('qlinks "OUTPUT" = v1_gate,v1_pitch,v1_channel,v1_note,v2_gate,v2_pitch,v2_channel,v2_note,'
             'v3_gate,v3_pitch,v3_channel,v3_note,cc1_source,cc1_channel,cc1_number,output')
    return L


def manual_page():
    L = ["[tab MANUAL]", BACKGROUND,
         'enum_v cx=150 cy=430 label="" key=manual_page sw=220 options="%s"' % ",".join(t for t, _ in MANUAL)]
    for i, (title, lines) in enumerate(MANUAL):
        when = "manual_page:%d" % i
        L.append(text(320, 214, title, "c83d58", size=1.8, when=when, align="left"))
        for j, line in enumerate(lines):
            if line:
                L.append(text(320, 254 + j * 35, line.replace('"', "'"), "1a1919", size=1.45, when=when, weight=600,
                              align="left"))
    L.append('qlinks "MANUAL" = manual_page')
    return L


def qlink_bounds():
    """Per tab, the screen area of each Q-Link column (MPC X 4x4 grid; the MPC highlights it while a Q-Link turns).
    Skin coordinates are layout y - 86. Written to skin/qlink_bounds.json for post_build.py."""
    def rect(x0, y0, x1, y1):
        return "%d %d %d %d" % (x0, y0 - 86, x1 - x0, y1 - y0)
    none = rect(0, 196, 1, 197)
    main = [rect(30, 196, 420, 664), rect(420, 196, 860, 664), rect(860, 196, 1250, 664), none]
    yc = [rect(30, 196, 500, 664), rect(500, 196, 760, 664), rect(760, 196, 1020, 664), none]
    mod = [rect(110, 208, 610, 326), rect(110, 327, 610, 445), rect(10, 466, 530, 660), rect(760, 220, 1260, 420)]
    return [main, yc, mod, [none] * 4, [rect(30, 196, 270, 664)] + [none] * 3]


def main():
    L = ["# Glass Spheres skin. Generated by skin/gen_layout.py -- edit that, not this file."] + THEME + [""]
    L += main_page() + [""] + yc_page() + [""] + mod_page() + [""] + output_page() + [""] + manual_page()
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "layout.conf")
    open(path, "w").write("\n".join(L) + "\n")
    print("wrote", os.path.normpath(path))
    json.dump(qlink_bounds(), open(os.path.join(here, "qlink_bounds.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
