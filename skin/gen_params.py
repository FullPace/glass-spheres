#!/usr/bin/env python3
"""Rewrites the modulation part of ../params.json (2 envelopes, 2 LFOs, 8 matrix slots), between the base params and
manual_page (which stays last).

The keys and order must match InitModParams() in src/engine.cc; the option lists match the enums in src/mod.h.

    python3 skin/gen_params.py
"""
import json
import os
import re

ENV_TRIGGERS = ["MIDI", "LFO 1", "LFO 2", "T1", "T2", "T3"]
LFO_SHAPES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth Rnd"]
DIVISIONS = ["4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32"]
SOURCES = ["Off", "LFO 1", "LFO 2", "Env 1", "Env 2", "T1", "T2", "T3", "X1", "X2", "X3", "Y", "Velocity"]
DESTS = ["Off", "T Rate", "T Bias", "Jitter", "Deja Vu", "Steps", "Spread", "X Bias", "T Clock", "X Clock"]
MOD_KEY = re.compile(r"^(env\d|lfo\d|mod\d|manual_)")


def mod_params():
    out = []
    for e in (1, 2):
        out += [
            {"key": "env%d_attack" % e, "name": "Env %d Attack" % e, "min": 0.0, "max": 1.0, "default": 0.1,
             "dynamic_display": True},
            {"key": "env%d_decay" % e, "name": "Env %d Decay" % e, "min": 0.0, "max": 1.0, "default": 0.5,
             "dynamic_display": True},
            {"key": "env%d_sustain" % e, "name": "Env %d Sustain" % e, "min": 0.0, "max": 1.0, "default": 0.7},
            {"key": "env%d_release" % e, "name": "Env %d Release" % e, "min": 0.0, "max": 1.0, "default": 0.5,
             "dynamic_display": True},
            {"key": "env%d_trig" % e, "name": "Env %d Trigger" % e, "options": ENV_TRIGGERS, "default": 0},
        ]
    for l in (1, 2):
        out += [
            {"key": "lfo%d_shape" % l, "name": "LFO %d Shape" % l, "options": LFO_SHAPES, "default": 0},
            {"key": "lfo%d_rate" % l, "name": "LFO %d Rate" % l, "min": 0.0, "max": 1.0, "default": 0.5,
             "dynamic_display": True},
            {"key": "lfo%d_sync" % l, "name": "LFO %d Sync" % l, "options": ["Off", "On"], "default": 0},
            {"key": "lfo%d_div" % l, "name": "LFO %d Division" % l, "options": DIVISIONS, "default": 4},
        ]
    for m in range(1, 9):
        out += [
            {"key": "mod%d_src" % m, "name": "Mod %d Source" % m, "options": SOURCES, "default": 0},
            {"key": "mod%d_dst" % m, "name": "Mod %d Dest" % m, "options": DESTS, "default": 0},
            {"key": "mod%d_amt" % m, "name": "Mod %d Amount" % m, "min": -100.0, "max": 100.0, "unit": "%",
             "display": "int", "default": 0.0},
        ]
    return out


def main():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "params.json")
    d = json.load(open(path))
    manual = [p for p in d["params"] if p["key"] == "manual_page"]
    base = [p for p in d["params"] if not MOD_KEY.match(p["key"])]
    d["params"] = base + mod_params() + manual
    d["sections"] = [s for s in d.get("sections", []) if s["label"] not in ("Envelopes", "LFOs", "Matrix")]
    d["sections"] += [
        {"label": "Envelopes", "keys": [p["key"] for p in mod_params() if p["key"].startswith("env")]},
        {"label": "LFOs", "keys": [p["key"] for p in mod_params() if p["key"].startswith("lfo")]},
        {"label": "Matrix", "keys": [p["key"] for p in mod_params() if p["key"].startswith("mod")]},
    ]
    json.dump(d, open(path, "w"), indent=2)
    open(path, "a").write("\n")
    print("params.json: %d params" % len(d["params"]))


if __name__ == "__main__":
    main()
