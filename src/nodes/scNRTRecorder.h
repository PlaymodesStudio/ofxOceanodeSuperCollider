//
//  scNRTRecorder.h
//  ofxOceanodeSuperCollider
//

#ifndef scNRTRecorder_h
#define scNRTRecorder_h

#include "ofxOceanodeSuperColliderConfig.h"

#if OFXOCEANODESC_HAS_TIMELINE

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeSuperColliderController.h"

// Starts a frame-stepped SuperCollider NRT capture while Record is true.
//
// Record is an ordinary parameter, so it has both an inlet and an outlet: it
// can be driven by an external modulator and still drive another node, for
// example the original Texture Recorder's Record input, which keeps the audio
// render and the frame sequence in step. It is kept out of presets so that
// recalling one never starts a render on its own.
class scNRTRecorder : public ofxOceanodeNodeModel {
public:
    explicit scNRTRecorder(ofxOceanodeSuperColliderController* controller)
    : ofxOceanodeNodeModel("SC NRT Recorder"), controller(controller) {
        addParameter(record.set("Record", false), ofxOceanodeParameterFlags_DisableSavePreset);
        addParameter(server.set("Server", 0, 0, 127));
        addParameter(channels.set("Channels", 2, 1, 128));
        addParameter(filename.set("Filename", "Supercollider/NRT/recording.wav"));

        recordListener = record.newListener([this](bool& value){ onRecordChanged(value); });
    }

    ~scNRTRecorder() override {
        if (controller != nullptr && record.get()) {
            controller->endNRTRecording(true);
        }
    }

private:
    void onRecordChanged(bool& value){
        if(suppressRecordListener || controller == nullptr) return;

        if(value){
            if(controller->beginNRTRecording(server.get(), channels.get(), filename.get())) return;
            suppressRecordListener = true;
            record = false;
            suppressRecordListener = false;
            return;
        }

        controller->endNRTRecording(false);
    }

    ofxOceanodeSuperColliderController* controller = nullptr;
    ofParameter<bool> record;
    ofParameter<int> server;
    ofParameter<int> channels;
    ofParameter<std::string> filename;
    ofEventListener recordListener;
    bool suppressRecordListener = false;
};

#endif // OFXOCEANODESC_HAS_TIMELINE

#endif /* scNRTRecorder_h */
