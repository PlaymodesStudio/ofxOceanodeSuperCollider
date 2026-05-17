// scWavetableBuffer.h - Thread-safe version

#ifndef scWavetableBuffer_h
#define scWavetableBuffer_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSuperCollider.h"
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <mutex>

class scWavetableBuffer : public ofxOceanodeNodeModel {
public:
	scWavetableBuffer(vector<serverManager*> outputServers)
		: ofxOceanodeNodeModel("SC Wavetable Buffer") {
		servers = outputServers;
		isProcessing = false;
		shouldLoadBuffer = false;
	};
	
	~scWavetableBuffer() {
		if(processingThread.joinable()) {
			processingThread.join();
		}
		
		for(auto b : buffers) {
			b->free();
			delete b;
		}
		buffers.clear();
	}
	
	void setup() {
		addParameter(path.set("Path", ""));
		addParameter(openFileDialog.set("Open"));
		addOutputParameter(bufferParam.set("Buffer", {0}, {0}, {INT_MAX}));
		addOutputParameter(numChannels.set("Num Channels", {0}, {0}, {INT_MAX}));
		addOutputParameter(numWavetables.set("Num Wavetables", {256}, {0}, {INT_MAX}));
		addOutputParameter(durationMs.set("Duration", {0}, {0}, {FLT_MAX}));
		addOutputParameter(sampleRateParam.set("Sample Rate", {0}, {0}, {FLT_MAX}));
		
		addParameter(fftSize.set("FFT Size", 2048, 512, 8192));
		addParameter(filtersPerOctave.set("Filters/Octave", 3, 1, 12));
		addParameter(normalizeLinkChans.set("Normalize Link", true));
		
		addInspectorParameter(embedInProject.set("Embed Wavetable", false));
		
		addInspectorParameter(processingStatus.set([this](){
			ImGui::Text("Status: %s", statusText.c_str());
			if(isProcessing) {
				ImGui::Text("Processing...");
			}
			if(!originalFile.empty()) {
				ImGui::Text("Original: %s", ofFile(originalFile).getFileName().c_str());
			}
			if(!processedFile.empty()) {
				ImGui::Text("Processed: %s", ofFile(processedFile).getFileName().c_str());
			}
			ImGui::Text("Channels: %d", currentNumChannels);
		}));
		
		listener = openFileDialog.newListener([this]{
			auto result = ofSystemLoadDialog("Select wavetable file", false,
				ofToDataPath("Supercollider/Wavetables", true));
			if(result.bSuccess) {
				path = result.getPath();
			}
		});
		
		listener2 = path.newListener([this](string &s){
			if(s.empty()) {
				resetOutputs();
				return;
			}
			
			loadWavetable(s);
		});
	}
	
	// Called from main thread
	void update(ofEventArgs &e) override {
		// Check if background thread finished processing
		if(shouldLoadBuffer.load()) {
			std::lock_guard<std::mutex> lock(bufferMutex);
			
			// Load buffer on main thread (thread-safe with OSC)
			actuallyLoadBuffer(pendingProcessedPath, pendingChannels, pendingFrames, pendingSrate);
			
			shouldLoadBuffer = false;
		}
	}
	
	void macroSave(ofJson &json, string presetFolderPath) override {
		if(embedInProject && !path.get().empty()) {
			string wavetablesFolder = presetFolderPath + "/wavetables";
			ofDirectory dir(wavetablesFolder);
			if(!dir.exists()) {
				dir.create(true);
			}
			
			string currentPath = path.get();
			string absolutePath;
			
			if(!currentPath.empty() && currentPath[0] == '/') {
				absolutePath = currentPath;
			} else if(currentPath.rfind("Presets/", 0) == 0) {
				absolutePath = ofToDataPath(currentPath, true);
			} else {
				absolutePath = ofToDataPath("Supercollider/Wavetables/" + currentPath, true);
			}
			
			if(ofFile::doesFileExist(absolutePath)) {
				ofFile sourceFile(absolutePath);
				string destPath = wavetablesFolder + "/" + sourceFile.getFileName();
				sourceFile.copyTo(destPath, true, true);
				
				json["EmbedInProject"] = true;
				json["EmbeddedFilename"] = sourceFile.getFileName();
			}
		} else {
			json["EmbedInProject"] = false;
		}
		
		json["FFTSize"] = fftSize.get();
		json["FiltersPerOctave"] = filtersPerOctave.get();
		json["NormalizeLinkChans"] = normalizeLinkChans.get();
	}
	
