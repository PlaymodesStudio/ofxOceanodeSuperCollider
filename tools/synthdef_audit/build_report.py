#!/usr/bin/env python3
"""Build the read-only library audit and exhaustive source catalog."""
import sys, json, re, collections, hashlib, subprocess, csv, unicodedata
from pathlib import Path
from xml.sax.saxutils import escape
import scan, review
from reportlab.pdfgen import canvas
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, PageBreak, Table, TableStyle, KeepTogether, HRFlowable
from reportlab.platypus.tableofcontents import TableOfContents
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
ROOT=scan.ROOT; OUT=ROOT/'output/pdf'; DATA=OUT/'synthdef-inventory.json'
D=json.loads(DATA.read_text()); E=D['entries']; F=D['files']; FM={f['path']:f for f in F}
for e in E:
    e['proposed_category']=review.proposed(e)
    e['review_groups']=[g['id'] for g in review.GROUPS if e['name'] in g['names'] and e['eligible']]
ACTIVE=[e for e in E if e['eligible'] and e['body_hash']]
NAMES={e['name']:e for e in ACTIVE}
for g in review.GROUPS:
    assert all(n in NAMES for n in g['names']),(g['id'],g['names'])
assert len({n for g in review.GROUPS for n in g['names']})==sum(len(g['names']) for g in review.GROUPS),'Overlapping reduction estimates'
D['review_groups']=review.GROUPS
D['summary']['raw_synthdef_declarations']=sum(f['raw_calls'] for f in F)
D['summary']['catalog_records']=len(E)
D['summary']['category_count']=len(set(e['category'] for e in E))
D['summary']['first_pass_reduction']=sum(len(g['names'])-1 for g in review.GROUPS if g['phase']==1)
D['summary']['second_pass_reduction']=sum(len(g['names'])-1 for g in review.GROUPS if g['phase']==2)
D['snapshot']={'date':'2026-10-23','git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'manifest_sha256':hashlib.sha256(json.dumps([(f['path'],f['sha256']) for f in F]).encode()).hexdigest()}
DATA.write_text(json.dumps(D,indent=2))
with (OUT/'synthdef-inventory.csv').open('w') as fh:
    fields=['name','path','line','status','eligible','category','proposed_category','review_groups','description','parameters','ugens'];w=csv.DictWriter(fh,fieldnames=fields);w.writeheader()
    for e in E:w.writerow({k:'; '.join(e[k]) if isinstance(e[k],(list,dict)) else e[k] for k in fields})
# Locally available fonts, embedded for reliable rendering.
fontbase=Path('/System/Library/Fonts/Supplemental')
for name,file in [('Audit','Arial.ttf'),('AuditBold','Arial Bold.ttf'),('AuditItalic','Arial Italic.ttf')]:pdfmetrics.registerFont(TTFont(name,str(fontbase/file)))
pdfmetrics.registerFontFamily('Audit',normal='Audit',bold='AuditBold',italic='AuditItalic',boldItalic='AuditBold')
INK=colors.HexColor('#17303D');TEAL=colors.HexColor('#087F8C'); MUTED=colors.HexColor('#536874');PALE=colors.HexColor('#EDF5F6');LIGHT=colors.HexColor('#D3E2E5')
S=getSampleStyleSheet()
for k in S.byName:S[k].fontName='Audit';S[k].textColor=INK
S.add(ParagraphStyle(name='TitleBig',fontName='AuditBold',fontSize=35,leading=39,textColor=INK,spaceAfter=20))
S.add(ParagraphStyle(name='Deck',fontName='Audit',fontSize=15,leading=22,textColor=MUTED,spaceAfter=15))
S.add(ParagraphStyle(name='H1Audit',fontName='AuditBold',fontSize=22,leading=27,textColor=INK,spaceBefore=6,spaceAfter=16,keepWithNext=True))
S.add(ParagraphStyle(name='H2Audit',fontName='AuditBold',fontSize=13,leading=17,textColor=TEAL,spaceBefore=15,spaceAfter=8,keepWithNext=True))
S.add(ParagraphStyle(name='BodyAudit',fontName='Audit',fontSize=10,leading=14,spaceAfter=9))
S.add(ParagraphStyle(name='SmallAudit',fontName='Audit',fontSize=8.1,leading=11,textColor=MUTED,spaceAfter=5,splitLongWords=True))
S.add(ParagraphStyle(name='TinyAudit',fontName='Audit',fontSize=7.2,leading=10,textColor=MUTED,spaceAfter=3,splitLongWords=True))
S.add(ParagraphStyle(name='EntryTitle',fontName='AuditBold',fontSize=11,leading=14,textColor=INK,spaceBefore=8,spaceAfter=4,keepWithNext=True))
S.add(ParagraphStyle(name='TableAudit',fontName='Audit',fontSize=8,leading=11,spaceAfter=0))
S.add(ParagraphStyle(name='TOC0',fontName='AuditBold',fontSize=11,leading=16,spaceBefore=9,textColor=INK))
S.add(ParagraphStyle(name='TOC1',fontName='Audit',fontSize=9,leading=12,leftIndent=12,textColor=MUTED))
def txt(x):
    # Plain ASCII hyphens in all report text; preserve accented proper names.
    return escape(str(x).replace('\u2011','-').replace('\u2013','-').replace('\u2014',' - ').replace('×','x').replace('→','to').replace('≤','<=').replace('≥','>='))
STYLE_MODE=False
S.add(ParagraphStyle(name='BodyPlan',parent=S['BodyAudit'],fontSize=9.5,leading=13,spaceAfter=7))
S.add(ParagraphStyle(name='H2Plan',parent=S['H2Audit'],spaceBefore=11,spaceAfter=6))
def p(x,style='BodyAudit'):
    if STYLE_MODE:style={'BodyAudit':'BodyPlan','H2Audit':'H2Plan'}.get(style,style)
    return Paragraph(txt(x),S[style])
def rich(x,style='BodyAudit'):return Paragraph(x,S[style])
ST=[];seq=0
def heading(title,level=0):
    global seq
    seq+=1;pp=p(title,'H1Audit' if level==0 else 'H2Audit');pp.audit_heading=(level,title,f'h{seq}');ST.append(pp)
def body(x):ST.append(p(x))
def small(x):ST.append(p(x,'SmallAudit'))
def page(title):ST.append(PageBreak());heading(title)
def table(rows,widths,header=True):
    data=[[p(c,'TableAudit') for c in row] for row in rows]
    t=Table(data,colWidths=widths,repeatRows=1 if header else 0,hAlign='LEFT')
    style=[('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),7),('RIGHTPADDING',(0,0),(-1,-1),7),('TOPPADDING',(0,0),(-1,-1),7),('BOTTOMPADDING',(0,0),(-1,-1),7),('LINEBELOW',(0,0),(-1,0),1,TEAL),('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.white,PALE])]
    if header:style += [('BACKGROUND',(0,0),(-1,0),LIGHT)]
    t.setStyle(TableStyle(style));ST.append(t)
