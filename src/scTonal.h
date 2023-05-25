//
//  scTonal.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 25/11/2022.
//

#ifndef scTonal_h
#define scTonal_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSuperCollider.h"

//class scTonal : public ofxOceanodeNodeModel{
//public:
//    scTonal() : ofxOCeanodeNodeModel("SC Tonal"){}
//    
//    void setup(){
//        
//    }
//    
//private:
//    template <typename T>
//    auto getValueForPosition(const vector<T> &param, int index) -> T{
//        if(param.size() == 1 || param.size() <= index){
//            return param[0];
//        }
//        else{
//            return param[index];
//        }
//    };
//};

class scPitch : public ofxOceanodeNodeModel{
public:
    scPitch() : ofxOceanodeNodeModel("SC Pitch"){}
    
    void setup(){
        ofFile file;
        file.open("Supercollider/Pitchclass/scales.txt");
        
        ofBuffer buffer = file.readToBuffer();
        vector<string> names;
        
        for(auto &l : buffer.getLines()){
            vector<int> scaleVals;
            vector<string> split = ofSplitString(ofSplitString(ofSplitString(l, ";")[0], ", ")[1], " ");
            for(int i = 1; i < split.size(); i++){
                scaleVals.push_back(ofToInt(split[i]));
            }
            scales.emplace_back(split[0], scaleVals);
            names.push_back(split[0]);
        }
        
        
        //addParameter(input.set("Input", {0}, {0}, {INT_MAX}));
        addParameterDropdown(mode, "Class", 0, names);
        addOutputParameter(modeSize.set("Size", 5, 0, INT_MAX));
        addOutputParameter(output.set("Output", {0}, {0}, {INT_MAX}));
        
        listeners.push(input.newListener([this](vector<int> &v){
//            tempOut = v;
//            for(int i = 0; i < tempOut.size(); i++){
//                int nota = scales[mode].second[tempOut[i] % scales[mode].second.size()];
//                int octava = tempOut[i] / scales[mode].second.size();
//                int scaleNumOctaves = floor(scales[mode].second.back() / 12) + 1;
//                tempOut[i] = nota + (octava * 12 * scaleNumOctaves);
//            }
//            output = tempOut;
        }));
        
        listeners.push(mode.newListener([this](int &m){
            modeSize = scales[mode].second.size();
            output = scales[mode].second;
        }));
    }
    
private:
    ofParameter<vector<int>> input;
    ofParameter<int>    mode;
    ofParameter<int>    modeSize;
    ofParameter<vector<int>> output;
    
    vector<pair<string, vector<int>>> scales;
    
    vector<int> tempOut;
    
    ofEventListeners listeners;
};

class scChord : public ofxOceanodeNodeModel{
public:
    scChord() : ofxOceanodeNodeModel("SC Chord"){}
    
    void setup(){
        ofFile file;
        file.open("Supercollider/Pitchclass/chords.txt");
        
        ofBuffer buffer = file.readToBuffer();
        vector<string> names;
        
        for(auto &l : buffer.getLines()){
            vector<int> chordsVals;
            vector<string> split = ofSplitString(ofSplitString(ofSplitString(l, ";")[0], ", ")[1], " ");
            for(int i = 1; i < split.size(); i++){
                chordsVals.push_back(ofToInt(split[i]));
            }
            chords.emplace_back(split[0], chordsVals);
            names.push_back(split[0]);
        }
        
        
        //addParameter(input.set("Input", {0}, {0}, {INT_MAX}));
        addParameterDropdown(mode, "Chord", 0, names);
        addParameter(fold.set("Fold", false));
        addOutputParameter(modeSize.set("Size", 5, 0, INT_MAX));
        addOutputParameter(output.set("Output", {0}, {0}, {INT_MAX}));
        
        listeners.push(input.newListener([this](vector<int> &v){
//            tempOut = v;
//            //TODO: Acords de mes de una octava
//            for(int i = 0; i < tempOut.size(); i++){
//                int nota = chords[mode].second[tempOut[i] % chords[mode].second.size()];
//                int octava = tempOut[i] / chords[mode].second.size();
//                int scaleNumOctaves = floor(chords[mode].second.back() / 12) + 1;
//                tempOut[i] = nota + (octava * 12 * scaleNumOctaves);
//            }
//            output = tempOut;
        }));
        
        listeners.push(mode.newListener([this](int &m){
            compute();
        }));
        
        listeners.push(fold.newListener([this](bool &b){
            compute();
        }));
    }
    
    void compute(){
        modeSize = chords[mode].second.size();
        if(fold){
            auto v = chords[mode].second;
            for(auto &i : v) i %= 12;
            std::sort(v.begin(), v.end());
            output = v;
        }else{
            output = chords[mode].second;
        }
    }
    
private:
    ofParameter<vector<int>> input;
    ofParameter<int>    mode;
    ofParameter<int>    modeSize;
    ofParameter<vector<int>> output;
    ofParameter<bool> fold;
    
    vector<pair<string, vector<int>>> chords;
    
    vector<int> tempOut;
    
    ofEventListeners listeners;
};

#endif /* scTonal_h */
