//
//  ofxOceanodeSuperColliderController.cpp
//
//  Created by Eduard Frigola Bagué on 23/12/2022.
//

#include "ofxOceanodeSuperColliderController.h"
#include "scStart.h"
#include "ofxSCServer.h"
#include "imgui.h"
#include "serverManager.h"

ofxOceanodeSuperColliderController::ofxOceanodeSuperColliderController() : ofxOceanodeBaseController("SuperCollider"){
    volume = 1;
    delay = 0;
    stereomix = true;
    stereomixSize = 4;
    audioDevice = 0;
    selectedAudioDeviceName = "nil";
	mute = false;
	
	reloadAudioDevices();
}

void ofxOceanodeSuperColliderController::createServers(){
    ofDirectory dir;
    dir.open(ofToDataPath("Supercollider/Config/Server"));
    if(dir.exists()){
        dir.sort();
        for(auto f : dir.getFiles()){
            outputServers.push_back(new serverManager());
            loadConfig(f.getAbsolutePath(), outputServers.back()->preferences);
            outputServers.back()->setAudioDevices(audioDeviceNames);
            if(outputServers.size() == 1){
                selectedAudioDeviceName = outputServers.back()->preferences.deviceName;
                syncAudioDeviceSelection();
            }
        }
    }else{
        dir.createDirectory("Supercollider/Config/Server");
		ofSystemAlertDialog("Supercollider server dir not found!\nCheck ./data/SuperCollider/Config ");
    }
    //If no config found, create just one server with default settings
    if(outputServers.size() == 0){
        outputServers.push_back(new serverManager());
        outputServers.back()->setAudioDevices(audioDeviceNames);
    }
    applyAudioDeviceToServers(false);
}

void ofxOceanodeSuperColliderController::setup(){
	
	// load preferences from JSON
	ofJson json = ofLoadJson("Supercollider/Config/Controller/ControllerPreferences.json");
	if(!json.empty())
	{
		delay = json["delay"];
		volume = json["volume"];
		stereomixSize = json["stereomixsize"];
		stereomix = json["stereomix"];
        selectedAudioDeviceName = json.value<std::string>("deviceName", selectedAudioDeviceName);
        syncAudioDeviceSelection();
	}
	// apply preferences to output servers
    applyAudioDeviceToServers(false);
    for(auto s : outputServers)
	{
		s->setup();
		s->setDelay(delay);
		s->setVolume(volume);
		s->setStereoMix(stereomix);
		s->setStereoMixSize(stereomixSize);
	}
	
}