class AuditDoc(SimpleDocTemplate):
    def afterFlowable(self,f):
        if hasattr(f,'audit_heading'):
            level,title,key=f.audit_heading;self.canv.bookmarkPage(key);self.canv.addOutlineEntry(title,key,level=level,closed=level>0);
            if level==0:self.notify('TOCEntry',(level,title,self.page,key))
    def header(self,c,doc):
        c.saveState();w,h=A4
        if doc.page>1:
            c.setFont('AuditBold',8);c.setFillColor(TEAL);c.drawString(43,h-29,'OCEANAUDIO / SYNTHDEF LIBRARY REVIEW')
            c.setStrokeColor(LIGHT);c.line(43,h-37,w-43,h-37)
        c.setFont('Audit',8);c.setFillColor(MUTED);c.drawString(43,24,'Source audit  |  23 October 2026');c.drawRightString(w-43,24,str(doc.page));c.restoreState()
W=A4[0]-86
ST += [Spacer(1,42),p('OCEANAUDIO / ENGINEERING REVIEW','H2Audit'),Spacer(1,20),p('A smaller\nSynthDef library'.replace('\n',' '),'TitleBig'),p('Consolidation plan, category design and complete source catalog','Deck'),Spacer(1,18)]
a=D['summary']['first_pass_reduction'];b=D['summary']['second_pass_reduction']
table([['SOURCE FILES','AUTO-COMPILE CREATORS','FIRST-PASS REDUCTION'],[str(len(F)),str(len(ACTIVE)),str(a)+' entries']], [W/3]*3)
ST += [Spacer(1,24)]
body('The objective is to reduce the number of maintained engines and selectable SynthDef entries while preserving existing patches and their sound. This report proposes a bounded first pass, followed by explicit algorithm-profile consolidation where the musical benefit justifies the complexity.')
body('No production SynthDef source, preset, macro or compiler behavior was changed for this audit. Consolidation counts below are proposals, not completed migrations or proven audio-equivalence results.')
small('Snapshot: 23 October 2026. Sources under synthdefs/. Baseline commit '+D['snapshot']['git_head'][:12]+', plus the live working tree captured in the accompanying SHA-256 inventory. Concurrent workspace edits are outside this audit; the catalog is a fixed source snapshot.')
small('Prepared for Santiago Vilanova. The inventory covers every .scd source, including runtime contracts, old archives, code examples, experiments and one binary-dependent placeholder.')
page('Contents')
toc=TableOfContents();toc.levelStyles=[S['TOC0'],S['TOC1']];ST.append(toc)
page('1. Decision and reduction targets')
body(f'Start with the {len([g for g in review.GROUPS if g["phase"]==1])} first-pass groups: {sum(len(g["names"]) for g in review.GROUPS if g["phase"]==1)} current creator entries become {len([g for g in review.GROUPS if g["phase"]==1])} canonical families, removing {a} selectable entries. Across the {len(ACTIVE)} auto-compile-eligible creator declarations, that is {len(ACTIVE)-a} remaining entries ({100*a/len(ACTIVE):.1f}% fewer).')
body(f'The second pass identifies another {b} potential entry reductions across {len([g for g in review.GROUPS if g["phase"]==2])} groups. Completing both passes would take this creator subset from {len(ACTIVE)} to {len(ACTIVE)-a-b} ({100*(a+b)/len(ACTIVE):.1f}% fewer). This is a conditional planning target: some modes may prove too expensive or too awkward to expose together.')
body('These figures do not count runtime raw SynthDefs, archive files or experiments as deletable library entries. They also do not assume that all compiled binaries disappear. Structural variants remain necessary wherever array length, channel count or buffer allocation changes the graph.')
table([['What gets smaller','How to measure it','Proposed approach'],['Browser entries','Canonical user-facing family IDs','Modes/profiles inside focused nodes; migrate old IDs through aliases.'],['Maintained generators','Independent DSP implementations','Share repeated wrapper code and generate structural variants from one family definition.'],['Compiled graphs','Unique DSP graph + channels + structural choices','Deduplicate exact graphs; build only requested variants. Runtime type modes can reduce graph count at a CPU cost.'],['Shipped/build inventory','Variants actually installed or prebuilt','Demand-driven compilation/cache policy; profile real preset usage first.']], [105,125,W-230])
heading('The immediate duplicate',1)
body('MonoBandPassBank32 and MonoBandPassBank64 have byte-identical generator bodies after comment/whitespace normalization. Both set numBands = 64. The two entries can share a canonical graph; old 32 must load the 64-band profile to preserve its present sound. Changing it to 32 bands is a separate correction.')
small('Evidence: Effect/Filter/monobandpassbank256.scd; see A01 and the full catalog. No duplicate literal creator names or whole normalized .scd files were found in this snapshot. That does not establish that all other sounds are unique.')
heading('What should remain atomic',1)
body('Do not build one enormous SynthDef for all synthesis or all effects. Collapse wrappers and closely related families. Keep specialized sample-accurate feedback, envelope, filter and random kernels. Parameter mode selection using Select.ar normally leaves every branch running; a smaller browser alone can hide a large CPU regression.')
body('Use compile-time structural choices for bank size, oversampling or large algorithm topology; use runtime modes for cheap transfer equations and controls that genuinely share a graph. Label these separately in the UI and inventory.')
page('2. Scope, evidence and limits')
rows=[['Source role','Files','Treatment']]
for role,n in D['summary']['status_files'].items():
 desc={'Library':'Normal metadata-backed sources.','Runtime':'Server/C++ node contracts, often generating multiple names/channel counts.','Compatibility':'Six creator adapters plus the old raw source archive.','Build tools':'Compiler/prelude/debug scripts; not browser SynthDef entries.','Code sandbox':'Capture/promote tools and examples.','Binary-dependent':'OteyPiano placeholder; source is not reproducibly rebuildable.','Experiment':'Excluded from normal source sync.'}[role];rows.append([role,str(n),desc])
