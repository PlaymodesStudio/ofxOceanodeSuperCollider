# LLDB-only local reference capture for the audited Reaktor 6.5.0 arm64 binary.
# Run in a NEW isolated Reaktor process, stopped at main. Never attach to a session.
# This invokes DSP on synthetic memory, and does not open/change any ensemble.
# Binary SHA validation prevents execution against another version/layout.
import lldb,struct,json,hashlib,pathlib
APP='/Applications/Native Instruments/Reaktor 6/Reaktor 6.app/Contents/MacOS/Reaktor 6'
def dump(debugger,command,result,internal):
 expected=json.loads((pathlib.Path(__file__).resolve().parent/'primary-reference.json').read_text())['source_sha256']
 assert hashlib.sha256(open(APP,'rb').read()).hexdigest()==expected,'Different Reaktor binary: resolve and audit symbols before calling DSP entry points.'
 t=debugger.GetSelectedTarget();p=t.GetProcess()
 assert p.GetSelectedThread().GetFrameAtIndex(0).GetFunctionName()=='main','Use a new isolated process stopped at main.'
 slide=t.GetModuleAtIndex(0).GetObjectFileHeaderAddress().GetLoadAddress(t)-0x100000000
 opts=lldb.SBExpressionOptions();opts.SetLanguage(lldb.eLanguageTypeC_plus_plus);opts.SetTimeoutInMicroSeconds(10000000)
 def expr(code):
  v=t.EvaluateExpression(code,opts);assert v.GetError().Success(),str(v.GetError());return v
 def read(a,n):
  e=lldb.SBError();data=p.ReadMemory(a,n,e);assert e.Success(),str(e);return data
 def write(a,b):
  e=lldb.SBError();p.WriteMemory(a,b,e);assert e.Success(),str(e)
 expr(f'(((void(*)())0x{0x1001bf070+slide:x})(),1)')
 e=lldb.SBError();base=p.AllocateMemory(200000,lldb.ePermissionsReadable|lldb.ePermissionsWritable,e);assert e.Success()
 write(base,b'\0'*200000)
 mem=base;signals=base+256;inputs=base+512;sample=base+768;module=base+1024;ensemble=base+2048;event=base+4096;out=base+8192
 write(mem+0x50,struct.pack('<QQ',inputs,signals));write(mem+0x18,struct.pack('<Q',module));write(module+0x28,struct.pack('<Q',ensemble));write(inputs,struct.pack('<Q',sample));write(event+8,struct.pack('<Q',mem))
 def eventfn(addr,value):expr(f'(((void(*)(void*,float,unsigned))0x{addr+slide:x})((void*)0x{event:x},(float){value},0),1)')
 data={'source_sha256':hashlib.sha256(open(APP,'rb').read()).hexdigest(),'sample_rate':48000,'envelopes':[], 'filter_coefficients':[]}
 for pitch in [36,60,84]:
  write(signals,b'\0'*128);write(mem+0x40,struct.pack('<f',1/48000))
  for addr,val in [(0x1006d1a00,40+(pitch-60)*-.5),(0x1006d1b60,34+(pitch-60)*-.5),(0x1006d1db8,40+(pitch-60)*-.5),(0x1006d1f5c,.3)]:eventfn(addr,val)
  coeff=list(struct.unpack('<10f',read(signals,40)))
  code=f'''([](){{auto gate=(void(*)(void*,float,unsigned))0x{0x1006d16d4+slide:x};auto audio=(void(*)(void*))0x{0x10029148c+slide:x};for(int i=0;i<20000;++i){{if(i==0)gate((void*)0x{event:x},.8f,0);if(i==4800)gate((void*)0x{event:x},0.f,0);if(i==9600)gate((void*)0x{event:x},.4f,0);if(i==14400)gate((void*)0x{event:x},0.f,0);audio((void*)0x{mem:x});((float*)0x{out:x})[i]=*(float*)0x{signals:x};}}return 1;}})()'''
  expr(code);values=list(struct.unpack('<20000f',read(out,80000)));data['envelopes'].append(dict(pitch=pitch,coefficients=coeff,values=values))
 for pitch in [0,36,60,100,120]:
  for resonance in [0,.15,.666,1]:
   write(signals,b'\0'*128);write(mem+0x40,struct.pack('<f',2/48000))
   eventfn(0x1006d0fa4,resonance);eventfn(0x1006d0db8,pitch)
   floats=struct.unpack('<9f',read(signals,36));data['filter_coefficients'].append(dict(pitch=pitch,resonance=resonance,f=floats[0],d=floats[4]))
 open('/tmp/steam-envelope-oracle.json','w').write(json.dumps(data))
 result.PutCString('Captured 60,000 original ADSR samples and 20 Multi 2-Pole coefficient pairs.')
def __lldb_init_module(debugger,internal):debugger.HandleCommand('command script add -f capture_primary_reference.dump steam-env-oracle')
