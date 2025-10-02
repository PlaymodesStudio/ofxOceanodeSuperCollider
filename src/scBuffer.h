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

// WAVE file header format
struct WaveHeader {
    char riff[4];                // RIFF string
    uint32_t overall_size;      // overall size of file in bytes
    char wave[4];               // WAVE string
    char fmt_chunk_marker[4];   // fmt string with trailing null char
    uint32_t length_of_fmt;     // length of the format data
    uint16_t format_type;       // format type. 1-PCM, 3- IEEE float
    uint16_t channels;          // no.of channels
    uint32_t sample_rate;       // sampling rate (blocks per second)
    uint32_t byterate;          // SampleRate * NumChannels * BitsPerSample/8
    uint16_t block_align;       // NumChannels * BitsPerSample/8
    uint16_t bits_per_sample;   // bits per sample, 8- 8bits, 16- 16 bits etc
    char data_chunk_header[4];  // DATA string or FLLR string
    uint32_t data_size;         // NumSamples * NumChannels * BitsPerSample/8
};

class scBuffer : public ofxOceanodeNodeModel {
public:
    scBuffer(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC Buffer"){
        servers = outputServers;
    };
    
    ~scBuffer(){
        for(auto b : buffers){
            b->free();
            delete b;
        }
        buffers.clear();
    }
    
	void setup(){
		addParameter(path.set("Path", ""));
		addParameter(openFileDialog.set("Open"));
		addOutputParameter(buffersParam.set("Buffer", {0}, {0}, {INT_MAX}));
		addOutputParameter(durationsMs.set("Duration", {0}, {0}, {FLT_MAX}));
		addOutputParameter(sampleRates.set("Sample Rate", {0}, {0}, {FLT_MAX})); // New sample rate output
		
		addInspectorParameter(filenamesList.set([this](){
			int i = 0;
			for(auto &file : files){
				for(int j = 0; j < file.second; j++){
					ImGui::Text((ofToString(i) + " // " + file.first + " // ch" + ofToString(j+1) + " // " + ofToString(durations[i]) + "ms // " + ofToString(sampleRates.get()[i]) + "Hz").c_str());
					i++;
				}
			}
		}));
		
		addInspectorParameter(embedInProject.set("Embed Samples", false));
		
		listener2 = openFileDialog.newListener([this]{
			auto result = ofSystemLoadDialog("Select sample file or folder", false, ofToDataPath("Supercollider/Samples", true));
			if(result.bSuccess){
				string pathWidthData = result.getPath();
				ofStringReplace(pathWidthData, ofToDataPath("Supercollider/Samples/", true), "");
				path = pathWidthData;
			}
		});
		
		listener3 = path.newListener([this](string &s){
			// ---- reset outputs
			vector<int>   newIndices;
			vector<float> newDurations;
			vector<float> newSampleRates;
			durations.clear();
			sampleRates.set(vector<float>());
			
			if(s.empty()){
				buffersParam = newIndices;
				durationsMs  = newDurations;
				sampleRates  = newSampleRates;
				return;
			}
			
			// ---- free old buffers
			for(auto b : buffers){
				if(b != nullptr){
					b->free();
					delete b;
				}
			}
			buffers.clear();
			files.clear();
			
			// ---- sanitize incoming GUI path (strip leading "./")
			string sClean = s;
			if(sClean.size() >= 2 && sClean[0] == '.' && sClean[1] == '/'){
				sClean = sClean.substr(2);
			}
			
			// ---- resolve to absolute path (favor embedded copy if available)
			string absolutePath;
			
			// 1) If embed flag is on and we have a preset folder, try the embedded copy first (by filename)
			if(embedInProject && !currentPresetPath.empty()){
				string filename;
				if(!sClean.empty() && sClean[0] == '/'){
					ofFile f(sClean);
					filename = f.getFileName();
				}else{
					// sClean is data-relative; if it points to a folder/file, we still only want the filename
					ofFile f(ofToDataPath(sClean, true));
					filename = f.getFileName();
				}
				
				string embeddedPath = ofToDataPath(currentPresetPath + "/samples/" + filename, true);
				if(ofFile::doesFileExist(embeddedPath)){
					absolutePath = embeddedPath;
				}
			}
			
			// 2) If not resolved yet, choose based on path form
			if(absolutePath.empty()){
				if(!sClean.empty() && sClean[0] == '/'){
					// absolute path from GUI
					absolutePath = sClean;
				}else if(sClean.rfind("Presets/", 0) == 0){
					// data-relative embedded path like "Presets/Debug/446--OTT/samples/TANTAS-9.wav"
					absolutePath = ofToDataPath(sClean, true);
				}else{
					// default: treat as a path inside Supercollider/Samples/
					absolutePath = ofToDataPath("Supercollider/Samples/" + sClean, true);
				}
			}
			
			// ---- existence check
			if(!ofFile::doesFileExist(absolutePath)){
				ofLogError("scBuffer") << "Path does not exist: " << absolutePath;
				buffersParam = newIndices;
				durationsMs  = newDurations;
				sampleRates  = newSampleRates;
				return;
			}
			
			// ---- folder or file?
			if(ofFilePath::getFileExt(absolutePath) == ""){
				// -------- FOLDER: iterate *.wav
				ofDirectory dir;
				dir.open(absolutePath);
				if(dir.exists()){
					dir.allowExt("wav");
					dir.sort();
					for(auto f = dir.begin(); f < dir.end(); ++f){
						string wavPath = f->getAbsolutePath();
						int   numChannels = 0;
						float durationMs  = 0.0f;
						float srate       = 0.0f;
						
						getFileInfo(wavPath, numChannels, durationMs, srate);
						if(numChannels <= 0) continue;
						
						for(int ch = 0; ch < numChannels; ++ch){
							try{
								auto buf0 = new ofxSCBuffer(0, 0, servers[0]->getServer());
								buf0->readChannel(wavPath, {ch});
								buffers.push_back(buf0);
								
								newIndices.push_back(buf0->index);
								newDurations.push_back(durationMs);
								newSampleRates.push_back(srate);
								durations.push_back(durationMs);
								
								// mirror to the rest of servers
								for(size_t j = 1; j < servers.size(); ++j){
									auto bufn = new ofxSCBuffer(0, 0, servers[j]->getServer());
									bufn->readChannel(wavPath, {ch});
									buffers.push_back(bufn);
								}
							}catch(const std::exception& e){
								ofLogError("scBuffer") << "Exception reading file: " << e.what();
								continue;
							}
						}
						files[f->getFileName()] = numChannels;
					}
				}
			}else if(ofFilePath::getFileExt(absolutePath) == "wav"){
				// -------- SINGLE FILE
				of::filesystem::path wavPath = absolutePath;
				int   numChannels = 0;
				float durationMs  = 0.0f;
				float srate       = 0.0f;
				
				getFileInfo(wavPath, numChannels, durationMs, srate);
				if(numChannels > 0){
					for(int ch = 0; ch < numChannels; ++ch){
						try{
							auto buf0 = new ofxSCBuffer(0, 0, servers[0]->getServer());
							buf0->readChannel(wavPath, {ch});
							buffers.push_back(buf0);
							
							newIndices.push_back(buf0->index);
							newDurations.push_back(durationMs);
							newSampleRates.push_back(srate);
							durations.push_back(durationMs);
							
							for(size_t j = 1; j < servers.size(); ++j){
								auto bufn = new ofxSCBuffer(0, 0, servers[j]->getServer());
								bufn->readChannel(wavPath, {ch});
								buffers.push_back(bufn);
							}
						}catch(const std::exception& e){
							ofLogError("scBuffer") << "Exception reading file: " << e.what();
							continue;
						}
					}
					files[wavPath.filename()] = numChannels;
				}
			}
			
			// ---- publish outputs
			buffersParam = newIndices;
			durationsMs  = newDurations;
			sampleRates  = newSampleRates;
		});
	}
	