table(rows,[105,48,W-153])
body(f'The scan found {len(F)} .scd files, {D["summary"]["creator_declarations"]} literal ~synthCreator declarations, {len(ACTIVE)} eligible creator declarations and {D["summary"]["eligible_sources"]} auto-compile-eligible source files. Counts are source templates/declarations, not the number of installed channel-count variants. Raw SynthDef calls can generate whole families in loops; their {D["summary"]["raw_synthdef_declarations"]} declaration sites are cataloged without pretending they are a concrete binary total.')
body('Method: comment-aware lexical scan of every source, balanced creator-function extraction, metadata/parameter/UGen inventory, normalized body hashes, numeric-only difference grouping, token-similarity triage within categories, and manual comparison of the candidate DSP bodies. Similarity is used to find candidates, not to assert equivalent sound.')
body('Compile eligibility follows src/synthdefs/scSynthdefSync.cpp: Defaults scripts are included; Oceanode, OceanodeCode, WIP and compiled-output folders are excluded; other folders require a creator call and no @oceanode-no-autocompile marker. Compatibility/OceanAudio adapters therefore remain eligible. Location in Compatibility alone does not disable a source.')
body('Limit: this is a source review, with targeted manual examination of consolidation families. The audit does not execute arbitrary archive/example scripts or render every parameter combination. Descriptions marked metadata reproduce source intent; algorithm signatures are static evidence, not an evaluation of quality. Raw names expressed by concatenation are shown as expressions/families, not expanded binary lists.')
body('The PDF lists current categories before proposed destinations. Every .scd path appears in a catalog; every literal creator declaration has its own entry. Raw/support files list all detected SynthDef declaration sites with their name expression and core algorithm signature. CSV/JSON provide complete extracted controls, hashes and proposed categories.')
page('3. First-pass consolidation')
body('These are the most tractable families. Low means relatively local implementation work, not audio-verified equivalence. Each group needs preset migration and the sound-preservation checks in section 7. The exact 32/64 duplicate is counted once within A01.')
for g in review.GROUPS:
 if g['phase']!=1:continue
 heading(f'{g["id"]}  {g["target"]}  |  {len(g["names"])} to 1',1)
 body('Candidates: '+', '.join(g['names'])+'.')
 body(g['reason'])
 body('Preserve: '+g['preserve'])
 small('Implementation: '+g['kind']+'. Risk: '+g['risk']+'. Evidence: '+'; '.join(dict.fromkeys(NAMES[n]['path']+':'+str(NAMES[n]['line']) for n in g['names'])))
