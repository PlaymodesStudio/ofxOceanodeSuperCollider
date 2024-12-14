//
// scRave.h
//

#ifndef scRave_h
#define scRave_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scRave : public scNode {
public:
    scRave(vector<serverManager*> outputServers) : scNode("RAVE Loader") {
        serverList = outputServers;
        syncID = 1000;
    }
    
    void setup() override {
        addParameter(path.set("Path", ""));
        addParameter(openFileDialog.set("Load Model"));
        addParameter(modelLoaded.set("Model Loaded", false));
        
        listener = openFileDialog.newListener([this]{
            auto result = ofSystemLoadDialog("Select RAVE model file", false, ofToDataPath("Supercollider/RAVE_MODELS", true));
            if(result.bSuccess){
                string pathWithData = result.getPath();
                path = pathWithData;
                loadModel(result.getPath());
            }
        });
        
        listener2 = path.newListener([this](string &s){
            if(s != ""){
                string absolutePath;
                if(s[0] == '/'){  //Path is absolute
                    absolutePath = s;
                }
                else{ //Path is relative
                    absolutePath = ofToDataPath("Supercollider/RAVE_MODELS/" + s, true);
                }
                loadModel(absolutePath);
            }
        });
    }
    
private:
    void loadModel(const string& modelPath) {
            if(serverList.size() > 0 && serverList[0]->getServer()) {
                // Load new model
                ofxOscBundle bundle;
                
                // Add load command
                ofxOscMessage m;
                m.setAddress("/cmd");
                m.addStringArg("/nn_load");
                m.addIntArg(-1);
                m.addStringArg(modelPath);
                m.addStringArg(ofToDataPath("Supercollider/tmp/nn-sc-" + ofToString(syncID) + ".yaml", true));
                bundle.addMessage(m);
                
                // Add sync
                ofxOscMessage sync;
                sync.setAddress("/sync");
                sync.addIntArg(syncID++);
                bundle.addMessage(sync);
                
                // Send bundle
                serverList[0]->getServer()->sendBundle(bundle);
                
                // Wait a moment for load to complete
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                // Force reload of any synths using this model
                ofxOscMessage reload;
                reload.setAddress("/g_freeAll");
                reload.addIntArg(1);  // Free all synths in default group
                serverList[0]->getServer()->sendMsg(reload);
                
                modelLoaded = true;
                ofLog() << "Sent RAVE model load bundle for: " << modelPath;
            }
        }
    
    ofEventListener listener;
    ofEventListener listener2;
    
    ofParameter<string> path;
    ofParameter<void> openFileDialog;
    ofParameter<bool> modelLoaded;
    
    vector<serverManager*> serverList;
    int syncID;
};

#endif /* scRave_h */
