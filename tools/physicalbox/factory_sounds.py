#!/usr/bin/env python3
"""PhysicalBox's factory sounds: one table, two outputs.

  python3 factory_sounds.py cpp      -> the C++ list for scPhysicalBox.cpp (factorySounds)
  python3 factory_sounds.py scd      -> render cases for render_models.scd (PB_CASES file)
  python3 factory_sounds.py levels <dir>  -> Level (dB) per sound from rendered WAVs

Values: PhysicalVoice inputs by name (PhysicalModes.h), "level" (dB), or any
node control by its escaped key (Gate_Mode, Pulse_Rate, ...): those the
renders skip.
"""
import json, math, pathlib, sys

PULSE = {"Gate_Mode": 3}
S = [
    # ── Strings & winds: the waveguide A alone ─────────────────────────────
    ("init", {}),
    ("Struck string", {"excite": 0, "listen": 0, "adecay": 3, "hardness": 0.6, "symp": 0.2, "sympset": 4, "sympdecay": 4}),
    ("Box guitar", {"excite": 0, "listen": 0, "adecay": 3, "hardness": 0.7, "pos": 0.16, "radiator": 3, "radmix": 0.55, "damper": 0.25, "symp": 0.25, "sympset": 3, "sympdecay": 3}),
    ("Stiff string", {"excite": 0, "listen": 0, "astiff": 0.35, "adecay": 5, "hardness": 0.75, "abright": 0.8, "mass": 0.4, "radiator": 3, "radmix": 0.3, "damper": 0.6, "symp": 0.3, "sympset": 4, "sympdecay": 5, "sympshimmer": 2}),
    # Sitar / Tanpura: the jawari bridge's buzz and bloom, the taraf ringing on (Sympathetic, Taraf)
    ("Sitar", {"excite": 0, "listen": 0, "adecay": 5, "hardness": 0.85, "pos": 0.08, "radiator": 2, "radmix": 0.5, "material": 1, "abright": 0.9,
               "jawari": 0.8, "symp": 0.7, "sympset": 0, "sympdecay": 6, "sympbright": 0.7, "sympshimmer": 3}),
    ("Tanpura", {"excite": 0, "listen": 0, "adecay": 9, "hardness": 0.5, "mass": 0.5, "pos": 0.12, "radiator": 2, "radmix": 0.6, "material": 1, "abright": 0.85,
                 "jawari": 1, "symp": 0.5, "sympset": 3, "sympdecay": 8, "sympbright": 0.6, "sympshimmer": 2}),
    ("Veena", {"excite": 0, "listen": 0, "adecay": 7, "hardness": 0.6, "mass": 0.4, "pos": 0.15, "radiator": 2, "radmix": 0.7, "material": 0.8, "abright": 0.75,
               "jawari": 0.5, "symp": 0.5, "sympset": 0, "sympdecay": 6, "sympbright": 0.5, "sympshimmer": 2}),
    ("Jawari guitar", {"excite": 0, "listen": 0, "adecay": 3, "hardness": 0.75, "pos": 0.16, "radiator": 3, "radmix": 0.55, "material": 0.7, "abright": 0.85,
                       "jawari": 0.5, "symp": 0.3, "sympset": 3, "sympdecay": 3}),
    ("Bowed tanpura", {"strike": 0, "bow": 0.35, "bowpress": 0.5, "bowspeed": 0.3, "atk": 0.4, "rel": 2, "excite": 0, "listen": 0, "adecay": 6, "pos": 0.1,
                       "radiator": 2, "radmix": 0.6, "material": 1, "abright": 0.85, "jawari": 1, "symp": 0.6, "sympset": 0, "sympdecay": 8, "sympbright": 0.6, "sympshimmer": 3}),
    ("Shimmer piano", {"excite": 0, "listen": 0, "astiff": 0.3, "adecay": 6, "hardness": 0.7, "abright": 0.8, "mass": 0.45, "radiator": 3, "radmix": 0.3,
                       "symp": 0.6, "sympset": 4, "sympdecay": 7, "sympbright": 0.6, "sympshimmer": 10}),
    ("Cluster harp", {"excite": 0, "listen": 0, "adecay": 5, "hardness": 0.55, "pos": 0.3, "abright": 0.75, "radiator": 3, "radmix": 0.4,
                      "symp": 0.7, "sympset": 5, "sympdecay": 6, "sympbright": 0.5, "sympshimmer": 6}),
    ("Bowed string", {"strike": 0, "bow": 1, "excite": 0, "listen": 0, "adecay": 2, "pos": 0.12, "atk": 0.15, "radiator": 3, "radmix": 0.45, "symp": 0.35, "sympset": 3, "sympdecay": 4}),
    ("Scratchy bow", {"strike": 0, "bow": 1, "excite": 0, "listen": 0, "adecay": 2, "pos": 0.12, "bowpress": 1.1, "rosin": 0.8, "atk": 0.1}),
    # matched to the old Source/PhysicalModelling/flute.scd (render_old_flute.scd, compare_flute.py):
    # an open tube, the breath heard at the mouthpiece (Air)
    ("Flute", {"strike": 0, "blow": 1, "blowtype": 0, "abore": 1, "abright": 0.5, "adecay": 1, "breath": 0.9, "jetratio": 0.5, "pos": 0.02, "turb": 0.25, "air": 1, "excite": 0, "listen": 0}),
    ("Pan flute", {"strike": 0, "blow": 1, "blowtype": 0, "abore": 0.5, "abright": 0.25, "adecay": 1, "breath": 0.8, "pos": 0.02, "turb": 0.25, "air": 0.4, "excite": 0, "listen": 0}),
    ("Clarinet", {"strike": 0, "blow": 1, "blowtype": 1, "abore": 0.5, "pos": 0.02, "adecay": 0.5, "excite": 0, "listen": 0}),
    ("Sympathetic strings", {"excite": 0, "listen": 0, "adecay": 4, "hardness": 0.6, "abright": 0.8, "material": 0.8, "symp": 0.9, "sympset": 0, "sympdecay": 7, "sympbright": 0.6, "sympshimmer": 4}),
    # ── Bars (Family 1: free bar 0, xylophone .25, gamelan .5, marimba .75, kalimba 1)
    ("Marimba", {"excite": 1, "listen": 1, "bfamily": 1, "bshape": 0.75, "material": 0, "bdecay": 4, "hardness": 0.45, "mass": 0.4, "pos": 0.25, "radiator": 1, "radmix": 0.8, "damper": 0.2}),
    ("Xylophone", {"excite": 1, "listen": 1, "bfamily": 1, "bshape": 0.25, "material": 0, "bdecay": 3, "hardness": 0.85, "pos": 0.25, "radiator": 1, "radmix": 0.5}),
    ("Vibraphone", {"excite": 1, "listen": 1, "bfamily": 1, "bshape": 0.75, "material": 1, "bdecay": 6, "hardness": 0.55, "pos": 0.25, "radiator": 1, "radmix": 0.8, "tremrate": 5, "tremdepth": 0.6, "damper": 0.6, "symp": 0.2, "sympset": 4, "sympdecay": 6, "sympbright": 0.5}),
    ("Glockenspiel", {"excite": 1, "listen": 1, "bfamily": 1, "bshape": 0, "material": 1, "bdecay": 4, "hardness": 0.95, "mass": 0.15, "pos": 0.5, "bbright": 0.7, "symp": 0.2, "sympset": 4, "sympdecay": 4, "sympbright": 0.8, "sympshimmer": 4}),
    ("Wood bar", {"excite": 1, "listen": 1, "bfamily": 1, "bshape": 0, "material": 0, "bdecay": 2, "hardness": 0.5}),
    ("Gamelan saron", {"excite": 1, "listen": 1, "bfamily": 1, "bshape": 0.5, "material": 1, "bdecay": 4, "hardness": 0.7, "beat": 5, "damper": 0.5, "symp": 0.4, "sympset": 1, "sympdecay": 4, "sympbright": 0.7, "sympshimmer": 8}),
    # Kalimba: plucked near the tine's tip with a nail: the bright attack, the other tines answering
    ("Kalimba", {"excite": 1, "listen": 1, "bfamily": 1, "bshape": 1, "material": 1, "bdecay": 2.5, "hardness": 0.95, "mass": 0.05, "bbright": 0.95, "pos": 0.97,
                 "radiator": 3, "radmix": 0.5, "symp": 0.25, "sympset": 3, "sympdecay": 2, "sympbright": 0.8, "sympshimmer": 5}),
    ("Bowed metal bar", {"strike": 0, "bow": 1, "excite": 1, "listen": 1, "bfamily": 1, "bshape": 0, "material": 1, "symp": 0.4, "sympset": 7, "sympdecay": 3, "sympbright": 1, "sympshimmer": 20}),
    ("Cristal Baschet", {"strike": 0, "bow": 1, "excite": 1, "listen": 1, "bfamily": 1, "bshape": 0, "material": 0.5, "bdecay": 3, "bowpress": 0.25, "bowspeed": 0.6, "atk": 0.25, "radiator": 5, "radmix": 0.7, "damper": 0.3, "symp": 0.4, "sympset": 4, "sympdecay": 5, "sympbright": 0.7, "sympshimmer": 4}),
    # ── Shells & bells (Family 2: wine glass 0, bowl .25, hand bell .5, church bell .75, gong 1)
    ("Wine glass", {"excite": 1, "listen": 1, "bfamily": 2, "bshape": 0, "material": 0.5, "bdecay": 4, "hardness": 0.9, "split": 2, "symp": 0.25, "sympset": 1, "sympdecay": 4, "sympbright": 0.8}),
    ("Singing glass", {"strike": 0, "bow": 1, "excite": 1, "listen": 1, "bfamily": 2, "bshape": 0, "material": 0.5, "bdecay": 3, "pos": 0.5, "bowpress": 0.2, "bowspeed": 0.7, "atk": 0.3, "split": 1, "symp": 0.2, "sympset": 4, "sympdecay": 4, "sympbright": 0.7}),
    ("Singing bowl", {"excite": 1, "listen": 1, "bfamily": 2, "bshape": 0.25, "material": 1, "bdecay": 8, "hardness": 0.4, "mass": 0.6, "split": 1.5, "drift": 0.3, "symp": 0.5, "sympset": 1, "sympdecay": 8, "sympbright": 0.6, "sympshimmer": 3}),
    ("Bowed bowl", {"strike": 0, "bow": 1, "excite": 1, "listen": 1, "bfamily": 2, "bshape": 0.25, "material": 1, "bdecay": 8, "pos": 0.05, "bowpress": 0.3, "bowspeed": 0.4, "atk": 0.3, "symp": 0.3, "sympset": 1, "sympdecay": 8, "sympbright": 0.6, "sympshimmer": 3}),
    ("Hand bell", {"excite": 1, "listen": 1, "bfamily": 2, "bshape": 0.5, "material": 1, "bdecay": 3, "hardness": 0.8, "damper": 0.4, "symp": 0.2, "sympset": 1, "sympdecay": 3, "sympbright": 0.8}),
    ("Church bell", {"excite": 1, "listen": 1, "bfamily": 2, "bshape": 0.75, "material": 1, "bdecay": 6, "hardness": 0.8, "mass": 0.7, "split": 1, "symp": 0.35, "sympset": 4, "sympdecay": 6, "sympbright": 0.6, "sympshimmer": 2}),
    ("Gong ageng", {"excite": 1, "listen": 1, "bfamily": 2, "bshape": 1, "material": 1, "bdecay": 8, "hardness": 0.3, "mass": 0.8, "beat": 3, "symp": 0.4, "sympset": 1, "sympdecay": 8, "sympbright": 0.5, "sympshimmer": 6}),
    ("Opera gong", {"excite": 1, "listen": 1, "bfamily": 2, "bshape": 1, "material": 1, "bdecay": 3, "hardness": 0.6, "tension": -0.8}),
    ("Blown bell drone", {"strike": 0, "blow": 1, "blowtype": 0, "excite": 1, "listen": 1, "bfamily": 2, "bshape": 0.75, "material": 1, "atk": 0.5, "symp": 0.5, "sympset": 3, "sympdecay": 6, "sympbright": 0.6, "sympshimmer": 3}),
    ("Glass drone", {"strike": 0, "bow": 0.6, "blow": 0.4, "excite": 0.6, "listen": 0.6, "bfamily": 2, "bshape": 0, "drift": 1, "freeze": 0.5, "atk": 1, "rel": 3, "split": 3, "symp": 0.4, "sympset": 5, "sympdecay": 8, "sympbright": 0.6, "sympshimmer": 8}),
    # ── Tonefields (Family 3: handpan 0, steel pan 1)
    # the note field's own fifth and octaves bloom after the hit (Sympathetic, Pan: the old Handpan's coupled modes); never damped
    ("Handpan", {"excite": 1, "listen": 1, "bfamily": 3, "bshape": 0, "material": 1, "bdecay": 4, "hardness": 0.6, "mass": 0.4, "split": 6, "bbright": 0.95, "pos": 0.3,
                 "symp": 0.9, "sympset": 1, "sympdecay": 4, "sympbright": 0.7, "sympshimmer": 6}),
    ("Handpan Kurd", {"excite": 1, "listen": 1, "bfamily": 3, "bshape": 0, "material": 1, "bdecay": 4, "hardness": 0.5, "mass": 0.45, "split": 6, "bbright": 0.9, "pos": 0.3,
                      "symp": 0.8, "sympset": 2, "sympdecay": 5, "sympbright": 0.6, "sympshimmer": 4}),
    ("Steel pan", {"excite": 1, "listen": 1, "bfamily": 3, "bshape": 1, "material": 1, "bdecay": 2.5, "hardness": 0.7, "split": 3, "symp": 0.4, "sympset": 1, "sympdecay": 2, "sympbright": 0.8}),
    ("Steel drum", {"excite": 1, "listen": 1, "bfamily": 3, "bshape": 1, "material": 1, "bdecay": 2.5, "hardness": 0.75, "bbright": 0.85, "split": 8, "beat": 2,
                    "symp": 0.7, "sympset": 1, "sympdecay": 2.5, "sympbright": 0.8, "sympshimmer": 9}),
    # ── Membranes (Family 4: tabla 0, timpani .5, membrane 1); Position = radius, Y = angle
    ("Tabla rim", {"excite": 1, "listen": 1, "bfamily": 4, "bshape": 0, "material": 0.4, "bdecay": 1.5, "hardness": 0.7, "tension": 0.3, "pos": 0.85, "radiator": 4, "radmix": 0.4, "symp": 0.15, "sympset": 4, "sympdecay": 1.5, "sympbright": 0.6}),
    ("Tabla centre", {"excite": 1, "listen": 1, "bfamily": 4, "bshape": 0, "material": 0.4, "bdecay": 0.8, "hardness": 0.4, "mass": 0.6, "tension": 0.5, "pos": 0.05, "radiator": 4, "radmix": 0.4}),
    ("Timpani", {"excite": 1, "listen": 1, "bfamily": 4, "bshape": 0.5, "material": 0.4, "bdecay": 3, "hardness": 0.3, "mass": 0.7, "pos": 0.75, "tension": 0.15, "damper": 0.5, "symp": 0.2, "sympset": 3, "sympdecay": 3, "sympbright": 0.4}),
    ("Tom", {"excite": 1, "listen": 1, "bfamily": 4, "bshape": 1, "material": 0.4, "tension": 0.8, "bdecay": 1, "hardness": 0.4, "pos": 0.5, "radiator": 4, "radmix": 0.3}),
    ("Talking drum", {"excite": 1, "listen": 1, "bfamily": 4, "bshape": 1, "material": 0.3, "tension": 1, "bdecay": 1.5, "hardness": 0.5, "pos": 0.6}),
    ("Frame drum", {"excite": 1, "listen": 1, "bfamily": 4, "bshape": 1, "material": 0.2, "bdecay": 0.8, "hardness": 0.25, "mass": 0.6, "pos": 0.8, "radiator": 4, "radmix": 0.5}),
    ("Frame drum centre", {"excite": 1, "listen": 1, "bfamily": 4, "bshape": 1, "material": 0.2, "bdecay": 0.6, "hardness": 0.2, "mass": 0.8, "pos": 0.08, "radiator": 4, "radmix": 0.5}),
    # ── Plates & blocks (Family 5: plate 0, wood block 1); Position X / Y
    ("Wood block", {"excite": 1, "listen": 1, "bfamily": 5, "bshape": 1, "material": 0, "bdecay": 1, "hardness": 0.9}),
    # Cymbals: a plate's modes, Cascade's bloom and the Cymbal strings' dense comb wash (cymbalB's bank)
    ("Crash cymbal", {"excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 4, "bmodes": 48, "bbright": 1, "hardness": 0.95, "mass": 0.25,
                      "cascade": 0.8, "split": 18, "beat": 5, "pos": 0.8, "posy": 0.7, "symp": 0.9, "sympset": 6, "sympdecay": 3, "sympbright": 1, "sympshimmer": 25}),
    ("Ride cymbal", {"excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 5, "bmodes": 48, "bbright": 0.9, "hardness": 0.98, "mass": 0.08,
                     "cascade": 0.3, "split": 10, "beat": 3, "pos": 0.35, "posy": 0.3, "symp": 0.5, "sympset": 6, "sympdecay": 4, "sympbright": 1, "sympshimmer": 15}),
    ("Brushed cymbal", {"strike": 0, "scrape": 1, "scrapespeed": 0.7, "rough": 0.95, "grit": 0.5, "chatter": 0.3, "atk": 0.08, "rel": 0.4,
                        "excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 3, "bmodes": 48, "bbright": 1, "cascade": 0.5, "split": 15, "pos": 0.7,
                        "symp": 0.8, "sympset": 6, "sympdecay": 2, "sympbright": 1, "sympshimmer": 25,
                        "EQ_On": True, "EQ_High": 6.0}),   # the reference's lift above 10 kHz
    ("Hi-hat", {"excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 0.35, "bmodes": 48, "bbright": 1, "hardness": 1, "mass": 0.05,
                "split": 30, "beat": 12, "pos": 0.6, "posy": 0.4, "damper": 0.8, "symp": 0.8, "sympset": 7, "sympdecay": 0.3, "sympbright": 1, "sympshimmer": 40}),
    ("Sizzle cymbal", {"excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 5, "bmodes": 48, "bbright": 1, "hardness": 0.9, "mass": 0.3,
                       "cascade": 0.6, "split": 15, "beat": 4, "pos": 0.75, "posy": 0.6, "rattle": 0.5, "rattleth": 0.03,
                       "symp": 0.8, "sympset": 6, "sympdecay": 4, "sympbright": 1, "sympshimmer": 20}),
    ("Metal shimmer pad", {"strike": 0, "bow": 1, "bowpress": 0.35, "bowspeed": 0.5, "atk": 1.2, "rel": 3, "excite": 1, "listen": 1, "bfamily": 5, "bshape": 0,
                           "material": 1, "bdecay": 4, "bmodes": 48, "bbright": 0.9, "split": 12, "beat": 2, "drift": 0.5,
                           "symp": 0.8, "sympset": 7, "sympdecay": 5, "sympbright": 0.9, "sympshimmer": 30}),
    ("Tam-tam bloom", {"excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 4, "cascade": 0.8, "hardness": 0.8, "mass": 0.8, "symp": 0.5, "sympset": 6, "sympdecay": 5, "sympbright": 0.8, "sympshimmer": 15}),
    ("Thunder sheet", {"excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 3, "hardness": 0.9, "pos": 0.06, "posy": 0.08, "cascade": 0.5, "rattle": 0.3, "rattleth": 0.1, "bbright": 0.8, "symp": 0.4, "sympset": 7, "sympdecay": 3, "sympbright": 1, "sympshimmer": 25}),
    ("Scraped plate", {"strike": 0, "scrape": 1, "excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "chatter": 0.5, "symp": 0.5, "sympset": 7, "sympdecay": 2, "sympbright": 1, "sympshimmer": 20}),
    ("Harsh scrape", {"strike": 0, "scrape": 1, "rough": 0.9, "grit": 0.8, "chatter": 1, "cascade": 0.6, "rattle": 0.4, "rattleth": 0.1, "material": 1, "bfamily": 5, "bshape": 0, "excite": 1, "listen": 1}),
    # ── Playing by themselves (Gate Mode Pulse)
    ("Rain on glass", {**PULSE, "excite": 1, "listen": 1, "bfamily": 2, "bshape": 0, "material": 0.5, "bdecay": 3, "hardness": 0.95, "mass": 0.1, "split": 2,
                       "Pulse_Rate": 6, "Pulse_Chaos": 1, "Pulse_Link": 0, "Pulse_Velocity": 0.5, "Pulse_Velocity_Jitter": 0.9, "symp": 0.3, "sympset": 4, "sympdecay": 3, "sympbright": 0.8, "sympshimmer": 4}),
    ("Gamelan clock", {**PULSE, "excite": 1, "listen": 1, "bfamily": 1, "bshape": 0.5, "material": 1, "bdecay": 4, "hardness": 0.7, "beat": 5, "damper": 0.4,
                       "Pulse_Sync": True, "Pulse_Beats": 0.5, "Pulse_Chance": 0.7, "Pulse_Link": 0.5, "Pulse_Length": 0.8, "symp": 0.4, "sympset": 1, "sympdecay": 4, "sympbright": 0.7, "sympshimmer": 8}),
    ("Bowl circle", {**PULSE, "excite": 1, "listen": 1, "bfamily": 2, "bshape": 0.25, "material": 1, "bdecay": 8, "hardness": 0.4, "mass": 0.6, "split": 1.5, "drift": 0.3,
                     "Pulse_Rate": 0.4, "Pulse_Chaos": 0.3, "Pulse_Link": 0, "Pulse_Phasing": 0.4, "symp": 0.4, "sympset": 1, "sympdecay": 8, "sympbright": 0.6, "sympshimmer": 3}),
    ("Taraf rain", {**PULSE, "excite": 0, "listen": 0, "adecay": 5, "hardness": 0.85, "pos": 0.08, "radiator": 2, "radmix": 0.5, "material": 1, "abright": 0.9,
                    "jawari": 0.8, "symp": 0.8, "sympset": 0, "sympdecay": 7, "sympbright": 0.7, "sympshimmer": 3,
                    "Pulse_Rate": 3, "Pulse_Chaos": 0.8, "Pulse_Link": 0, "Pulse_Chance": 0.8, "Pulse_Velocity_Jitter": 0.7}),
    ("Geiger hats", {**PULSE, "excite": 1, "listen": 1, "bfamily": 5, "bshape": 0, "material": 1, "bdecay": 0.35, "bmodes": 48, "bbright": 1, "hardness": 1, "mass": 0.05,
                     "split": 30, "beat": 12, "pos": 0.6, "posy": 0.4, "damper": 0.8, "symp": 0.8, "sympset": 7, "sympdecay": 0.3, "sympbright": 1, "sympshimmer": 40,
                     "Pulse_Rate": 9, "Pulse_Chaos": 1, "Pulse_Cluster": 0.6, "Pulse_Link": 0, "Pulse_Length": 0.1, "Pulse_Velocity_Jitter": 0.8}),
    ("Drum circle", {**PULSE, "excite": 1, "listen": 1, "bfamily": 4, "bshape": 1, "material": 0.2, "bdecay": 0.8, "hardness": 0.3, "mass": 0.6, "pos": 0.7, "radiator": 4, "radmix": 0.5,
                     "Pulse_Sync": True, "Pulse_Beats": 0.25, "Pulse_Chance": 0.6, "Pulse_Cluster": 0.3, "Pulse_Link": 0.3, "Pulse_Velocity_Jitter": 0.6}),
]

LEVEL = {}   # filled by "levels" and pasted back here
try:
    LEVEL = json.loads(pathlib.Path(__file__).with_name("factory_levels.json").read_text())
except Exception:
    pass

def cval(v):
    if v is True: return "1"
    if v is False: return "0"
    if isinstance(v, int): return str(v)
    s = repr(float(v))
    return s + "f" if "." in s else s

if sys.argv[1] == "cpp":
    for name, vals in S:
        vals = dict(vals)
        if name in LEVEL and abs(LEVEL[name]) > 0.01: vals["level"] = LEVEL[name]
        items = ", ".join('{"%s", %s}' % (k, cval(v)) for k, v in vals.items())
        print('        {"%s", {%s}},' % (name, items))
elif sys.argv[1] == "scd":
    def scv(v): return "1" if v is True else "0" if v is False else str(v)
    out = []
    for name, vals in S:
        if name == "init": continue
        sustained = any(vals.get(k, 0) for k in ("bow", "blow", "scrape"))
        params = [x for k, v in vals.items() if k[0].islower() and k != "level" for x in ("\\" + k, scv(v))]
        out.append('["f_%s", %d, [%s]]' % (name.replace(" ", "_").replace("-", "_"), 4, ", ".join(["\\freq", "130.81"] + params)))
    print("[" + ",\n".join(out) + "]")
elif sys.argv[1] == "levels":
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    from analyze import read_wav
    import numpy as np
    d = pathlib.Path(sys.argv[2]); res = {}
    for name, vals in S:
        p = d / ("f_" + name.replace(" ", "_").replace("-", "_") + ".wav")
        if not p.exists(): continue
        x, r = read_wav(p); m = x.mean(1)
        # how loud it sounds: the loudest 50 ms (a hit's attack, a drone's body)
        hop = r // 20
        rms = max(float(np.sqrt((m[i:i + hop] ** 2).mean())) for i in range(0, len(m) - hop, hop // 2))
        db = 20 * math.log10(0.15 / max(rms, 1e-6))
        res[name] = round(max(-12, min(20, db)) * 2) / 2
        print(f"{name:22s} rms {rms:.4f} peak {np.abs(x).max():.2f} -> level {res[name]:+.1f} dB")
    pathlib.Path(__file__).with_name("factory_levels.json").write_text(json.dumps(res, indent=1))

# python3 factory_sounds.py library <nodeSnapshots/PhysicalBox>: the library's
# slots become this factory set, followed by every snapshot that is not a
# factory sound (yours), unchanged; a copy of the folder is kept next to it.
if sys.argv[1] == "library":
    import re, shutil, time
    root = pathlib.Path(__file__).resolve().parents[2]
    lib = pathlib.Path(sys.argv[2])
    shutil.copytree(lib, lib.with_name(lib.name + "_backup_" + time.strftime("%Y%m%d-%H%M%S")))
    src = (root / "src/nodes/scPhysicalBox.cpp").read_text()
    def esc(s):
        for ch in " <>{}[],()/\\.#": s = s.replace(ch, "_")
        return s
    key = {sc: esc(name) for name, sc in re.findall(r'addSound\("([^"]+)", "(\w+)"', src)}
    h = (root / "ugens/OceanodePhysical/PhysicalModes.h").read_text()
    pdef = {n: float(v) for n, v in re.findall(r'X\((\w+),\s*([-\d.]+)\)', h[h.index('#define PHYSICAL_PARAMS'):h.index('enum Param')])}
    bank = json.loads((lib / "banks/Factory.json").read_text())["slots"]
    base = dict(next(s for s in bank if s.get("name") == "init"))
    for sc, k in key.items():
        if sc in pdef and k not in base: base[k] = pdef[sc]
    base["Shape"] = pdef["bshape"]; base["Family"] = pdef["bfamily"]; base["Gate_Mode"] = 2
    factoryNames = {s.get("name") for s in bank} | {n for n, _ in S}
    slots = []
    for name, vals in S:
        j = dict(base); j["name"] = name; vals = dict(vals)
        if name in LEVEL and abs(LEVEL[name]) > 0.01: vals["level"] = LEVEL[name]
        for k, v in vals.items():
            if k in key: j[key[k]] = float(v)
            elif k in base:
                d = base[k]; j[k] = bool(v) if isinstance(d, bool) else (int(round(v)) if isinstance(d, int) else float(v))
            else: print("no control", name, k)
        slots.append(j)
    files = sorted(lib.glob("snapshot_*.json"), key=lambda f: int(f.stem.split("_")[1]))
    mine = [json.loads(f.read_text()) for f in files]
    mine = [j for j in mine if j.get("name") not in factoryNames]
    for j in mine:   # saved before the body models: the same body in the new scheme
        if "Family" not in j and isinstance(j.get("Shape"), (int, float)):
            old = max(0, min(5, round(j["Shape"])))
            j["Family"] = [0, 1, 2, 2, 5, 4][old]; j["Shape"] = [0, 0, 0, 0.75, 0, 1][old]
    for f in files: f.unlink()
    for i, j in enumerate(slots + mine): (lib / f"snapshot_{i}.json").write_text(json.dumps(j, indent=4))
    (lib / "library.json").write_text(json.dumps({"snapshots": 1, "bank": ""}))
    print(len(slots), "factory +", len(mine), "yours:", [j.get("name") for j in mine])
