#include "ofApp.h"
#include "ofxOceanodeSuperCollider.h"

//--------------------------------------------------------------
void ofApp::setup(){
    ofSystem("killall scsynth");
    ofSetFrameRate(60);

    ofDisableArbTex();
    
    oceanode.setup();
    
    ofxOceanodeSuperCollider::registerCollection(oceanode);
    ofxOceanodeSuperCollider::setup(oceanode);
}

//--------------------------------------------------------------
void ofApp::update(){

    oceanode.update();
}

//--------------------------------------------------------------
void ofApp::draw(){
    oceanode.draw();
}

//--------------------------------------------------------------
void ofApp::exit(){
    ofxOceanodeSuperCollider::kill(oceanode);
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key){

}

//--------------------------------------------------------------
void ofApp::keyReleased(int key){

}

//--------------------------------------------------------------
void ofApp::mouseMoved(int x, int y ){

}

//--------------------------------------------------------------
void ofApp::mouseDragged(int x, int y, int button){

}

//--------------------------------------------------------------
void ofApp::mousePressed(int x, int y, int button){

}

//--------------------------------------------------------------
void ofApp::mouseReleased(int x, int y, int button){

}

//--------------------------------------------------------------
void ofApp::mouseScrolled(int x, int y, float scrollX, float scrollY){

}

//--------------------------------------------------------------
void ofApp::mouseEntered(int x, int y){

}

//--------------------------------------------------------------
void ofApp::mouseExited(int x, int y){

}

//--------------------------------------------------------------
void ofApp::windowResized(int w, int h){

}

//--------------------------------------------------------------
void ofApp::gotMessage(ofMessage msg){

}

//--------------------------------------------------------------
void ofApp::dragEvent(ofDragInfo dragInfo){ 

}