page('4. Versioned and deeper consolidation')
body('Suffixes A/B/C do not consistently mean successive compatible versions. The following families can be presented through one focused node only if each old algorithm survives as a named mode/profile. Reusing a mode label without preserving the DSP, parameter contract and initial state is not a sound-preserving reduction.')
for g in review.GROUPS:
 if g['phase']!=2:continue
 heading(f'{g["id"]}  {g["target"]}  |  {len(g["names"])} to 1',1)
 body('Candidates: '+', '.join(g['names'])+'.');body(g['reason']);body('Preserve: '+g['preserve'])
 small('Implementation: '+g['kind']+'. Risk: '+g['risk']+'. Evidence: '+'; '.join(dict.fromkeys(NAMES[n]['path']+':'+str(NAMES[n]['line']) for n in g['names'])))
heading('Keep separate until stronger evidence exists',1)
for t in [
'Cymbal and CymbalB: a stochastic Ringz crash generator versus a much larger dispersion/comb/feedback instrument. B is not a compatible superset; prefer descriptive instrument names, not a forced merge.',
'BubbleA and BubbleB: a sine pitch-envelope voice versus noise driving a TwoTube/filter-bank model. The similar names hide different algorithms.',
'Pluck and PluckResonator: they share DWGPluckedStiff, but one supplies an internal noise/envelope excitation and safety/output stages, while the other consumes an audio input. Share a private core; keep source and processor roles separate.',
'StereoPan2, Balance, Panner, PanAz and XPanAz: output topology, channel summing and panning laws differ. A shared spatial core may help maintenance, but a balance control is not interchangeable with equal-power mono panning.',
'LadderMultimode, LadderFilter, MoogFilter, MoogVCF, CompensatedSVF and other filters: model-specific nonlinearities, compensation and integration matter. A multimode label does not prove DSP coverage.',
'Noise, RandomValues and CongruentialRandom: distribution, state, seed/reset behavior and update rate are part of the sound. The seeded per-sample congruential mode cannot be replaced with generic white noise.',
'BloobsNoise/BloobsBandpass/BloobsStereoPan: existing optimized profiles preserve old ordering, control rates and gains. Move them into normal role categories with a legacy/performance tag; do not delete before checking the known 78-channel presets.',
'XPanAzLegacy and FormsLegacy: Legacy marks a compatibility contract. Retain hidden adapters until referenced graphs are migrated and exact behavior is covered.',
'OteyPiano: @oceanode-no-autocompile marks a lost/unreproducible source placeholder. Preserve shipped binaries and document provenance; never count removal as simplification.'
]:body(t)
heading('Larger opportunity: instrument data versus engine code',1)
body('Many named modal resonators repeat DynKlank/Ringz bank construction with instrument-specific ratios, amplitudes and decay formulas. A shared ModalResonator engine plus model data/profiles could remove further entries. However, BellResonator uses moving/random partials and startup glides; MarimbaResonator uses position-dependent amplitudes, a 30-mode formula and a final limiter. These are not merely interchangeable constant tables. This opportunity is deliberately excluded from the quantified target until every model is mapped.')
body('Similarly, physical-model sources can share excitation and modal-building utilities without merging waveguides, membrane UGens, piano models and acoustic approximations into a universal instrument. Keep model-specific behavior in small reusable components or data profiles, and keep instruments/macros outside the generic DSP library.')
page('5. A category structure that supports reduction')
body('Use signal role first and algorithm family second. Keep model, provenance, legacy compatibility, optimization, oversampling and preferred channel count as tags. One canonical family ID should own its profiles; folders and display labels must not be the identity used by saved graphs.')
table([['Primary branch','Scope'],['Source','Generators: oscillator, additive, FM, phase distortion, pulsar, noise/stochastic, physical model, percussion, sampler and named voice macros.'],['Effect','Input-processing DSP: filter, EQ, nonlinear, dynamics, modulation, pitch, delay, loop, freeze, blur, reverb, resonator, spectral and filter-bank delay.'],['Control','Envelope, oscillator, phase, random, sequence, event, transport, quantize and value utilities. Preserve actual ar/kr rates separately.'],['Routing','Mix, gain, channel mapping, feedback plumbing, polarity, band splitting and encoding.'],['Spatial','Panning, width/decorrelation, binaural and rotation.'],['Analysis / Utility / IO','Measurement, mathematical/conversion/signal hygiene tools and external audio endpoints.'],['Runtime','Contract-owned implementations supporting C++ nodes/server infrastructure; hidden from generic engine browsing.'],['Compatibility / Archive / Development','Live migration adapters, inactive legacy references, experimental/code examples and build tools as distinct statuses.']], [115,W-115])
heading('Concrete category corrections',1)
move_names=['DynKlang','PluckResonator','TransientShaper','HarmonicExciter','Gain','LinearGain','Sanitize','DCCorrection','PhaseInvert','RingmodSidechain','SampleRate','SampleAndHold','ImpulseCounter','PhasorCycleTrigger','Transport','transportPhasor','TransportPulse','ImpulseQuantizer','GateQuantizer','ValueQuantizer','ChannelDecorrelator','MidSide','InFeedback','FeedbackNode','SuperFM7','SuperFMDrone','BloobsNoise','BloobsBandpass','BloobsStereoPan','MonoSpectralDelay512','SpectralDelay512']
rows=[['Entry','Current category','Proposed category']]
for n in move_names:
 if n in NAMES:
  e=NAMES[n];rows.append([n,e['category'],e['proposed_category']])
