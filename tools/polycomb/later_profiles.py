"""Explicit, deliberately approximate projections of the later ensembles.

The user chose frozen pitch vectors instead of new pitch-map/harmony DSP.
No inferred control is presented as a decoded NI algorithm. Full source states
are retained separately by import_ensemble.py, including unsupported controls.
"""
import math

def projection(version,source,voices):
    # Reused IDs whose paths retain the same function in these versions.
    common=(8,9,10,11,12,13,21,30,45,55,56,58,127,128,154,155,156,157,159,
            178,179,181,182,193,194,195,200,204,205,207,208,209,210,211,212,
            226,227,228,229,237,238,240,242,243,260,261,268,270,344,347,348,
            359,360,361,362,364,365,369,376,377,378,385,386,387,389,390,391,
            392,393,394,443,444,446,450,456,457,460,461,480,481,482,483,484,486,
            578,579,589,592)
    p={k:source[k]for k in common if k in source}
    # Neutral defaults for removed controls, never values from another subsystem
    # that happens to reuse an old local ID.
    p.update({7:12,18:.7,19:1,36:0,37:0,38:0,39:5,49:0,75:0,83:.5,
              84:.3,85:.02,86:0,87:120,124:0,125:0,216:1,219:0,220:60,
              231:1,232:0,233:60,272:0,351:0,380:0,436:0,442:0,493:2,
              495:1,496:1})
    issues=[f'{version}: user-authorized static pitch approximation; pitch-map curves, randomization, clocked harmony/chord changes and MIDI-dependent remapping are not reproduced',
            f'{version}: new modulation selectors, gated LFOs, tremolos and independent layers are retained in raw data; only existing native routes are approximated']
    def alias(old,new,default=0):p[old]=source.get(new,default)
    if version=='2018':
        for old,new in ((7,635),(18,1321),(19,1322),(37,1310),(38,1311),(49,1333),
                        (75,868),(493,493)):
            alias(old,new)
        p[39]=2/max(.0625,source.get(1312,1))
        p[83]=source.get(863,50)/100
        p[436]=-source.get(1318,60)  # reversed exponential-time knob
        # First retain the stored source oscillator selection (an integer state,
        # although that version's panel has snapshot isolation enabled).
        p[11]=source.get(11,5)
        main=[];comb=[]
        # Evenly sample the original 32-voice pool into the native 16 voices.
        for i in range(voices):
            index=i*32/voices
            main.append(60+p[37]+p.get(45,0)+p.get(128,0)+index*source.get(1244,0))
            comb.append(60+p[37]+p.get(30,0)+p.get(127,0)+index*source.get(1275,0))
        issues.extend(['2018: 32 source voices sampled uniformly into 16 native voices; linear interval grid stands in for the later pitch-set/harmonic selection',
                       '2018: Micro Space is approximated by shared reverb; cabinet and additional amplitude-selector stages differ'])
    else:
        p.update({7:12,18:source.get(2539,.7),19:source.get(2409,1),
                  37:source.get(1310,0)+source.get(1313,0),38:source.get(1311,0),
                  39:2/max(.0625,source.get(1702,1)),49:source.get(1333,0),
                  56:source.get(1736,0),127:source.get(1737,0),461:source.get(1978,0),
                  460:source.get(1977,1),592:source.get(1985,0)})
        for old,new in ((154,2017),(155,2018),(156,2019),(157,2020),(159,2021),
                        (178,1840),(181,1843),(182,1844),(194,1846),(195,1847),
                        (238,1854),(242,1836),(243,1837),(268,1838),
                        (204,2495),(207,2498),(208,2499),(210,2500),(211,2501),
                        (237,2504),(260,2491),(261,2492),(270,2493)):
            if new in source:alias(old,new)
        # The redesigned continuous wave shaper has no discrete waveform ID.
        p[193]=5;p[209]=5
        if int(source.get(2540,1))==2:p[18]=-p[18]
        # The later panel stores filter family and pole count independently.
        # Project to the old six-family vocabulary before native type selection.
        for old,family,poles in ((10,2092,2094),(13,2101,2103)):
            f=int(source.get(family,1));four=int(source.get(poles,2))>=3
            p[old]=({1:6 if four else 3,2:1,3:4 if four else 2,4:3}).get(f,3)
        main=[];comb=[]
        for i in range(voices):
            def grid(step,length,offset,transpose):
                phase=(i/max(1,voices-1)*max(0,source.get(length,1))+source.get(offset,0))%1
                return 60+p[37]+source.get(transpose,0)+phase*source.get(step,1)*12
            main.append(grid(2332,2333,2334,2565))
            comb.append(grid(2604,2605,2606,2624)+source.get(1737,0))
        issues.extend(['2025: frozen linear pitch-index grid at MIDI 60; separate Harmony instrument and probability chains are not translated into live processing',
                       '2025: sample oscillator, tube/sine inserts and external limiter are not recreated',
                       '2025: 6 dB filter selections approximated by the available 12 dB option'])
    # Names that preserve waveform identity in the classic vocabulary. A sample
    # selection cannot be recovered without its sample layer: flag, do not hide.
    if p.get(11,1)not in range(1,9):
        issues.append(f'{version}: unsupported oscillator selection replaced by sine')
        p[11]=5
    for k,default in ((181,1),(182,0),(193,5),(194,1),(195,4),(207,1),(208,0),
                      (209,5),(210,1),(211,4),(243,32),(261,32),(359,1)):
        p.setdefault(k,default)
    # Removed, inactive stages keep usable native defaults instead of producing
    # spurious range-clipping warnings for a source control that no longer exists.
    for k,default in ((443,.45),(444,.5),(446,84),(450,0),(456,.2),
                      (480,40),(481,-18),(482,20*math.log10(150)),(483,2/3),
                      (484,0),(486,6)):
        p.setdefault(k,default)
    assert all(math.isfinite(x)for x in main+comb)
    return p,issues,(main,comb)
