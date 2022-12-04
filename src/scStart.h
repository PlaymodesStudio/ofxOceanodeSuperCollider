//
//  scStart.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 24/01/2022.
//

#ifndef scStart_h
#define scStart_h

#include "ofThread.h"

// this needs to be threaded otherwise it blocks the main thread
class scStart : public ofThread{
  
public:
    scStart(){}
      
    void start(string scPath_ = ""){
        scPath = scPath_;
        startThread();
    }
      
    void threadedFunction(){
        string termcmd = scPath + " -u 57110 -z 512 -Z 512 -w 224 -a 1024 -n 16384 -m 65536 -i 2 -o 2 -R 0 -l 1";
          
        cout << ofSystem(termcmd);
    }
      
      
    ~scStart(){
        waitForThread(true);
    }
      
      
private:
    string scPath;

};

#endif /* scStart_h */
