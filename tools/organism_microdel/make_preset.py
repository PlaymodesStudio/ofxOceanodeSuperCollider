#!/usr/bin/env python3
"""Build only organism_microdel.ens's saved opening state, never its snapshots."""
import json, pathlib, sys
from patch import Patch
DATA=pathlib.Path(sys.argv[1]); HERE=pathlib.Path(__file__).resolve().parent
STATE=json.loads((HERE/'opening-state.json').read_text()); S=STATE['controls']; VOICES=STATE['voices']
def val(mid):return S[str(mid)]['value']
def controls(m,items):
 for name,v,lo,hi in items:m.control(name,v,lo,hi)
def bind(m,name,dst,param):m.bind(m.controls[name][0],dst,param)
def output(m,src,name='Out',x=1800,y=0):
 r=m.bus(name,x,y);m.connect(src,'Out',r,'Value');return r
def bus(m,name,x=-900,y=-400):return m.bus(name,x,y)
def scaled(m,src,lo,hi,n,x=0,y=0):
 k=m.sc('Mapper',n,{'Mininput':0,'Maxinput':1,'Minoutput':lo,'Maxoutput':hi},x,y);m.connect(src,'Out',k,'In');return k

def events():
 m=Patch();controls(m,[('Run',1,0,1),('BPM',100,10,400),('Change Beats',val(1162),1,32),('Random Timing',1,0,1),('Pitch Span',val(1227),0,128)])
 clk=m.sc('RandomBeatClock',VOICES,{'Division':2},-750,0)
 for name,param in [('Run','Run'),('BPM','Bpm'),('Change Beats','Change'),('Random Timing','Randomize')]:bind(m,name,clk,param)
 output(m,clk,'Trig',600,-300)
 for i,name in enumerate(['Pitch','Attack','Decay','Cutoff','Pan','Wave']):
  r=m.sc('CongruentialRandom',VOICES,{'Seed':{'Attack':3,'Decay':5,'Cutoff':5,'Pan':5}.get(name,0),'Mode':0},-400,i*240);m.connect(clk,'Out',r,'Trigger')
  if name not in ('Cutoff','Pitch','Pan'):
   absolute=m.sc('UnaryOps',VOICES,{'Operation':0},-200,i*240);m.connect(r,'Out',absolute,'In');r=absolute
  if name=='Pitch':
   span=m.dc(m.controls['Pitch Span'][0],-500,-450)
   multiply=m.sc('Arithmetic',VOICES,{'Operation':2},0,-450)
   m.connect(r,'Out',multiply,'In1');m.connect(span,'Out',multiply,'In2');r=multiply
   # Arbitrary step quantization is not present in Mapper; opening Pitch Q=0.
  output(m,r,name,600,i*240)
 return m

