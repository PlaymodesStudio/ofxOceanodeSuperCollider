#include "timing/scTransportSync.h"
#include <cassert>
#include <iostream>
#include <map>

using namespace scTransportSync;
constexpr uint64_t epoch=1000000000;

ofxOceanodeFrameTransportState frame(double seconds, float bpm=135, double loop=0) {
    ofxOceanodeFrameTransportState f;
    auto& c=f.current;
    c.isPlaying=true;c.bpm=bpm;c.steadyTimeUs=epoch+std::llround(seconds*1e6);
    const double beat=seconds*bpm/60;
    c.beatPosition=beat;
    if(loop>0){c.loopEnabled=true;c.loopEndBeat=loop;c.loopCount=uint64_t(beat/loop);c.beatPosition=positiveMod(beat,loop);}
    f.previous=c;f.previous.steadyTimeUs-=16667;
    return f;
}
void near(double a,double b,double eps=1e-6){assert(std::abs(a-b)<eps);}

int main(){
    // Ordinary corrections leave before the true 4-beat / 2-second deadline.
    for(float bpm : {30.f, 135.f, 240.f}) {
        Follower follower;
        int corrections=0;
        double lastDeadline=0;
        for(int i=0;i<=3600;++i){
            auto f=frame(i/60.,bpm);
            for(const auto& a:follower.poll(f)){
                if(a.discontinuity)continue;
                ++corrections;
                assert(a.offsetSeconds>0 && a.offsetSeconds<=.050001);
                assert(a.steadyTimeUs>f.current.steadyTimeUs+30000);
                near(a.beat,(a.steadyTimeUs-epoch)/1e6*bpm/60,2e-6);
                const double deadline=(a.steadyTimeUs-epoch)/1e6;
                near(deadline-lastDeadline,std::min(4.*60/bpm,2.),2e-6);
                lastDeadline=deadline;
                AnchorScope scope(a);assert(ofxSCServer::tag==a.oscTimetag);
                near(ofxSCServer::offset,a.offsetSeconds);
            }
        }
        assert(corrections>=29);
    }
    // Four-bar loop: one hard anchor per wrap, no backwards soft correction
    // just before the wrap, no second hard reset on the following frame.
    {
        Follower follower;StepClock clock;clock.setPatternLength(1);
        int wraps=0,hard=0;uint64_t last=0;
        for(int i=0;i<60*30;++i){
            auto f=frame(i/60.,135,16);
            for(const auto& a:follower.poll(f)){
                assert(a.steadyTimeUs>=last);last=a.steadyTimeUs;
                auto sa=clock.make(a.beat*48,a.discontinuity,a.playing);
                if(sa.hard)++hard;
                if(a.loopWrap){++wraps;assert(sa.hard && sa.fire);near(a.beat,0);}
                else if(!a.discontinuity){assert(!sa.hard);assert(a.beat>0 && a.beat<16);}
            }
        }
        assert(wraps==4 && hard==wraps+1);
    }
    // Predictable loops shorter than the correction period need only wraps.
    for(double length:{.25,1.,3.}){
        Follower follower;int wraps=0,soft=0;
        for(int i=0;i<600;++i)for(auto a:follower.poll(frame(i/60.,120,length))){
            wraps+=a.loopWrap;soft+=!a.discontinuity;
        }
        assert(wraps>1 && soft==0);
    }
    // A queued periodic correction cannot restore the old state after a stop,
    // seek, tempo change, reset, forced anchor or loop edit.
    for(int edit=0;edit<6;++edit){
        Follower follower;follower.poll(frame(0,120));
        auto pending=follower.poll(frame(1.96,120));assert(pending.size()==1);
        auto queued=pending.front();assert(queued.steadyTimeUs==epoch+2000000);
        auto f=frame(1.97,120);
        if(edit==0)f.current.isPlaying=false;
        if(edit==1){f.current.generation++;f.current.beatPosition=10;}
        if(edit==2)f.current.bpm=90;
        if(edit==3)follower.reset();
        if(edit==4)follower.requestAnchor();
        if(edit==5){f.current.loopEnabled=true;f.current.loopEndBeat=8;}
        auto out=follower.poll(f);assert(out.size()>=2);
        auto replacement=std::find_if(out.begin(),out.end(),[&](const Anchor& a){return a.oscTimetag==queued.oscTimetag;});
        assert(replacement!=out.end() && replacement->steadyTimeUs==queued.steadyTimeUs);
        assert(replacement->playing==f.current.isPlaying && replacement->bpm==f.current.bpm);
        near(replacement->beat,f.current.beatPosition+(f.current.isPlaying?.03*f.current.bpm/60:0));
        near(replacement->offsetSeconds,.03);
        AnchorScope scope(*replacement);assert(ofxSCServer::tag==queued.oscTimetag);
    }
    // Disable a loop after its wrap is queued: the old wrap deadline receives
    // the continuing position. It must not jump back to the obsolete start.
    {
        Follower follower;follower.poll(frame(0,120,4));
        auto out=follower.poll(frame(1.96,120,4));assert(out.size()==1 && out[0].loopWrap);
        auto queued=out[0];auto f=frame(1.97,120);out=follower.poll(f);
        auto r=std::find_if(out.begin(),out.end(),[&](const Anchor& a){return a.oscTimetag==queued.oscTimetag;});
        assert(r!=out.end() && !r->loopWrap);near(r->beat,4);
    }
    // A forced refresh must retain an unchanged queued wrap's first step.
    {
        Follower follower;follower.poll(frame(0,120,4));follower.poll(frame(1.96,120,4));
        follower.requestAnchor();auto out=follower.poll(frame(1.97,120,4));
        assert(out.back().loopWrap && out.back().discontinuity);near(out.back().beat,0);
    }
    // Resetting while a loop wrap is queued reuses that exact deadline once.
    {
        Follower follower;follower.poll(frame(0,120,4));
        auto q=follower.poll(frame(1.96,120,4)).front();
        follower.reset();auto out=follower.poll(frame(1.97,120,4));
        assert(std::count_if(out.begin(),out.end(),[](const Anchor& a){return a.loopWrap;})==1);
        assert(out.back().oscTimetag==q.oscTimetag);
    }
    // A genuinely missed deadline remains late (no timestamp clamping).
    {
        Follower follower;follower.poll(frame(0,120));
        auto out=follower.poll(frame(2.2,120));assert(out.size()==1);
        assert(out[0].steadyTimeUs==epoch+2000000 && out[0].offsetSeconds<0);
    }
    // NRT/frame-step cadence follows musical time even when wall time advances
    // much more slowly. Corrections retain the original current-frame offset.
    {
        Follower follower;int count=0;
        for(int i=0;i<600;++i){
            auto f=frame(i/60.,120);f.current.driverMode=TransportDriverMode::FrameStep;
            f.previous.driverMode=TransportDriverMode::FrameStep;
            f.previous.beatPosition=std::max(0.,f.current.beatPosition-1./30);
            f.current.steadyTimeUs=epoch+i*100;f.previous.steadyTimeUs=f.current.steadyTimeUs-100;
            for(auto a:follower.poll(f))if(!a.discontinuity){
                ++count;assert(a.offsetSeconds==0 && a.steadyTimeUs==f.current.steadyTimeUs);
            }
        }
        assert(count==4);
    }
    assert(ofxSCServer::tag==0 && ofxSCServer::offset==0);
    std::cout<<"PASS: periodic deadlines at 30/135/240 BPM, exact loop wraps, short loops, pending-anchor replacement for six edits, loop removal/refresh, genuine late deadlines and NRT cadence.\n";
}
