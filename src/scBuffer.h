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
    scBuffer(ofxSCServer *_server = ofxSCServer::local()) : ofxOceanodeNodeModel("SC Buffer"){
        server = _server;
    };
    ~scBuffer(){
        for(auto buffer : buffers){
            buffer->free();
            delete buffer;
        }
    }
    
    void setup(){
        addParameter(path.set("Path", ""));
        addParameter(openFileDialog.set("Open"));
        auto orderParamRef = addParameter(bufferOrder.set("Order", {0}, {0}, {INT_MAX}));
        addOutputParameter(numSamples.set("Num S", 0, 0, INT_MAX));
        addOutputParameter(buffersParam.set("Buffer", {}));
        
        addInspectorParameter(filenamesList.set([this](){
            for(int i = 0; i < wavFiles.size(); i++){
                ImGui::Text((ofToString(i) + "-" + ofFilePath::getBaseName(wavFiles[i])).c_str());
            }
        }));
        
        listener = bufferOrder.newListener([this](vector<int> &vi){
            if(vi.size() == 0){
                vector<int> iotaOrder(wavFiles.size());
                std::iota(iotaOrder.begin(), iotaOrder.end(), 0);
                doNotReorder = true;
                bufferOrder = iotaOrder;
                doNotReorder = false;
            }
            else if(!doNotReorder && buffers.size() == wavFiles.size() && buffers.size() != 0){
                if(vi.size() == 1){
                    int index = vi[0];
                    if(wavFiles.size() > 1 && index < wavFiles.size()){
                        buffersParam = vector<ofxSCBuffer*>(1, buffers[index]);
                    }
                }else{
                    vector<ofxSCBuffer*> reorderedBuffers(vi.size());
                    for(int i = 0; i < vi.size(); i++){
                        int index = vi[i];
                        reorderedBuffers[i] = buffers[ofWrap(index, 0, buffers.size())];
//                        if(wavFiles.size() > 1 && index < wavFiles.size()){
//                            reorderedBuffers[i] = buffers[index];
//                        }
                    }
                    buffersParam = reorderedBuffers;
                }
            }
            else{
                vector<int> iotaOrder(wavFiles.size());
                std::iota(iotaOrder.begin(), iotaOrder.end(), 0);
                bufferOrder = iotaOrder;
            }
        });
        
        listener2 = openFileDialog.newListener([this]{
            auto result = ofSystemLoadDialog("Select sample file or folder", true, ofToDataPath("Samples", true));
            if(result.bSuccess){
                string pathWidthData = result.getPath();
                ofStringReplace(pathWidthData, ofToDataPath("Samples/", true), "");
                path = pathWidthData;
            }else{
                path = "";
            }
        });
        
        
        listener3 = path.newListener([this, orderParamRef](string &s){
            for(auto buffer : buffers){
                buffer->free();
                delete buffer;
            }
            buffers.clear();
            
            string pathWithData = ofToDataPath("Samples/" + s, true);
            wavFiles.clear();
            if(ofFilePath::getFileExt(pathWithData) == ""){ //is a folder
                ofDirectory dir;
                dir.open(pathWithData);
                if(dir.exists()){
                    dir.sort();
                    for(auto f = dir.begin(); f < dir.end(); f++){
                        if(f->getExtension() == "wav"){
                            wavFiles.push_back(f->getAbsolutePath());
                        }
                    }
//                    if(bufferOrder->size() != wavFiles.size()){
//                        doNotReorder = true;
//                        orderParamRef->removeAllConnections();
//                        doNotReorder = false;
//                    }
                    vector<int> iotaOrder(wavFiles.size());
                    std::iota(iotaOrder.begin(), iotaOrder.end(), 0);
                    doNotReorder = true;
                    bufferOrder = iotaOrder;
                    doNotReorder = false;
                }
            }else if(ofFilePath::getFileExt(pathWithData) == "wav"){ //is a file
                wavFiles.push_back(pathWithData);
                doNotReorder = true;
                bufferOrder = {0};
                doNotReorder = false;
            }else{
                doNotReorder = true;
                bufferOrder = {0};
                doNotReorder = false;
            }
            buffers.resize(wavFiles.size(), nullptr);
            for(int i = 0; i < wavFiles.size(); i++){
                buffers[i] = new ofxSCBuffer(0, 0, server);
                buffers[i]->readChannel(wavFiles[i], {0});
            }
            buffersParam = buffers;
            numSamples = buffers.size();
        });
    }
    
private:
    ofEventListener listener;
    ofEventListener listener2;
    ofEventListener listener3;
    

    
    vector<ofxSCBuffer*> buffers;
    ofParameter<vector<ofxSCBuffer*>> buffersParam;
    
    ofParameter<string> sampleName;
    ofParameter<string> path;
    ofParameter<void> openFileDialog;
    ofParameter<vector<int>> bufferOrder;
    ofParameter<int> numSamples;
    
    customGuiRegion filenamesList;
    
    ofxSCServer *server;
    vector<string> wavFiles;
    
    bool doNotReorder;
};

#endif /* scBuffer_h */