void ofxOceanodeSuperColliderController::draw(){
    if(ImGui::Button("Boot Servers")){
        for(auto s : outputServers) s->boot();
    }
    
    ImGui::SameLine();
    if(ImGui::Button("Kill Server")){
        for(auto s : outputServers) s->kill();
    }
    
    ImGui::SameLine();
    
    if(ImGui::Button("Load Defs")){
        for(auto s : outputServers) s->loadDefs();
    }
    
    ImGui::Separator();

    auto vector_getter = [](void* vec, int idx, const char** out_text)
    {
        auto& vector = *static_cast<std::vector<std::string>*>(vec);
        if (idx < 0 || idx >= static_cast<int>(vector.size())) { return false; }
        *out_text = vector.at(idx).c_str();
        return true;
    };

    if(ImGui::Button("Refresh Audio Devices")){
        reloadAudioDevices();
        syncAudioDeviceSelection();
        for(auto s : outputServers) s->setAudioDevices(audioDeviceNames);
    }

    ImGui::SameLine();

    if(ImGui::Combo("Audio Device", &audioDevice, vector_getter, static_cast<void*>(&audioDeviceNames), audioDeviceNames.size())){
        const std::string newAudioDeviceName = getAudioDeviceNameFromSelection();
        if(newAudioDeviceName != selectedAudioDeviceName){
            ofLogNotice("ofxOceanodeSuperColliderController") << "Switching audio output device to "
                << (newAudioDeviceName == "nil" ? "Default" : newAudioDeviceName);
            selectedAudioDeviceName = newAudioDeviceName;
            applyAudioDeviceToServers(true);
        }
    }

    ImGui::Separator();
    
    if(ImGui::SliderFloat("Master Volume", &volume, 0, 1)){
        for(auto &n : outputServers){
            n->setVolume(volume);
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Master Mute", &mute)){
        for(auto &n : outputServers){
            if(mute) n->setVolume(0);
            else n->setVolume(volume);
        }
    }
    
    if(ImGui::SliderInt("Master Delay", &delay, 0, 5000)){
        for(auto &n : outputServers){
            n->setDelay(delay);
        }
    }
    
    if(ImGui::Checkbox("StereoMix", &stereomix)){
        for(auto &n : outputServers){
            n->setStereoMix(stereomix);
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::SliderInt("StereoMix Size", &stereomixSize, 2, 100)){
        for(auto &n : outputServers){
            n->setStereoMixSize(stereomixSize);
        }
    }
	if(ImGui::Button("[Save SC Controller Settings]"))
	{
		saveControllerConfig("Supercollider/Config/Controller/ControllerPreferences.json");
	}

    ImGui::Separator();
    
    for(int i = 0; i < outputServers.size(); i++){
        if(ImGui::TreeNode(("Server " + ofToString(i)).c_str())){
            outputServers[i]->draw();
            ImGui::TreePop();
        }
    }
    
    if(ImGui::Button("[Save Server Settings]")){
        for(int i = 0; i < outputServers.size(); i++){
            saveConfig("Supercollider/Config/Server/ServerPreferences_" + ofToString(i) + ".json", outputServers[i]->preferences);
        }
    }
}

void ofxOceanodeSuperColliderController::killServers(){
    for(auto s : outputServers) s->kill();
}


void ofxOceanodeSuperColliderController::reloadAudioDevices(){
    auto devices = ofSoundStreamListDevices();
    
    audioDeviceNames = {"Default"};
    audioDeviceSuperColliderNames = {"nil"};
    audioDeviceInputChannels = {0};
    audioDeviceOutputChannels = {0};
    for(auto &d : devices){
        audioDeviceNames.push_back(d.name);
        audioDeviceSuperColliderNames.push_back(getSuperColliderDeviceName(d.name));
        audioDeviceInputChannels.push_back((int)d.inputChannels);
        audioDeviceOutputChannels.push_back((int)d.outputChannels);
    }
}


void ofxOceanodeSuperColliderController::saveConfig(std::string filepath, scPreferences prefs){
    ofJson json;
    json["local"] = prefs.local;
    json["loadOnPreset"] = prefs.loadOnPreset;
    json["udpPort"] = prefs.udpPort;
    json["bindAddress"] = prefs.bindAddress;
    json["numControlBusChannels"] = prefs.numControlBusChannels;
    json["numAudioBusChannels"] = prefs.numAudioBusChannels;
    json["numInputBusChannels"] = prefs.numInputBusChannels;
    json["numOutputBusChannels"] = prefs.numOutputBusChannels;
    json["blockSize"] = prefs.blockSize;
    json["hardwareBufferSize"] = prefs.hardwareBufferSize;
    json["hardwareSampleRate"] = prefs.hardwareSampleRate;
    json["numBuffers"] = prefs.numBuffers;
    json["maxNodes"] = prefs.maxNodes;
    json["maxSynthDefs"] = prefs.maxSynthDefs;
    json["memSize"] = prefs.memSize;
    json["numWireBufs"] = prefs.numWireBufs;
    json["numRGens"] = prefs.numRGens;
    json["maxLogins"] = prefs.maxLogins;
    json["safetyClipThreshold"] = prefs.safetyClipThreshold;
    json["deviceName"] = prefs.deviceName;
    json["verbosity"] = prefs.verbosity;
    json["ugensPlugins"] = prefs.ugensPlugins;
    
    ofSavePrettyJson(filepath, json);
}

void ofxOceanodeSuperColliderController::saveControllerConfig(std::string filepath){
	ofJson json;

	json["delay"] = delay;
	json["volume"] = volume;
	json["stereomixsize"] = stereomixSize;
	json["stereomix"] = stereomix;
    json["deviceName"] = selectedAudioDeviceName;

	ofSavePrettyJson(filepath, json);
}


void ofxOceanodeSuperColliderController::loadConfig(std::string filepath, scPreferences &prefs){
    ofJson json = ofLoadJson(filepath);
    if(!json.empty()){
        prefs.local = json["local"];
        prefs.loadOnPreset = json.value<bool>("loadOnPreset", false);
        prefs.udpPort = json["udpPort"];
        prefs.bindAddress = json["bindAddress"];
        prefs.numControlBusChannels = json["numControlBusChannels"];
        prefs.numAudioBusChannels = json["numAudioBusChannels"];
        prefs.numInputBusChannels = json["numInputBusChannels"];
        prefs.numOutputBusChannels = json["numOutputBusChannels"];
        prefs.blockSize = json["blockSize"];
        prefs.hardwareBufferSize = json["hardwareBufferSize"];
        prefs.hardwareSampleRate = json["hardwareSampleRate"];
        prefs.numBuffers = json["numBuffers"];
        prefs.maxNodes = json["maxNodes"];
        prefs.maxSynthDefs = json["maxSynthDefs"];
        prefs.memSize = json["memSize"];
        prefs.numWireBufs = json["numWireBufs"];
        prefs.numRGens = json["numRGens"];
        prefs.maxLogins = json["maxLogins"];
        prefs.safetyClipThreshold = json["safetyClipThreshold"];
        prefs.deviceName = json.value<std::string>("deviceName", "nil");
        prefs.verbosity = json["verbosity"];
        prefs.ugensPlugins = json["ugensPlugins"];
    }
}

void ofxOceanodeSuperColliderController::syncAudioDeviceSelection(){
    if(selectedAudioDeviceName.empty() || selectedAudioDeviceName == "Default"){
        selectedAudioDeviceName = "nil";
    }

    audioDevice = 0;
    const std::string displayName = selectedAudioDeviceName == "nil" ? "Default" : selectedAudioDeviceName;
    for(int i = 0; i < (int)audioDeviceNames.size(); i++){
        if(audioDeviceNames[i] == displayName ||
           (i < (int)audioDeviceSuperColliderNames.size() && audioDeviceSuperColliderNames[i] == selectedAudioDeviceName)){
            audioDevice = i;
            return;
        }
    }

    if(selectedAudioDeviceName != "nil"){
        audioDeviceNames.push_back(selectedAudioDeviceName);
        audioDeviceSuperColliderNames.push_back(selectedAudioDeviceName);
        audioDeviceInputChannels.push_back(0);
        audioDeviceOutputChannels.push_back(0);
        audioDevice = (int)audioDeviceNames.size() - 1;
    }
}

std::string ofxOceanodeSuperColliderController::getSuperColliderDeviceName(const std::string& deviceName) const{
    const auto separator = deviceName.find(": ");
    if(separator == std::string::npos){
        return deviceName;
    }
    return deviceName.substr(separator + 2);
}

std::string ofxOceanodeSuperColliderController::getAudioDeviceNameFromSelection() const{
    if(audioDevice <= 0 || audioDevice >= (int)audioDeviceSuperColliderNames.size()){
        return "nil";
    }
    return audioDeviceSuperColliderNames[audioDevice];
}

void ofxOceanodeSuperColliderController::applyAudioDeviceToServers(bool restartServers){
    const std::string deviceName = getAudioDeviceNameFromSelection();
    const int inputChannels = (audioDevice >= 0 && audioDevice < (int)audioDeviceInputChannels.size()) ? audioDeviceInputChannels[audioDevice] : 0;
    const int outputChannels = (audioDevice >= 0 && audioDevice < (int)audioDeviceOutputChannels.size()) ? audioDeviceOutputChannels[audioDevice] : 0;
    selectedAudioDeviceName = deviceName;

    for(auto s : outputServers){
        s->setAudioDevices(audioDeviceNames);
        s->setAudioDeviceName(deviceName, inputChannels, outputChannels);
    }

    if(restartServers){
        for(auto s : outputServers) s->prepareForRestart();
        for(auto s : outputServers) s->boot();
    }
}