def voices():
 m=Patch();controls(m,[('On',1,0,1),('Base Pitch',0,-60,84),('Attack',val(22),-20,100),('Decay',val(23),-20,100),('Sustain',val(26),0,1),('Release',val(27),0,80),('Cut Amount',val(56),0,1),('Resonance',val(2097),0,.98),('Filter Attack',val(46),-20,100),('Filter Decay',val(47),-20,100),('Filter Sustain',val(50),0,1),('Filter Release',val(51),0,80),('Filter Env',val(2499),0,1),('Level',1,0,2)])
 inp={name:bus(m,name,-900,i*170) for i,name in enumerate(['Trig','Pitch','Attack Mod','Decay Mod','Cutoff Mod'])}
 # Explicitly named controls for audio-rate randomness (blockless routing).
 p=m.gain(VOICES,[],x=-500,y=-350);m.connect(inp['Pitch'],'Value',p,'In')
 base=m.sc('Mapper',VOICES,{'Mininput':0,'Maxinput':1,'Minoutput':0,'Maxoutput':0},-700,-500);bind(m,'Base Pitch',base,'Minoutput');p2=m.gain(VOICES,[p,base],x=-200,y=-350)
 # P is an audio pin in the original cell: its integer seed is rewritten
 # every sample. This produces stepped values, not free-running white noise.
 noise=m.sc('CongruentialRandom',VOICES,{'Mode':2},100,-350);m.connect(p2,'Out',noise,'Seed')
 env=m.sc('EventADSR',VOICES,{},100,0);m.connect(inp['Trig'],'Value',env,'Trigger')
 bind(m,'Sustain',env,'Sustain')
 relcode=m.dc(m.controls['Release'][0],-400,800);relms=m.sc('Conversions',1,{'Conversion':7},-100,800);m.connect(relcode,'Out',relms,'Input');relwide=m.sc('ChannelDuplicator',VOICES,{'Idx':[0]*VOICES},200,800);m.connect(relms,'Out',relwide,'In');m.connect(relwide,'Out',env,'Release')
 for name,label,param,y in [('Attack Mod','Attack','Attack',200),('Decay Mod','Decay','Decay',450)]:
  g=m.sc('Mapper',VOICES,{'Mininput':0,'Maxinput':1,'Minoutput':0,'Maxoutput':val(22) if label=='Attack' else val(23)},-400,y);m.connect(inp[name],'Value',g,'In');bind(m,label,g,'Maxoutput');ms=m.sc('Conversions',VOICES,{'Conversion':7},-100,y);m.connect(g,'Out',ms,'Input');m.connect(ms,'Out',env,param)
 amp=m.gain(VOICES,[noise],x=400,y=-350);m.connect(env,'Out',amp,'Gain')
 fe=m.sc('EventADSR',VOICES,{},400,200);m.connect(inp['Trig'],'Value',fe,'Trigger')
 bind(m,'Filter Sustain',fe,'Sustain')
 for j,(name,param) in enumerate([('Filter Attack','Attack'),('Filter Decay','Decay'),('Filter Release','Release')]):
  code=m.sc('Mapper',VOICES,{'Mininput':0,'Maxinput':1,'Minoutput':val({'Attack':46,'Decay':47,'Release':51}[param]),'Maxoutput':0},650,700+j*220);bind(m,name,code,'Minoutput');ms=m.sc('Conversions',VOICES,{'Conversion':7},900,700+j*220);m.connect(code,'Out',ms,'Input');m.connect(ms,'Out',fe,param)
 amount=m.gain(VOICES,[fe],x=650,y=250);bind(m,'Filter Env',amount,'Gain')
 amount2=m.gain(VOICES,[amount],x=900,y=250);bind(m,'Cut Amount',amount2,'Gain')
 cut=m.gain(VOICES,[amount2],x=1150,y=250);m.connect(inp['Cutoff Mod'],'Value',cut,'Gain')
 # Bipolar random modulation selects the high-pass or low-pass sweep.
 # Build the two piecewise pitch curves from existing Mapper and sum nodes.
 hp_sweep=scaled(m,cut,0,120,VOICES,650,-350)
 hp_floor=scaled(m,cut,-60,0,VOICES,650,-950)
 m.nodes[hp_floor][1].update(Mininput=-.0001,Maxinput=0)
 hp=m.gain(VOICES,[hp_sweep,hp_floor],x=900,y=-950)
 lp_sweep=scaled(m,cut,120,0,VOICES,650,-700)
 m.nodes[lp_sweep][1].update(Mininput=-1,Maxinput=0)
 lp_ceiling=scaled(m,cut,0,120,VOICES,650,-1200)
 m.nodes[lp_ceiling][1].update(Mininput=0,Maxinput=.0001)
 lp=m.gain(VOICES,[lp_sweep,lp_ceiling],x=900,y=-1200)
 h=m.sc('CompensatedSVF',VOICES,{'Type':0},900,-350);m.connect(amp,'Out',h,'In');m.connect(hp,'Out',h,'Pitch');bind(m,'Resonance',h,'Res')
 l=m.sc('CompensatedSVF',VOICES,{'Type':2},1150,-350);m.connect(h,'Out',l,'In');m.connect(lp,'Out',l,'Pitch');bind(m,'Resonance',l,'Res')
 # Preserve the saved instrument voice count and its unnormalized combiner.
 summed=m.sc('NtoM',VOICES,{'NumOut':'1','Level':1},1400,-350);m.connect(l,'Out',summed,'In')
 gain=m.gain(1,[summed],x=1650,y=-350);bind(m,'Level',gain,'Gain')
 enabled=m.gain(1,[gain],x=1900,y=-350);bind(m,'On',enabled,'Gain');output(m,enabled,x=2150)
 return m

