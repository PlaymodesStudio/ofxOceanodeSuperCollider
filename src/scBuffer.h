//
//  scBuffer.h
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#ifndef scBuffer_h
#define scBuffer_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSuperCollider.h"

class scBuffer : public ofxOceanodeNodeModel {
public:
    scBuffer(vector<string> _wavs) : ofxOceanodeNodeModel("SC Buffer"){
        wavFiles = _wavs;
    };
    ~scBuffer(){
        
    }
    
    void setup(){
        addParameter(path.set("Path", ""));
        addParameter(openFileDialog.set("Open"));
        addOutputParameter(buffersParam.set("Buffer", {0, 1, 2}, {0}, {INT_MAX}));

        addInspectorParameter(filenamesList.set([this](){
            for(int i = 0; i < buffersParam->size(); i++){
                ImGui::Text((ofToString(i) + " // " + ofFilePath::getBaseName("Supercollider/Samples" + wavFiles[buffersParam->at(i)])).c_str());
            }
        }));

        listener2 = openFileDialog.newListener([this]{
            auto result = ofSystemLoadDialog("Select sample file or folder", true, ofToDataPath("Supercollider/Samples", true));
            if(result.bSuccess){
                string pathWidthData = result.getPath();
                ofStringReplace(pathWidthData, ofToDataPath("Supercollider/Samples/", true), "");
                path = pathWidthData;
            }else{
                path = "";
            }
        });


        listener3 = path.newListener([this](string &s){            
            vector<int> newIndices;

            if(s != ""){
                string pathWithData = ofToDataPath("Supercollider/Samples/" + s, true);
                if(ofFilePath::getFileExt(pathWithData) == ""){ //is a folder
                    ofDirectory dir;
                    dir.open(pathWithData);
                    if(dir.exists()){
                        dir.sort();
                        for(auto f = dir.begin(); f < dir.end(); f++){
                            if(f->getExtension() == "wav"){
                                string wavPath = f->getAbsolutePath();
                                ofStringReplace(wavPath, ofToDataPath("Supercollider/Samples", true), "");
                                int pos = std::find(wavFiles.begin(), wavFiles.end(), wavPath) - wavFiles.begin();
                                newIndices.push_back(pos);
                            }
                        }
                    }
                }else if(ofFilePath::getFileExt(pathWithData) == "wav"){ //is a file
                    string wavPath = pathWithData;
                    ofStringReplace(wavPath, ofToDataPath("Supercollider/Samples", true), "");
                    int pos = std::find(wavFiles.begin(), wavFiles.end(), wavPath) - wavFiles.begin();
                    newIndices.push_back(pos);
                }
                buffersParam = newIndices;
            }
        });
    }
    
private:
    ofEventListener listener;
    ofEventListener listener2;
    ofEventListener listener3;
    ofParameter<vector<int>> buffersParam;
    
    ofParameter<string> path;
    ofParameter<void> openFileDialog;
    
    customGuiRegion filenamesList;
    
    vector<string> wavFiles;
};

#endif /* scBuffer_h */