table(rows,[145,160,W-305])
heading('Replace inconsistent umbrellas',1)
body('Source/Analog becomes Source/Oscillator: not every waveform is an analog model. Source/Oversampling joins oscillator families with a quality tag. Source/Special is split by actual method: AliasingSynth to Digital, Lissajous to Chaotic, Recursive to Feedback, SigmoidOSC to PhaseDistortion, InterferenceRhythm to Rhythm, BubbleB to PhysicalModel and BubbleA to Percussion. Named bass/voice instruments remain discoverable as voice profiles/macros.')
body('Effect/Time/Phase becomes Effect/ModulatedDelay for chorus/flanger families, with phasers in Effect/Phaser; PhaseInvert moves to Routing/Polarity. Effect/Spectral retains FFT/PV algorithms; the two 512-band BPF/CombC delay engines move to Effect/FilterbankDelay. Normalize Eq/EQ, WaveTable/Wavetable, case, underscores and trailing digits through display metadata, without rewriting saved IDs blindly.')
heading('Folder versus declared-category discrepancies',1)
body('The following paths do not match their declared category. Some are intentional compatibility/runtime distinctions, so resolve through metadata and role rules rather than treating every mismatch as a bug.')
rows=[['Source path','Declared category']]
for e in ACTIVE:
 if str(Path(e['path']).parent)!=e['category']:rows.append([e['path'],e['category']])
