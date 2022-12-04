//
//  scSimpleSynth.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 24/01/2022.
//

#ifndef scSimpleSynth_h
#define scSimpleSynth_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSuperCollider.h"

class scSimpleSynth : public ofxOceanodeNodeModel{
public:
    scSimpleSynth(ofxSCServer *_server = ofxSCServer::local());
    ~scSimpleSynth();
    
    void setup();
    
private:
    template <typename T>
    auto getValueForPosition(const vector<T> &param, int index) -> T{
        if(param.size() == 1 || param.size() <= index){
            return param[0];
        }
        else{
            return param[index];
        }
    };
    
    void checkSize(vector<float> &v);
    
    ofEventListener l, la, lp, lpan;
    ofParameter<vector<float>> f;
    ofParameter<vector<float>> a;
    ofParameter<vector<float>> p;
    ofParameter<vector<float>> pan;
    
    vector<std::unique_ptr<ofxSCSynth>> synths;
    ofxSCServer *server;
};

#endif /* scSimpleSynth_h */