	//using macrosave/macroload because they implement folder/file management
	//but the name is confusing, as this is not used for any macro (subpacth) operation
	void macroSave(ofJson &json, string presetFolderPath) override {
		if(embedInProject && !path.get().empty()) {
			string samplesFolder = presetFolderPath + "/samples";
			ofDirectory dir(samplesFolder);
			if(!dir.exists()) {
				dir.create(true);
			}
			
			string currentPath = path.get();
			string absolutePath;
			
			if(currentPath[0] == '/') {
				absolutePath = currentPath;
			} else {
				absolutePath = ofToDataPath("Supercollider/Samples/" + currentPath, true);
			}
			
			if(ofFile::doesFileExist(absolutePath)) {
				ofFile sourceFile(absolutePath);
				
				if(sourceFile.isDirectory()) {
					ofDirectory sourceDir(absolutePath);
					string destPath = samplesFolder + "/" + sourceFile.getFileName();
					sourceDir.copyTo(destPath, true, true);
				} else {
					string destPath = samplesFolder + "/" + sourceFile.getFileName();
					sourceFile.copyTo(destPath, true, true);
				}
				
				json["EmbedInProject"] = true;
				json["EmbeddedFilename"] = sourceFile.getFileName();  // Just the filename
				// Don't save OriginalPath - the path parameter itself stores the original
			}
		} else {
			json["EmbedInProject"] = false;
		}
	}

