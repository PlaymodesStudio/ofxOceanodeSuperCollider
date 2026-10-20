#include "scTransportSync.h"
#include <iomanip>
#include <iostream>
#include <string>
using namespace scTransportSync;
int main(int argc,char** argv){
    std::string scenario=argc>1?argv[1]:"loop";
    const bool offline=argc>2;
    Follower follower;StepClock clock;clock.setPatternLength(1);
    ofxOceanodeFrameTransportState frame;
    double beat=0;uint64_t loopCount=0;int id=0,hard=0;
    const bool loopScenario=scenario=="loop"||scenario=="loop_tempo"||scenario=="loop_off";
    std::cout<<std::setprecision(15);
    for(int i=0;i<60*12;++i){
        double t=i/60.;
        float bpm=scenario=="loop"?135.f:120.f;
        bool playing=!(scenario=="stop" && t>=1.97 && t<2.97);
        if((scenario=="tempo"||scenario=="loop_tempo")&&t>=1.97)bpm=90;
        bool loop=loopScenario && !(scenario=="loop_off" && t>=1.97);
        double length=scenario=="loop"?16:4;
        frame.previous=frame.current;
        if(i>0 && frame.previous.isPlaying){
            beat+=frame.previous.bpm/60./60.;
            if(frame.previous.loopActive() && beat>=length-1e-9){beat-=length;++loopCount;}
        }
        auto& cur=frame.current;
        cur.driverMode=offline?TransportDriverMode::FrameStep:TransportDriverMode::RealTime;
        cur.steadyTimeUs=1000000000+std::llround(t*1e6);
        cur.beatPosition=beat;cur.bpm=bpm;cur.isPlaying=playing;
        cur.loopEnabled=loop;cur.loopStartBeat=0;cur.loopEndBeat=length;cur.loopCount=loopCount;
        for(const auto& a:follower.poll(frame)){
            const auto sa=clock.make(a.beat*48,a.discontinuity,a.playing);
            if(sa.hard)++hard;++id;
            // Same complete controls as EuclideanBox's production sender.
            std::cout<<(offline?t+a.offsetSeconds:(a.steadyTimeUs-1000000000)/1e6)<<' '<<a.bpm<<' '<<a.playing<<' '
                     <<sa.pos<<' '<<sa.fire<<' '<<hard<<' '<<id<<' '
                     <<positiveMod(std::floor(sa.pos),1536)<<'\n';
        }
    }
}