def modulation():
 m=Patch();controls(m,[('BPM',100,10,400),('LFO On',1,0,1),('Host Sync',1-val(1877),0,1),('Beat Length',round(val(1489)*4)/4,.25,32),('Range ms',val(1484),0,1000),('Offset ms',val(1542),0,500),('Power',val(1546),.125,8)])
 # All composition lives here: beat period -> slow pulse -> held random ->
 # power curve -> range + offset -> first-order time smoothing.
 host_period=m.sc('Conversions',1,{'Conversion':10,'Input':4},-1000,0);bind(m,'BPM',host_period,'Bpm')
 # The saved relay selects GO / beat length; GO carries constant event value 1.
 # Host sync selects the other relay input, the host beat period.
 event_period=m.sc('Mapper',1,{'Minoutput':1000,'Maxoutput':1000},-1000,-300)
 period=m.sc('CrossfadeAB',1,{'Linear':1,'Levels':1},-700,0);m.connect(event_period,'Out',period,'Ina');m.connect(host_period,'Out',period,'Inb');bind(m,'Host Sync',period,'Xfade')
 length=m.dc(m.controls['Beat Length'][0],-700,300)
 cycle=m.sc('Arithmetic',1,{'Operation':2},-400,0);m.connect(period,'Out',cycle,'In1');m.connect(length,'Out',cycle,'In2')
 hz=m.sc('Conversions',1,{'Conversion':3},-100,0);m.connect(cycle,'Out',hz,'Input')
 edge_rate=m.gain(1,[hz],gain=2,x=100,y=-300)
 pulse=m.sc('SimpleImpulse',1,{'Amp':1},200,0);m.connect(edge_rate,'Out',pulse,'Hz')
 rng=m.sc('CongruentialRandom',1,{'Seed':0},350,0);m.connect(pulse,'Out',rng,'Trigger')
 random=m.sc('UnaryOps',1,{'Operation':0},500,0);m.connect(rng,'Out',random,'In')
 power=m.dc(m.controls['Power'][0],500,350)
 shaped=m.sc('Arithmetic',1,{'Operation':7},800,0);m.connect(random,'Out',shaped,'In1');m.connect(power,'Out',shaped,'In2')
 enabled=m.gain(1,[shaped],x=1050,y=0);bind(m,'LFO On',enabled,'Gain')
 span=m.dc(m.controls['Range ms'][0],800,550)
 ranged=m.sc('Arithmetic',1,{'Operation':2},1300,0);m.connect(enabled,'Out',ranged,'In1');m.connect(span,'Out',ranged,'In2')
 offset=m.dc(m.controls['Offset ms'][0],1050,350)
 added=m.gain(1,[ranged,offset],x=1550,y=0)
 smooth=m.sc('OnePole',1,{'Pitch':40,'Type':0},1800,0);m.connect(added,'Out',smooth,'In')
 output(m,smooth,x=2050);return m

def microdelay():
 m=Patch();controls(m,[('On',1,0,1),('Feedback',val(1557),-.99,.99),('Filter Pitch',val(1475),20,120),('Resonance',val(1476),0,.98),('Delay Mix',val(1558),0,1),('Dry Wet',val(1571),0,1),('Time ms',val(1542),0,2000),('Level',10**(val(1439)/20),0,2)])
 m.choice('Filter Type',0,['HP','BP','LP'])
 inp=bus(m,'In');dup=m.sc('ChannelDuplicator',2,{'Idx':[0,0]},-500,0);m.connect(inp,'Value',dup,'In')
 d=m.sc('FilteredFeedbackDelay',2,{},0,0);m.connect(dup,'Out',d,'In')
 for name,param in [('Feedback','Feedback'),('Filter Pitch','Pitch'),('Resonance','Res'),('Filter Type','Type'),('Delay Mix','Mix')]:bind(m,name,d,param)
 dc=m.dc(m.controls['Time ms'][0],-500,-400);td=m.sc('ChannelDuplicator',2,{'Idx':[0,0]},-200,-400);m.connect(dc,'Out',td,'In');m.connect(td,'Out',d,'Time')
 g=m.gain(2,[d],x=300,y=0);bind(m,'Level',g,'Gain')
 mix=m.sc('CrossfadeAB',2,{'Linear':1,'Levels':1},600,0);m.connect(dup,'Out',mix,'Ina');m.connect(g,'Out',mix,'Inb');bind(m,'Dry Wet',mix,'Xfade')
 on=m.sc('CrossfadeAB',2,{'Linear':1,'Levels':1},900,0);m.connect(dup,'Out',on,'Ina');m.connect(mix,'Out',on,'Inb');bind(m,'On',on,'Xfade')
 output(m,on);return m