	void macroLoad(ofJson &json, string presetFolderPath) override {
		if(json.count("EmbedInProject") > 0 && json["EmbedInProject"].get<bool>()) {
			string embeddedFilename = json["EmbeddedFilename"].get<string>();
			string embeddedFullPath = ofToDataPath(presetFolderPath + "/samples/" + embeddedFilename, true);

			if(ofFile::doesFileExist(embeddedFullPath)) {
				embedInProject = true;
				currentPresetPath = presetFolderPath;

				string guiPresetPath = presetFolderPath;
				if(guiPresetPath.size() >= 2 && guiPresetPath[0]=='.' && guiPresetPath[1]=='/')
					guiPresetPath = guiPresetPath.substr(2);

				pathToSetAfterLoad = guiPresetPath + "/samples/" + embeddedFilename;
			} else {
				string errorMsg = "ERROR: Embedded samples not found!\n\n";
				errorMsg += "Expected: " + embeddedFullPath + "\n\n";
				errorMsg += "The original path will be used instead.";
				ofSystemAlertDialog(errorMsg);
				
				embedInProject = false;
				currentPresetPath = "";
				pathToSetAfterLoad = "";
			}
		} else {
			embedInProject = false;
			currentPresetPath = "";
			pathToSetAfterLoad = "";
		}
	}

	void presetRecallAfterSettingParameters(ofJson &json) override {
		// After all parameters are loaded, update path if we have embedded samples
		if(!pathToSetAfterLoad.empty()) {
			path = pathToSetAfterLoad;
			pathToSetAfterLoad = "";
		}
	}
    
private:
	
	struct EmbedInfo {
		bool isEmbedded = false;
		string originalPath = "";
	};
	std::map<string, EmbedInfo> embedInfo;
	
    bool findDataChunk(ofFile& file, uint32_t& dataSize) {
        char chunkID[4];
        uint32_t chunkSize;
        
        // Start from after the RIFF header (12 bytes)
        file.seekg(12);
        
        while(file.read((char*)&chunkID, 4).good()) {
            file.read((char*)&chunkSize, 4);
            
            if(strncmp(chunkID, "data", 4) == 0) {
                dataSize = chunkSize;
                return true;
            }
            
            // Skip this chunk
            file.seekg(chunkSize, std::ios::cur);
        }
        
        return false;
    }

    void getFileInfo(string filepath, int &numChannels, float &durationMs, float &sampleRate) { // Updated signature
        numChannels = 0;
        durationMs = 0;
        sampleRate = 0; // Initialize sample rate
        
        ofFile file(filepath, ofFile::ReadOnly, true);
        if(!file.is_open()) {
            ofLogError("scBuffer") << "Could not open file: " << filepath;
            return;
        }

        // Read RIFF header
        WaveHeader header;
        if(!file.read((char*)&header, sizeof(WaveHeader))) {
            ofLogError("scBuffer") << "Could not read WAV header: " << filepath;
            file.close();
            return;
        }

        // Verify RIFF header
        if(strncmp(header.riff, "RIFF", 4) != 0) {
            ofLogError("scBuffer") << "Invalid WAV file (no RIFF header): " << filepath;
            file.close();
            return;
        }

        // Store number of channels
        numChannels = header.channels;

        // Store sample rate
        sampleRate = (float)header.sample_rate; // Extract sample rate

        // Find the actual data chunk and its size
        uint32_t actualDataSize;
        if(!findDataChunk(file, actualDataSize)) {
            ofLogError("scBuffer") << "Could not find data chunk in WAV file: " << filepath;
            file.close();
            return;
        }

        // Calculate duration
        if(header.sample_rate > 0 && header.channels > 0 && header.bits_per_sample > 0) {
            int bytesPerSample = header.bits_per_sample / 8;
            int totalSamples = actualDataSize / (bytesPerSample * header.channels);
            durationMs = (float)totalSamples / header.sample_rate * 1000.0f;
            
            ofLogNotice("scBuffer") << "File info for: " << filepath;
            ofLogNotice("scBuffer") << "  Channels: " << numChannels;
            ofLogNotice("scBuffer") << "  Sample Rate: " << sampleRate;
            ofLogNotice("scBuffer") << "  Bits per Sample: " << header.bits_per_sample;
            ofLogNotice("scBuffer") << "  Data Size: " << actualDataSize;
            ofLogNotice("scBuffer") << "  Duration (ms): " << durationMs;
        }

        file.close();
    }
    
    ofEventListener listener;
    ofEventListener listener2;
    ofEventListener listener3;
    ofParameter<vector<int>> buffersParam;
    ofParameter<vector<float>> durationsMs;
    ofParameter<vector<float>> sampleRates; // New sample rate parameter
    vector<float> durations;  // Store durations for inspector display
    
    vector<serverManager*> servers;
    
    ofParameter<string> path;
    ofParameter<void> openFileDialog;
    
    customGuiRegion filenamesList;
        
    std::vector<ofxSCBuffer*> buffers;
    std::map<string, int> files;
	
	ofParameter<bool> embedInProject;
	string currentPresetPath = "";
	string pathToSetAfterLoad = "";


};

#endif /* scBuffer_h */
