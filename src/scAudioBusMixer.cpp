#include "scAudioBusMixer.h"
#include <filesystem>

std::string scAudioBusMixerSynthdefFolder() {
    auto folder=std::filesystem::path(ofToDataPath(std::string(SYNTHDEF_DIRECTORY)+"/Defaults/AudioRouting",true));
    if(!std::filesystem::exists(folder/"audioBusMixer.scsyndef"))
        folder=std::filesystem::path(__FILE__).parent_path().parent_path()/"data/Supercollider/Synthdefs/Defaults/AudioRouting";
    return folder.string();
}

void registerSCAudioInputs(const std::vector<serverManager*>& servers) {
    for(auto* manager:servers) if(manager) manager->requireSynthdefFolder(scAudioBusMixerSynthdefFolder());
    // ScBus is the only type opting into multiple incoming cables. The
    // ordinary graph still sees one port: a private chain of unity sums.
    ofxOceanodeAbstractParameter::registerInputCombiner(typeid(nodePort).name(),
        [servers](ofxOceanodeAbstractParameter& sink,
                  const std::vector<ofxOceanodeAbstractParameter*>& sources,
                  std::shared_ptr<void>& state) {
            using Bridges=std::vector<std::shared_ptr<scAudioBusMixer>>;
            if(!state) state=std::make_shared<Bridges>();
            auto bridges=std::static_pointer_cast<Bridges>(state);
            std::vector<nodePort> ports;
            for(auto* source:sources) {
                const auto port=source->cast<nodePort>().getParameter().get();
                if(port.getNodeRef()) ports.push_back(port);
            }
            const size_t count=ports.empty()?0:ports.size()-1;
            bridges->resize(count);
            nodePort result=ports.empty()?nodePort():ports.front();
            for(size_t i=0;i<count;++i) {
                auto& bridge=(*bridges)[i];
                if(!bridge) bridge=std::make_shared<scAudioBusMixer>(servers,true);
                bridge->setSources(result,ports[i+1],MAX_NODE_CHANNELS,1,1);
                result=nodePort(0,bridge.get(),bridge);
            }
            sink.cast<nodePort>().getParameter()=result;
        });
}