def master():
 m=Patch();controls(m,[('Level dB',val(1888),-50,10),('Pan Motion',val(1905),0,1),('Width',val(2168),0,1),('Threshold',val(1950),-60,0),('Ratio',val(1952),0,1),('Knee',val(1963),0,48),('Attack ms',10**(val(1949)/20),.1,200),('Release ms',10**(val(1951)/20),5,2000),('Makeup dB',val(1953),-12,24),('Compressor',1,0,1),('Master',1,0,2)])
 inp=bus(m,'In');trig=bus(m,'Trig',y=-700)
 rand=m.sc('CongruentialRandom',1,{'Seed':5},-700,-600);m.connect(trig,'Value',rand,'Trigger')
 pan=m.gain(1,[rand],x=-450,y=-600);bind(m,'Pan Motion',pan,'Gain')
 ps=m.sc('OnePole',1,{'Pitch':120,'Type':0},-200,-600);m.connect(pan,'Out',ps,'In')
 pd=m.sc('ChannelDuplicator',2,{'Idx':[0,0]},0,-600);m.connect(ps,'Out',pd,'In')
 # Parabolic pan: x*(4-x)/3, x=1 +/- pan, unity at centre.
 x=m.sc('Mapper',2,{'Mininput':-1,'Maxinput':1,'Minoutput':[2,0],'Maxoutput':[0,2]},250,-600);m.connect(pd,'Out',x,'In')
 cx=m.sc('Mapper',2,{'Mininput':0,'Maxinput':2,'Minoutput':4,'Maxoutput':2},500,-600);m.connect(x,'Out',cx,'In')
 product=m.sc('Arithmetic',2,{'Operation':2},750,-600);m.connect(x,'Out',product,'In1');m.connect(cx,'Out',product,'In2')
 law=m.gain(2,[product],gain=1/3,x=1000,y=-600)
 b=m.gain(2,[],x=0,y=0);m.connect(inp,'Value',b,'In');m.connect(law,'Out',b,'Gain')
 wr=m.sc('CongruentialRandom',1,{'Seed':5},-700,350);m.connect(trig,'Value',wr,'Trigger')
 ws=m.sc('OnePole',1,{'Pitch':120,'Type':0},-450,350);m.connect(wr,'Out',ws,'In')
 wd2=m.sc('ChannelDuplicator',2,{'Idx':[0,0]},-200,350);m.connect(ws,'Out',wd2,'In')
 times=m.sc('Mapper',2,{'Mininput':[-1,0],'Maxinput':[0,1],'Minoutput':[250,0],'Maxoutput':[0,250]},0,350);m.connect(wd2,'Out',times,'In')
 wg=m.gain(2,[times],x=250,y=350);bind(m,'Width',wg,'Gain')
 wd=m.sc('Delay',2,{'Mix':1,'Delayms':[0,0]},300,0);m.connect(b,'Out',wd,'In');m.connect(wg,'Out',wd,'Delayms')
 db=m.dc(m.controls['Level dB'][0],300,600)
  # dB gain computed by a small generic conversion node.
 lev=m.sc('Conversions',1,{'Conversion':7},600,600);m.connect(db,'Out',lev,'Input')
 lev2=m.sc('ChannelDuplicator',2,{'Idx':[0,0]},800,600);m.connect(lev,'Out',lev2,'In')
 gain=m.gain(2,[wd],x=600,y=0);m.connect(lev2,'Out',gain,'Gain')
 comp=m.sc('SaturatingCompressor',2,{'Drive':val(1956)},900,0);m.connect(gain,'Out',comp,'In')
 for name,param in [('Threshold','Threshold'),('Ratio','Ratio'),('Knee','Knee'),('Attack ms','Attack'),('Release ms','Release'),('Makeup dB','Makeup'),('Compressor','Mix')]:bind(m,name,comp,param)
 sat=comp
 hp=m.sc('OnePole',2,{'Pitch':0,'Type':1},1400,0);m.connect(sat,'Out',hp,'In')
 out=m.gain(2,[hp],x=1650,y=0);bind(m,'Master',out,'Gain');output(m,out,x=1900);return m