	void macroLoad(ofJson &json, string presetFolderPath) override {
		if(json.count("FFTSize") > 0) {
			fftSize = json["FFTSize"].get<int>();
		}
		if(json.count("FiltersPerOctave") > 0) {
			filtersPerOctave = json["FiltersPerOctave"].get<int>();
		}
		if(json.count("NormalizeLinkChans") > 0) {
			normalizeLinkChans = json["NormalizeLinkChans"].get<bool>();
		}
		
		if(json.count("EmbedInProject") > 0 && json["EmbedInProject"].get<bool>()) {
			string embeddedFilename = json["EmbeddedFilename"].get<string>();
			string embeddedFullPath = ofToDataPath(presetFolderPath + "/wavetables/" + embeddedFilename, true);
			
			if(ofFile::doesFileExist(embeddedFullPath)) {
				embedInProject = true;
				currentPresetPath = presetFolderPath;
				
				string guiPresetPath = presetFolderPath;
				if(guiPresetPath.size() >= 2 && guiPresetPath[0]=='.' && guiPresetPath[1]=='/')
					guiPresetPath = guiPresetPath.substr(2);
				
				pathToSetAfterLoad = guiPresetPath + "/wavetables/" + embeddedFilename;
			} else {
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
		if(!pathToSetAfterLoad.empty()) {
			path = pathToSetAfterLoad;
			pathToSetAfterLoad = "";
		}
	}
	
private:
	void resetOutputs() {
		bufferParam = vector<int>{0};
		numChannels = vector<int>{0};
		numWavetables = vector<int>{256};
		durationMs = vector<float>{0};
		sampleRateParam = vector<float>{0};
		statusText = "No file loaded";
		originalFile = "";
		processedFile = "";
		currentNumChannels = 0;
	}
	
	time_t getFileModificationTime(const string& filepath) {
		struct stat fileInfo;
		if(stat(filepath.c_str(), &fileInfo) != 0) {
			return 0;
		}
		return fileInfo.st_mtime;
	}
	
	bool isWavetableFormat(const string& filepath, int& channels, int& frames, float& srate) {
		ofFile file(filepath, ofFile::ReadOnly, true);
		if(!file.is_open()) return false;
		
		char riff[4], wave[4];
		uint32_t riffSize = 0;
		
		if(!file.read((char*)&riff, 4) ||
		   !file.read((char*)&riffSize, 4) ||
		   !file.read((char*)&wave, 4)) {
			return false;
		}
		
		if(strncmp(riff, "RIFF", 4) != 0 || strncmp(wave, "WAVE", 4) != 0) {
			return false;
		}
		
		bool haveFmt = false;
		uint16_t numChans = 0;
		uint32_t sampleRate = 0;
		uint32_t dataSize = 0;
		uint16_t bitsPerSample = 0;
		
		while(true) {
			char chunkID[4];
			uint32_t chunkSize = 0;
			if(!file.read((char*)&chunkID, 4)) break;
			if(!file.read((char*)&chunkSize, 4)) break;
			
			if(strncmp(chunkID, "fmt ", 4) == 0) {
				if(chunkSize >= 16) {
					uint16_t formatType = 0;
					file.read((char*)&formatType, 2);
					file.read((char*)&numChans, 2);
					file.read((char*)&sampleRate, 4);
					uint32_t byterate = 0;
					file.read((char*)&byterate, 4);
					uint16_t blockAlign = 0;
					file.read((char*)&blockAlign, 2);
					file.read((char*)&bitsPerSample, 2);
					haveFmt = true;
					if(chunkSize > 16) {
						file.seekg(chunkSize - 16, ios::cur);
					}
				}
			} else if(strncmp(chunkID, "data", 4) == 0) {
				dataSize = chunkSize;
				break;
			} else {
				file.seekg(chunkSize, ios::cur);
			}
			
			if(chunkSize & 1u) {
				file.seekg(1, ios::cur);
			}
		}
		
		file.close();
		
		if(!haveFmt || dataSize == 0) return false;
		
		int bytesPerSample = bitsPerSample / 8;
		if(bytesPerSample == 0 || numChans == 0) return false;
		
		int totalSamples = dataSize / (bytesPerSample * numChans);
		int framesPerWavetable = 2048;
		int numWavetablesInFile = totalSamples / framesPerWavetable;
		
		channels = numChans;
		frames = totalSamples;
		srate = sampleRate;
		
		bool isValid = (numWavetablesInFile >= 1 &&
					   totalSamples % framesPerWavetable == 0 &&
					   numChans == 1);
		
		if(isValid) {
			ofLogNotice("scWavetableBuffer") << "Detected wavetable: "
				<< numWavetablesInFile << " wavetables × "
				<< framesPerWavetable << " samples";
		}
		
		return isValid;
	}
	
	string getSclangPath() {
		vector<string> paths = {
			"/Applications/SuperCollider.app/Contents/MacOS/sclang",
			"/usr/local/bin/sclang",
			"sclang"
		};
		
		for(const auto& p : paths) {
			string checkCmd = "which " + p + " > /dev/null 2>&1";
			if(system(checkCmd.c_str()) == 0) {
				return p;
			}
		}
		
		return "";
	}
	
	void processWavetableAsync(const string& inputPath, const string& outputPath,
							   int channels, int frames, float srate) {
		if(processingThread.joinable()) {
			processingThread.join();
		}
		
		processingThread = std::thread([this, inputPath, outputPath, channels, frames, srate]() {
			isProcessing = true;
			
			string sclangPath = getSclangPath();
			if(sclangPath.empty()) {
				ofLogError("scWavetableBuffer") << "sclang not found";
				statusText = "ERROR: sclang not found";
				isProcessing = false;
				return;
			}
			
			string scriptPath = ofToDataPath("Supercollider/Scripts/process_wavetable.scd", true);
			
			if(!ofFile::doesFileExist(scriptPath)) {
				ofLogError("scWavetableBuffer") << "Script not found: " << scriptPath;
				statusText = "ERROR: Script not found";
				isProcessing = false;
				return;
			}
			
			stringstream cmd;
			cmd << sclangPath << " " << scriptPath << " "
				<< "\"" << inputPath << "\" "
				<< "\"" << outputPath << "\" "
				<< fftSize.get() << " "
				<< filtersPerOctave.get() << " "
				<< (normalizeLinkChans.get() ? "true" : "false")
				<< " 2>&1";
			
			string command = cmd.str();
			
			ofLogNotice("scWavetableBuffer") << "Running: " << command;
			statusText = "Processing...";
			
			FILE* pipe = popen(command.c_str(), "r");
			if(!pipe) {
				ofLogError("scWavetableBuffer") << "Failed to run sclang";
				statusText = "ERROR: Failed to run sclang";
				isProcessing = false;
				return;
			}
			
			char buffer[256];
			while(fgets(buffer, sizeof(buffer), pipe) != nullptr) {
				ofLogNotice("sclang") << buffer;
			}
			
			int returnCode = pclose(pipe);
			
			if(returnCode != 0 || !ofFile::doesFileExist(outputPath)) {
				ofLogError("scWavetableBuffer") << "Processing failed";
				statusText = "ERROR: Processing failed";
				isProcessing = false;
				return;
			}
			
			// Signal main thread to load buffer
			{
				std::lock_guard<std::mutex> lock(bufferMutex);
				pendingProcessedPath = outputPath;
				pendingChannels = channels;
				pendingFrames = frames;
				pendingSrate = srate;
			}
			shouldLoadBuffer = true;
			
			isProcessing = false;
		});
	}
	
	void actuallyLoadBuffer(const string& processedPath, int channels, int frames, float srate) {
		// This runs on main thread - safe to call OSC operations
		
		try {
			for(auto b : buffers) {
				if(b != nullptr) {
					b->free();
					delete b;
				}
			}
			buffers.clear();
			
			auto buf0 = new ofxSCBuffer(0, 0, servers[0]->getServer());
			buf0->read(processedPath);
			buffers.push_back(buf0);
			
			int estimatedOctaves = 10;
			currentNumChannels = estimatedOctaves * filtersPerOctave.get() + 1;
			
			for(size_t j = 1; j < servers.size(); ++j) {
				auto bufn = new ofxSCBuffer(0, 0, servers[j]->getServer());
				bufn->read(processedPath);
				buffers.push_back(bufn);
			}
			
			bufferParam = vector<int>{buf0->index};
			numChannels = vector<int>{currentNumChannels};
			numWavetables = vector<int>{256};
			
			float duration = (float)frames / srate * 1000.0f;
			durationMs = vector<float>{duration};
			sampleRateParam = vector<float>{srate};
			
			statusText = "Loaded successfully";
			processedFile = processedPath;
			
			ofLogNotice("scWavetableBuffer") << "Loaded buffer: " << buf0->index
				<< " with estimated " << currentNumChannels << " channels";
			
		} catch(const std::exception& e) {
			ofLogError("scWavetableBuffer") << "Error loading buffer: " << e.what();
			statusText = "ERROR: Failed to load buffer";
			resetOutputs();
		}
	}
	
	void loadWavetable(const string& filepath) {
		string absolutePath;
		string sClean = filepath;
		
		if(sClean.size() >= 2 && sClean[0] == '.' && sClean[1] == '/'){
			sClean = sClean.substr(2);
		}
		
		if(embedInProject && !currentPresetPath.empty()){
			ofFile f(sClean);
			string filename = f.getFileName();
			string embeddedPath = ofToDataPath(currentPresetPath + "/wavetables/" + filename, true);
			if(ofFile::doesFileExist(embeddedPath)){
				absolutePath = embeddedPath;
			}
		}
		
		if(absolutePath.empty()){
			if(!sClean.empty() && sClean[0] == '/'){
				absolutePath = sClean;
			}else if(sClean.rfind("Presets/", 0) == 0){
				absolutePath = ofToDataPath(sClean, true);
			}else{
				absolutePath = ofToDataPath("Supercollider/Wavetables/" + sClean, true);
			}
		}
		
		int channels = 0;
		int frames = 0;
		float srate = 0;
		
		if(!isWavetableFormat(absolutePath, channels, frames, srate)) {
			ofLogError("scWavetableBuffer") << "Not a valid wavetable format: " << absolutePath;
			statusText = "ERROR: Not a valid wavetable";
			resetOutputs();
			return;
		}
		
		originalFile = absolutePath;
		
		ofFile inputFile(absolutePath);
		string baseName = inputFile.getBaseName();
		string processedPath = ofToDataPath("Supercollider/Wavetables/processed/" +
			baseName + "_mipmap.wav", true);
		
		ofDirectory processedDir(ofToDataPath("Supercollider/Wavetables/processed", true));
		if(!processedDir.exists()) {
			processedDir.create(true);
		}
		
		bool needsProcessing = true;
		if(ofFile::doesFileExist(processedPath)) {
			time_t sourceTime = getFileModificationTime(absolutePath);
			time_t processedTime = getFileModificationTime(processedPath);
			needsProcessing = (sourceTime > processedTime);
		}
		
		if(needsProcessing) {
			ofLogNotice("scWavetableBuffer") << "Processing wavetable asynchronously...";
			processWavetableAsync(absolutePath, processedPath, channels, frames, srate);
		} else {
			ofLogNotice("scWavetableBuffer") << "Using cached processed file";
			statusText = "Using cached file";
			actuallyLoadBuffer(processedPath, channels, frames, srate);
		}
	}
	
	ofEventListener listener;
	ofEventListener listener2;
	
	ofParameter<string> path;
	ofParameter<void> openFileDialog;
	ofParameter<int> fftSize;
	ofParameter<int> filtersPerOctave;
	ofParameter<bool> normalizeLinkChans;
	ofParameter<bool> embedInProject;
	
	ofParameter<vector<int>> bufferParam;
	ofParameter<vector<int>> numChannels;
	ofParameter<vector<int>> numWavetables;
	ofParameter<vector<float>> durationMs;
	ofParameter<vector<float>> sampleRateParam;
	
	customGuiRegion processingStatus;
	
	vector<serverManager*> servers;
	vector<ofxSCBuffer*> buffers;
	
	string statusText = "No file loaded";
	string originalFile;
	string processedFile;
	int currentNumChannels = 0;
	
	string currentPresetPath = "";
	string pathToSetAfterLoad = "";
	
	std::thread processingThread;
	std::atomic<bool> isProcessing;
	std::atomic<bool> shouldLoadBuffer;
	std::mutex bufferMutex;
	
	// Pending buffer info (written by background thread, read by main thread)
	string pendingProcessedPath;
	int pendingChannels;
	int pendingFrames;
	float pendingSrate;
};

#endif /* scWavetableBuffer_h */