table(rows,[W-160,160])
page('6. Naming, profiles and lifecycle')
body('Use purpose-based canonical names such as BasicOscillator, BandpassBank, RatioResonatorBank and RationalSaturator. Expose descriptive modes such as Random10Tap / Sine6TapFeedback for Chorus and PeakSlope / PeakKnee / LinkedLookahead for Compressor. Avoid A/B/C labels for new work; retain them only in migration records.')
body('Use PeakNormalizer as a clearer display name for Cuntpressor: its source implements peak-normalizing AGC plus clipping, rather than a conventional ratio compressor. Retain its old ID as an alias. This is a naming correction, not an additional quantified merge.')
body('Do not rename established identities simply to make labels uniform. Add canonical IDs, an alias map and explicit parameter migrations. Each alias must record algorithm profile, structural size, channel topology, unit conversion, defaults and compatibility gain/pan laws. Resolve it at preset-load time; an alias does not need its own compiled SynthDef forever.')
body('Separate source modules from public nodes: small generic DSP builders can be shared by multiple family profiles. Store model ratios/decays in data where they truly are data. Store complete instruments and live controls in Oceanode macros. Avoid one bespoke .scd per preset and avoid one universal .scd for the entire library.')
body('A registry entry should carry stable ID, display name, category/tags, status, source, algorithm/profile, required UGens, parameter schema, structural options, output topology, compatibility aliases and validation evidence. Generate browser metadata and the build manifest from that registry so categorization cannot drift from folders.')
body('For footprint reduction, enumerate actual referenced variants before deleting binaries. The library already has N-channel and other build variables: compile the selected structure on demand, cache by graph identity/ABI and retain a small prebuilt set. This is a separate compiler/package change; no binary savings are claimed without measuring that manifest.')
STYLE_MODE=True
page('7. Implementation order and sound-preservation gates')
for title,text in [
('Step 1 - Freeze the baseline','Save a source/build manifest and reference renders, including accepted Organism Microdelay and SpaceSystem-Geiger states and GUI values. Measure browser entries, generators, graph variants, footprint, compile time and CPU separately.'),
('Step 2 - Remove the proven duplicate','Alias MonoBandPassBank32 to the current 64-band graph. Verify serialized array sizes and metadata. Do not regenerate the old 32 node as 32 bands. Confirm graph equivalence and identical output with the same test input.'),
('Step 3 - Implement the first-pass families','Start with size-only Additive/PanAz/PanX generators and the small gain/saturation/filter wrappers. Keep old IDs loadable through migration profiles. For every family record before/after entry counts and compiled variant counts separately.'),
('Step 4 - Measure runtime mode cost','Compare CPU at 1, 2 and high channel counts, including Bloobs at 78. If runtime branch selection is too expensive, use a build-time mode or specialized native mode UGen, retaining multiple cached graphs.'),
('Step 5 - Migrate versioned engines selectively','Implement second-pass groups only with clear benefit and explicit old-algorithm profiles. Hiding names does not complete a migration.'),
('Step 6 - Retire generated variants safely','After preset/macro migration and regression checks, prune redundant outputs and schedule alias retirement. Runtime contracts and unreproducible binaries need separate ownership decisions.')]:heading(title,1);body(text)
heading('Required checks for a sound-preserving merge',1)
body('For deterministic graphs: identical input, sample rate, block size, seed and initial state; compare output samples after controlled startup. Record max/RMS error and latency alignment. Exact graph deduplication should be exact; numerical reimplementations need an agreed tolerance and listening evidence. Never normalize levels to hide a mismatch.')
body('For random/chaotic/stateful graphs: fix seeds when reproducible, preserve random UGen ordering and reset timing; otherwise compare repeated renders statistically and listen to attacks, tails and feedback stability. Equal spectral averages do not prove the same time behavior.')
body('Exercise defaults, dry/wet endpoints and midpoint, parameter modulation rates, retrigger while gate stays high, minimum/maximum delay, polarity, multi-channel routing, bank boundaries and extreme resonances. Preserve linear versus equal-power mix and pre-send versus untouched dry taps. Sample-accurate feedback must not become block-delayed LocalIn/LocalOut feedback.')
body('Verify old presets, macros, connections and live GUI controls load without dropped parameters. Use original saved values, not regenerated defaults. Keep DSP corrections separate from consolidation so an intentional sound change cannot be disguised as cleanup.')
# All category inventory, including non-browser directories.
STYLE_MODE=False
page('8. Current category directory')
body('Counts below are catalog records: one per literal creator, otherwise one per raw/support .scd source. This makes all source locations visible without inflating dynamic runtime families into imaginary concrete SynthDef counts. Full entries follow in sections 9 and 10.')
CAT=scan._group('category',E)
for cat,items in sorted(CAT.items(),key=lambda x:x[0].lower()):
 heading(f'{cat or "(undeclared)"}  |  {len(items)}',1)
 desc=review.CATEGORY_DESC.get(cat)
 if not desc:
  if cat.startswith('Defaults/'):
   desc='Runtime support for '+cat.split('/',1)[1]+'. Names/parameters may be contracted by the corresponding C++ node; generated variants are documented per source.'
  elif cat=='Compatibility/Old':desc='Inactive old-format source archive. May include audition code and repeated historical implementations; not eligible for normal automatic source compilation.'
  elif cat.startswith('OceanodeCode/examples/'):desc='Code-editor examples for '+cat.rsplit('/',1)[-1]+'; not part of the normal auto-compiled library.'
  elif cat.startswith('OceanodeCode'):desc='Code capture/promotion support for the editor workflow.'
  elif cat.startswith('Oceanode'):desc='Build/prelude/debug support rather than selectable audio engines.'
  elif cat.startswith('WIP/'):desc='Experimental '+cat.split('/',1)[1]+' code, excluded from normal deployment; not a deletion saving in the active browser.'
  else:desc='Source grouping retained from current metadata; entries below document its algorithms and proposed destinations.'
 body(desc)