top=Patch();ev=events();vo=voices();mo=modulation();mi=microdelay();ma=master()
keys=[top.macro(m,{},name,i*480,0) for i,(m,name) in enumerate([(ev,'ORGANISM EVENTS'),(vo,'ORGANISM VOICES'),(mo,'MICRODELAY MOTION'),(mi,'CORE MICRODELAY'),(ma,'STEREO MASTER')])]
e,v,t,d,a=keys
for label,dest in [('Trig','Trig'),('Pitch','Pitch'),('Attack','Attack Mod'),('Decay','Decay Mod'),('Cutoff','Cutoff Mod')]:top.connect(e,label,v,dest)
top.connect(v,'Out',d,'In');top.connect(t,'Out',d,'Time ms');top.connect(d,'Out',a,'In');top.connect(e,'Trig',a,'Trig')
out=top.add('SC Output',{'Chan':'0','Server':'0'},2600,0);top.connect(a,'Out',out,'In')
# One BPM router keeps stochastic events and the delay modulation on the same tempo.
bpm=top.control('BPM',100,10,400,0,-500);top.bind(bpm,e,'BPM');top.bind(bpm,t,'BPM')
widgets=[]
colors=[[233,174,92,255],[93,201,178,255],[137,158,239,255],[214,135,182,255],[215,218,229,255]]
def widget(kind,node,name,label,x,y,w=5,h=4,color=None,**config):
 path=(node+'/'+name.replace(' ','_')) if node else ''
 widgets.append({'type':kind,'parameterRef':{'parameterPath':path,'nodeDisplayName':node.replace('_',' '),'parameterDisplayName':label},'label':label,'gridX':x,'gridY':y,'spanW':w,'spanH':h,'color':color or colors[0],'config':{'interactive':True,'showValue':True,'labelColor':[232,235,242,255],'bodyColor':[31,37,48,255],**config}})
widget('Label','','','ORGANISM / MICRODELAY',1,0,40,2,valueFontScale=1.4)
widget('Label','','',f'{VOICES} stochastic voice / opening ensemble state',1,2,45,1,color=[150,165,185,255])
sections=[(e,'EVENTS',4,[('Run','RUN'),('Change Beats','CHANGE / beats'),('Random Timing','RANDOM CLOCK'),('Pitch Span','PITCH RANGE')]),(v,'VOICE BANK',12,[('On','VOICES'),('Decay','DECAY'),('Attack','ATTACK'),('Resonance','RESONANCE'),('Cut Amount','FILTER RANGE'),('Filter Decay','FILTER DECAY'),('Level','SOURCE LEVEL')]),(t,'DELAY MOTION',24,[('LFO On','MOTION'),('Host Sync','HOST SYNC'),('Beat Length','BEATS / CYCLE'),('Range ms','RANGE / ms'),('Offset ms','MIN / ms'),('Power','CURVE')]),(d,'MICRODELAY',34,[('On','DELAY'),('Feedback','FEEDBACK'),('Filter Pitch','COLOR / MIDI'),('Resonance','RESONANCE'),('Dry Wet','DRY / WET')]),(a,'STEREO / DYNAMICS',44,[('Master','MASTER'),('Level dB','LEVEL / dB'),('Width','WIDTH'),('Pan Motion','PAN MOTION'),('Threshold','THRESHOLD'),('Makeup dB','MAKEUP / dB')])]
for i,(node,title,y,items) in enumerate(sections):
 color=colors[i];widget('BackgroundPanel','','','',0,y,60,9 if i!=1 else 11,color=[24,29,38,255],showLabel=False)
 widget('Label','','',title,1,y,40,1,color=color)
 for j,(name,title) in enumerate(items):
  toggle=name in ('Run','On','Random Timing','LFO On','Host Sync')
  widget('MultiToggle' if toggle else 'Knob',node,name,title,1+j*8,y+2,7,4,color=color,rows=1,cols=1)
 if node==t:widget('Label','','','SAMPLE / HOLD',42,y+6,16,2,color=color,labelFontScale=.8)
 if node==d:widget('CustomDropdown',d,'Filter Type','FILTER',43,y+6,15,2,color=color,customOptions=['HP','BP','LP'])
widget('Knob',bpm,'Value','BPM',51,0,7,4)
widget('Label','','','Delay time is modulated by MOTION. Switch MOTION off for a fixed MIN time.\nRun stops new events; existing voices and feedback tails continue to decay.',1,54,58,3,color=[155,170,190,255],labelFontScale=.8)
top.custom_gui={'panels':[{'id':'organism-microdel-live','name':'Organism — LIVE','designMode':False,'windowState':{'posX':25,'posY':25,'width':1110,'height':1020,'isOpen':True},'layout':{'columns':60,'rows':58,'cellWidth':18,'cellHeight':16,'zoom':1,'backgroundColor':[16,20,27,255],'widgets':widgets}}]}
top.write(DATA/'Presets/DEBUG/23--ORGANISM_MICRODEL')
for name,m in [('ORGANISM_EVENTS',ev),('ORGANISM_VOICES',vo),('ORGANISM_DELAY_MOTION',mo),('CORE_MICRODELAY',mi),('ORGANISM_MASTER',ma)]:m.write(DATA/'Macros/AUDIO/REAKTOR'/name)
print('Generated opening-state preset and five reusable macros')
