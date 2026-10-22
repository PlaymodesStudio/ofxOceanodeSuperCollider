#include "SC_PlugIn.h"
#include "CoreDSP.h"
static InterfaceTable* ft;
static inline float get(Unit* unit,int p,int i) {return INRATE(p)==calc_FullRate?IN(p)[i]:IN0(p);}
struct CompensatedSVF:Unit {SVFState filter;};
static void SVF_next(CompensatedSVF* unit,int n) {
 for(int i=0;i<n;++i) OUT(0)[i]=unit->filter.tick(get(unit,0,i),get(unit,1,i),get(unit,2,i),int(get(unit,3,i)),SAMPLERATE);
}
static void CompensatedSVF_Ctor(CompensatedSVF* unit) {new(&unit->filter)SVFState();SETCALC(SVF_next);OUT0(0)=0;}
struct FilteredFeedbackDelay:Unit {float* buffer;int size,pos;float filtered;SVFState filter;};
static void Delay_next(FilteredFeedbackDelay* unit,int n) {
 if(!unit->buffer){ClearUnitOutputs(unit,n);return;}
 for(int i=0;i<n;++i) {
  float x=get(unit,0,i);
  // Feedback is the prior filtered sample: no scsynth block delay.
  unit->buffer[unit->pos]=x+get(unit,2,i)*unit->filtered;
  float delay=std::clamp(get(unit,1,i)*float(SAMPLERATE*.001),1.f,float(unit->size-4));
  int k=int(delay);float f=delay-k;
  auto tap=[&](int d){return unit->buffer[(unit->pos-d+unit->size)%unit->size];};
  float y=coreCubic(f,tap(std::max(0,k-1)),tap(k),tap(k+1),tap(k+2));
  unit->filtered=unit->filter.tick(y,get(unit,3,i),get(unit,4,i),int(get(unit,5,i)),SAMPLERATE);
  OUT(0)[i]=coreFade(x,y,get(unit,6,i));
  unit->pos=(unit->pos+1)%unit->size;
 }
}
static void FilteredFeedbackDelay_Ctor(FilteredFeedbackDelay* unit) {
 unit->size=int(SAMPLERATE*2)+8;unit->pos=0;unit->filtered=0;new(&unit->filter)SVFState();
 unit->buffer=(float*)RTAlloc(unit->mWorld,unit->size*sizeof(float));
 if(unit->buffer)std::fill(unit->buffer,unit->buffer+unit->size,0.f);
 SETCALC(Delay_next);OUT0(0)=0;
}
static void FilteredFeedbackDelay_Dtor(FilteredFeedbackDelay* unit) {if(unit->buffer)RTFree(unit->mWorld,unit->buffer);}
struct EventADSRKernel:Unit {float env,peak,target,residual,prev;int stage;};
static void Env_next(EventADSRKernel* unit,int n) {
 for(int i=0;i<n;++i) {
  float trig=get(unit,0,i),gate=get(unit,1,i),sus=std::clamp(get(unit,4,i),0.f,1.f);
  if(trig>0&&unit->prev<=0) {
   unit->peak=std::max(unit->env,get(unit,6,i));unit->target=unit->peak*sus;
   unit->residual=unit->peak-unit->target;unit->stage=unit->env>get(unit,6,i)?1:3;
  }
  unit->prev=trig;
  if(gate<=0)unit->stage=0;
  if(unit->stage==0)unit->env=(unit->env+1e-12f)*std::max(0.f,1.f-(1000.f/(get(unit,5,i)*float(SAMPLERATE))));
  if(unit->stage==1) {unit->residual=(unit->residual+1e-12f)*std::max(0.f,1.f-(1000.f/(get(unit,3,i)*float(SAMPLERATE))));unit->env=unit->target+unit->residual;}
  if(unit->stage==3) {unit->env+=(1000.f/(get(unit,2,i)*float(SAMPLERATE)))*unit->peak;if(unit->env>unit->peak){unit->env=unit->peak;unit->stage=1;}}
  OUT(0)[i]=unit->env;
 }
}
static void EventADSRKernel_Ctor(EventADSRKernel* unit) {
 unit->env=unit->peak=unit->target=unit->residual=unit->prev=0;unit->stage=0;SETCALC(Env_next);OUT0(0)=0;
}
struct AsymmetricFollower:Unit {float state;};
static void Follow_next(AsymmetricFollower* unit,int n) {
 for(int i=0;i<n;++i) {
  float x=get(unit,0,i),t=get(unit,x>unit->state?1:2,i);
  unit->state+=(x-unit->state)*(.693147182f/std::max(1.f,t*float(SAMPLERATE)));
  OUT(0)[i]=unit->state;
 }
}
static void AsymmetricFollower_Ctor(AsymmetricFollower* unit) {unit->state=0;SETCALC(Follow_next);OUT0(0)=0;}
// Unsigned arithmetic makes the 32-bit recurrence wrap explicitly on both CPUs.
struct CongruentialRandomKernel:Unit {uint32_t state;float previous,resetPrevious,seedPrevious,value;};
static uint32_t randomSeed(float x) {return uint32_t(int32_t(std::clamp(x,-2147483520.f,2147483520.f)));}
static void Random_next(CongruentialRandomKernel* unit,int n) {
 for(int i=0;i<n;++i) {
  float clock=get(unit,0,i),reset=get(unit,2,i),seed=get(unit,1,i);int mode=int(get(unit,3,i));
  if(mode==2 || seed!=unit->seedPrevious || (reset>0 && unit->resetPrevious<=0))unit->state=randomSeed(seed);
  if(mode!=0 || (clock>0 && unit->previous<=0)) {
   unit->state=unit->state*uint32_t(1103515245)+uint32_t(12345);
   int64_t signedValue=unit->state<0x80000000u?int64_t(unit->state):int64_t(unit->state)-0x100000000LL;
   unit->value=float(signedValue)*4.65670003e-10f;
  }
  unit->previous=clock;unit->resetPrevious=reset;unit->seedPrevious=seed;OUT(0)[i]=unit->value;
 }
}
static void CongruentialRandomKernel_Ctor(CongruentialRandomKernel* unit) {
 unit->state=randomSeed(IN0(1));unit->seedPrevious=IN0(1);unit->previous=unit->resetPrevious=unit->value=0;
 SETCALC(Random_next);OUT0(0)=0;
}
PluginLoad(OceanodePrimitives) {
 ft=inTable;
 DefineSimpleUnit(CompensatedSVF);
 DefineSimpleUnit(AsymmetricFollower);
 DefineSimpleUnit(CongruentialRandomKernel);
 DefineDtorUnit(FilteredFeedbackDelay);
 DefineSimpleUnit(EventADSRKernel);
}