page('9. Metadata-backed SynthDef catalog')
body('All literal creator declarations, grouped by current declared category. Each entry includes source-relative path/line, extracted description, core algorithm signature, controls and proposed destination. Descriptions are source metadata; signatures/controls are extracted from source. A/B references point to the consolidation plan. Binary-dependent and runtime/adapter entries are labeled explicitly.')
CREATORS=[e for e in E if e['body_hash']]
for cat,items in sorted(scan._group('category',CREATORS).items(),key=lambda x:x[0].lower()):
 heading(cat or '(undeclared)',1)
 for e in sorted(items,key=lambda x:x['name'].lower()):
  id='s'+hashlib.sha1((e['path']+str(e['line'])).encode()).hexdigest()[:12]
  title=rich('<a name="'+id+'"/>'+txt(e['name'])+' <font color="#087F8C" size="8">'+txt(' / '.join(e['review_groups']))+'</font>','EntryTitle')
  desc=e['description'] or 'Metadata description missing. Source algorithm: '+', '.join(e['ugens'])+'.'
  sig=', '.join(e['ugens']) or 'Arithmetic/control processing; no distinct core UGen call detected.'
  controls=', '.join(e['parameters']) or 'No literal Oceanode parameter names detected.'
  block=[title,p(desc),p('Algorithm signature: '+sig,'SmallAudit'),p('Controls: '+controls,'SmallAudit'),p('Source: '+e['path']+':'+str(e['line'])+' | '+e['status']+(' | auto-compile' if e['eligible'] else ' | no auto-compile'),'TinyAudit'),p('Proposed: '+e['proposed_category'],'TinyAudit'),HRFlowable(width='100%',thickness=.35,color=LIGHT,spaceBefore=3,spaceAfter=3)]
  ST.append(KeepTogether(block))
page('10. Runtime, archive, examples and support catalog')
body('Every source without a literal creator is listed here. Named raw definitions appear as their source name expressions; concatenations/loops identify families. Descriptions use matching metadata or are inferred from names and core calls. Each declaration has a line number and partial static algorithm signature. Runtime contracts must be reviewed against C++ consumers before consolidation. Archive/examples are not counted in the proposed active-entry reduction.')
def raw_function(rd):
 u=set(rd['ugens']);name=rd['expression'].lower()
 if any(x in name for x in ['scope','meter','analyz','tracker','info']):return 'Signal measurement or analysis support.'
 if 'rec' in name and u.intersection({'BufWr','RecordBuf'}):return 'Record incoming audio into a buffer.'
 if 'vst' in name:return 'Host and route audio through a VST plugin.'
 if any(x in name for x in ['mixer','matrix','router','output']):return 'Mix or route audio between channels/buses.'
 if u.intersection({'BufRd','PlayBuf','TGrains','GrainBuf','BufRateScale'}):return 'Read, resample or granularly process buffered audio.'
 if any(x.startswith('PV_') for x in u) or u.intersection({'FFT','IFFT'}):return 'Transform/process frequency-domain audio and reconstruct output.'
 if u.intersection({'DWGPluckedStiff','DWGBowed','DWGFlute','DWGClarinet','DWGPlucked'}):return 'Waveguide physical-model excitation and resonant synthesis.'
 if u.intersection({'DynKlank','Klank','Ringz'}):return 'Excite or process a modal/resonator bank.'
 if u.intersection({'FreeVerb','FreeVerb2','JPverb','Greyhole','GVerb'}):return 'Generate a reverberant/diffuse decay from input audio.'
 if 'EnvGen' in u and u.intersection({'SinOsc','PMOsc','LFTri','WhiteNoise'}):return 'Envelope-shaped oscillator/noise voice with model-specific excitation.'
 if u.intersection({'Compander','Limiter','Amplitude'}):return 'Level detection, dynamics or amplitude control.'
 if u.intersection({'CombC','CombL','AllpassC','AllpassN','DelayC','DelayL','DelayN'}):return 'Delay, feedback or diffusion network; topology defined by this source.'
 if u.intersection({'LPF','HPF','BPF','RLPF','RHPF','MoogFF','MoogVCF'}):return 'Frequency-selective filtering or filtered synthesis.'
 if u.intersection({'PanAz','PanX','Pan2','Balance2'}):return 'Spatial placement or channel distribution.'
 if u.intersection({'Phasor','Impulse','Demand','PulseCount','TDuty','Duty'}):return 'Phase/event generation or event-driven signal processing.'
 if u.intersection({'SinOsc','Saw','LFSaw','LFPulse','LFTri','PMOsc'}):return 'Periodic oscillator synthesis and modulation.'
 if any('Noise' in x for x in u):return 'Noise/stochastic signal generation.'
 return 'Bus/control, arithmetic or helper-based implementation; inspect the listed source contract.'

