#!/usr/bin/env python3
"""NRT regression for host-locked pulses and the unchanged free-running clock.

check_transport.py BUILD [LEGACY_RandomMetro1.scsyndef]
"""
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import bundle, message, read_wav

work = pathlib.Path(sys.argv[1]).resolve()
sr = 48000
plugins = '/Applications/SuperCollider.app/Contents/Resources/plugins'

def render(name, defs, events, end):
    osc = work / (name + '.osc')
    score = [bundle(0, [message('/d_load', str(d)) for d in defs] + [message('/g_new', 1, 0, 0)])]
    score += [bundle(t, messages) for t, messages in events]
    score += [bundle(end, [message('/g_freeAll', 1)]), bundle(end + .01, [message('/c_set', 0, 0)])]
    osc.write_bytes(b''.join(score))
    wav = work / (name + '.wav')
    p = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth',
        '-N', str(osc), '_', str(wav), str(sr), 'WAV', 'float', '-o', '1', '-i', '0',
        '-z', '64', '-D', '0', '-U', plugins], capture_output=True, text=True, timeout=60)
    assert p.returncode == 0 and not any(s in p.stdout+p.stderr for s in ('FAILURE', 'ERROR')), p.stdout+p.stderr
    audio, channels, _ = read_wav(wav)
    assert channels == 1
    return audio

clockdef = work / 'defs/TransportPulse/TransportPulse1.scsyndef'
randomdef = work / 'defs/RandomMetro/RandomMetro1.scsyndef'

def assert_times(audio, expected, tolerance=2):
    events = [i for i, value in enumerate(audio) if value > .5]
    samples = [round(t * sr) for t in expected]
    assert len(events) == len(samples), (len(events), len(samples), [round(i/sr, 5) for i in events])
    assert all(abs(a-b) <= tolerance for a,b in zip(events, samples)), list(zip(events, samples))
    return events

def create(**params):
    args = ['TransportPulse1', 1000, 1, 1, 'out', 0]
    for key, value in params.items(): args += [key, value]
    return message('/s_new', *args)

# Sample-rate interpolation: restart, stop, continue at the stopped beat,
# tempo change, and seek to an off-grid position.
events = [(.1, [create(playing=1., bpm=120., division=3, beattransport=0.)]),
    (1.1, [message('/n_set', 1000, 'playing', 0., 'beattransport', 2.)]),
    (1.6, [message('/n_set', 1000, 'playing', 1., 'beattransport', 2.)]),
    (2.1, [message('/n_set', 1000, 'bpm', 60., 'beattransport', 3.)]),
    (2.7, [message('/n_set', 1000, 'beattransport', .2)])]
audio = render('transport_run_stop_seek', [clockdef], events, 3.1)
expected = [.1 + i*.125 for i in range(8)] + [1.6+i*.125 for i in range(4)]
expected += [2.1, 2.35, 2.6, 2.75, 3.]
assert_times(audio, expected)

# Continuous 60 Hz anchors must neither duplicate nor drop boundary pulses.
events = [(.1, [create(playing=1., bpm=120., division=3, beattransport=0.)])]
events += [(.1 + i/60, [message('/n_set', 1000, 'beattransport', i/30.)]) for i in range(1, 120)]
assert_times(render('transport_anchors', [clockdef], events, 2.09), [.1+i*.125 for i in range(16)], tolerance=128)

# Bar duration follows the explicit meter, rather than assuming 4/4.
events = [(.1, [create(playing=1., bpm=120., division=12, barbeats=3., beattransport=0.)])]
assert_times(render('transport_three_beat_bars', [clockdef], events, 4.7), [.1, 1.6, 3.1, 4.6])
for label, division, spacing in [('triplet', 4, 1/6), ('dotted', 7, .375)]:
    events = [(.1, [create(playing=1., bpm=120., division=division, beattransport=0.)])]
    assert_times(render('transport_' + label, [clockdef], events, 1.99),
                 [.1+i*spacing for i in range(20) if .1+i*spacing < 1.99])

# Clock mode selects the supplied pulses, ignoring the ms interval. Manual
# Fire remains available. The free-running clock remains selected by default.
events = [(.1, [message('/s_new', 'TransportPulse1', 1000, 1, 1, 'out', 8,
                       'playing', 1., 'bpm', 120., 'division', 5, 'beattransport', 0.),
    message('/s_new', 'RandomMetro1', 1001, 1, 1, 'clock', 8, 'out', 0, 'quantize', 1., 'seed', 123)]),
    (.42, [message('/n_set', 1001, 'fire', 1.)])]
audio = render('transport_selected', [clockdef, randomdef], events, 1.09)
assert_times(audio, [.1, .35, .42, .6, .85], tolerance=64)
events[0][1][1] = message('/s_new', 'RandomMetro1', 1001, 1, 1, 'clock', 8, 'out', 0,
                         'quantize', 1., 'chance', 0., 'seed', 123)
assert_times(render('transport_chance_zero', [clockdef, randomdef], events, 1.09), [.42], tolerance=64)

if len(sys.argv) > 2:
    events = [(.1, [message('/s_new', 'RandomMetro1', 1000, 1, 1, 'out', 0,
        'range', 350., 'minms', 10., 'curve', 1.4, 'chance', .65, 'seed', 123)])]
    old = render('free_legacy', [pathlib.Path(sys.argv[2]).resolve()], events, 4.)
    new = render('free_current', [randomdef], events, 4.)
    assert old == new, 'The free clock changed with quantize disabled'
    print('Free clock: bit-identical to the previous SynthDef with a fixed seed.')
print('Transport: musical grid, interpolation, stop/resume, BPM, seek, 3/4 bars, anchors and manual Fire passed.')