RAW=[e for e in E if not e['body_hash']]
OVERRIDES={
'Oceanode/compileExact.scd':'Exact requested-variant compilation entry point.',
'Oceanode/mainDefinitions.scd':'Shared SynthDef builder prelude and parameter/metadata helpers.',
'Oceanode/syncCompile.scd':'Source-sync compilation wrapper and output harvesting support.',
'Oceanode/debug_commands.scd':'Manual server and graph debugging commands.',
'OceanodeCode/captureCode.scd':'Capture code/SynthDef output for the live editor workflow.',
'OceanodeCode/promote.scd':'Promote editor code into a reusable library source.',
'Defaults/Core/info.scd':'Info<n> amplitude/peak/value analysis to control buses; n=1..100.',
'Defaults/Core/output.scd':'Server output and channel-count output routing contract.',
'Defaults/Core/bufalloc.scd':'Buffer allocation helper used by server/node infrastructure.',
}
for role in ['Runtime','Compatibility','Build tools','Code sandbox','Experiment']:
 group=[e for e in RAW if e['status']==role]
 if not group:continue
 heading(role,1)
 for e in sorted(group,key=lambda x:x['path'].lower()):
  f=FM[e['path']];defs=f['raw_definitions'];core=', '.join(e['ugens'])
  desc=OVERRIDES.get(e['path'])
  if not desc:
   # Derive a concise description from file role/name, supplemented by extracted algorithm evidence.
   topic=re.sub(r'(?<=[a-z])(?=[A-Z])',' ',e['name']).replace('_',' ')
   desc={'Runtime':'Runtime implementation for ','Compatibility':'Archived implementation of ','Code sandbox':'Editor example/support for ','Experiment':'Experimental implementation of ','Build tools':'Build support for '}[role]+topic+'.'
  block=[p(e['path'],'EntryTitle'),p(desc),p('Proposed: '+e['proposed_category']+' | '+('auto-compile eligible' if e['eligible'] else 'excluded from automatic compilation'),'SmallAudit')]
  if not defs:
   block.append(p('No direct SynthDef declaration detected; script/prelude content or indirect helper-generated family. Algorithm evidence: '+(core or 'language-level build/control logic')+'.','SmallAudit'))
   if e['description']:block.append(p('Source note: '+e['description'][:550],'TinyAudit'))
  ST.append(KeepTogether(block))
  for rd in defs:
   expr=re.sub(r'\s+',' ',rd['expression']);ug=', '.join(rd['ugens']) or 'arithmetic, control/bus or helper-based DSP'
   base=re.sub(r'[^a-z0-9]','',expr.lower().strip('\\\"\''))
   base=re.sub(r'(?:mono|13|78|6|2)$','',base)
   match=next((x for x in CREATORS if re.sub(r'[^a-z0-9]','',x['name'].lower())==base and x['description']),None)
   function=match['description'] if match else raw_function(rd)
   ST.append(p('Line '+str(rd['line'])+' | '+expr+' - '+function+' Core: '+ug+'.','SmallAudit'))
  ST.append(HRFlowable(width='100%',thickness=.35,color=LIGHT,spaceBefore=3,spaceAfter=5))
page('11. Machine-readable inventory and reproducibility')
body('The accompanying synthdef-inventory.csv provides one row per catalog record with current/proposed categories, controls, source location, status and review-group links. synthdef-inventory.json adds every source hash, raw declaration expressions and the exact consolidation plan. These are the exhaustive migration worklists for a later implementation.')
body('Rebuild the snapshot with tools/synthdef_audit/scan.py, then build_report.py using Python with reportlab. The review contains curated conclusions in review.py. Regeneration reads the working tree, so counts can change if another task edits sources. This report uses only its saved JSON snapshot for catalog content.')
small('Source root: '+str(ROOT/'synthdefs'))
small('Baseline Git commit: '+D['snapshot']['git_head'])
small('Manifest SHA-256: '+D['snapshot']['manifest_sha256'])
body('Further evidence: synthdefs/README.md describes generic compilation, runtime contracts and archived sources. src/synthdefs/scSynthdefSync.cpp defines scan eligibility. synthdefs/Optimized/Bloobs/README.md documents the 78-channel profiles and measured performance motivation. Candidate source paths and line numbers are listed with every recommendation and catalog entry.')
body('Coverage check: every inventoried .scd path belongs to either the creator catalog or raw/support catalog; every detected literal creator appears once. No compiled .scsyndef artifacts are counted as source entries. Auxiliary .sc classes, native UGen code and data tables are dependencies, not additional SynthDef entries: preserve them during migration, including OceanodePrimitives and resonator ratio/decay tables.')
PDF=OUT/'OceanAudio-SynthDef-Library-Review.pdf'
doc=AuditDoc(str(PDF),pagesize=A4,rightMargin=43,leftMargin=43,topMargin=52,bottomMargin=45,title='OceanAudio SynthDef Library Review',author='OceanAudio / Santiago Vilanova')
doc.multiBuild(ST,onFirstPage=doc.header,onLaterPages=doc.header)
print(json.dumps({'pdf':str(PDF),'summary':D['summary']},indent=2))
