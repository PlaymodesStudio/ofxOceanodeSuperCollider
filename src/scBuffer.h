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
#include <filesystem>

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
		addOutputParameter(sampleRates.set("Sample Rate", {0}, {0}, {FLT_MAX}));
		addParameter(select.set("Select", {0}, {0}, {INT_MAX}));
		addOutputParameter(selectOut.set("Select Out", {0}, {0}, {INT_MAX}));
		
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
		addInspectorParameter(unembedButton.set("Unembed (Restore Original)"));
		
		// Unembed button listener
		unembedListener = unembedButton.newListener([this](){
			if(!originalPath.empty() && originalPath != path.get()) {
				string absOriginal = resolveToAbsolutePath(originalPath);
				if(ofFile::doesFileExist(absOriginal)) {
					path = originalPath;
					embedInProject = false;
					originalPath = "";
				} else {
					ofSystemAlertDialog("Cannot unembed: original file no longer exists at:\n" + originalPath);
				}
			}
		});
		
		listener2 = openFileDialog.newListener([this]{
			auto result = ofSystemLoadDialog("Select sample file or folder", false, ofToDataPath("Supercollider/Samples", true));
			if(result.bSuccess){
				string pathWidthData = result.getPath();
				ofStringReplace(pathWidthData, ofToDataPath("Supercollider/Samples/", true), "");
				path = pathWidthData;
				originalPath = "";
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
			
			// ---- resolve to absolute path
			string absolutePath = resolveToAbsolutePath(s);
			
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
					dir.allowExt("WAV");
					dir.sort();
					for(auto f = dir.begin(); f < dir.end(); ++f){
						string wavPath = f->getAbsolutePath();
						int   numChannels = 0;
						float durationMs  = 0.0f;
						float srate       = 0.0f;
						
						getFileInfo(wavPath, numChannels, durationMs, srate);

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
			}else{
				std::string ext = ofFilePath::getFileExt(absolutePath);
				for(auto &c : ext) c = std::tolower(static_cast<unsigned char>(c));
				if(ext == "wav"){
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
			}
			
			// ---- publish outputs
			buffersParam = newIndices;
			durationsMs  = newDurations;
			sampleRates  = newSampleRates;
		});
		
		listener4 = select.newListener([this](vector<int> &selection){
			vector<int> selectedBuffers;
			
			// For each selected sample index
			for(int sampleIdx : selection){
				// Find which buffers correspond to this sample
				int currentSample = 0;
				int bufferIdx = 0;
				
				for(auto &file : files){
					int numChannels = file.second;
					
					if(currentSample == sampleIdx){
						// Add all channels of this sample
						for(int ch = 0; ch < numChannels; ch++){
							if(bufferIdx + ch < buffersParam.get().size()){
								selectedBuffers.push_back(buffersParam.get()[bufferIdx + ch]);
							}
						}
						break;
					}
					
					currentSample++;
					bufferIdx += numChannels;
				}
			}
			
			selectOut = selectedBuffers;
		});
	}
	
	void macroSave(ofJson &json, string presetFolderPath) override {
		if(embedInProject && !path.get().empty()) {
			string currentPath = path.get();
			string absolutePath = resolveToAbsolutePath(currentPath);
			
			if(!ofFile::doesFileExist(absolutePath)) {
				json["EmbedInProject"] = false;
				return;
			}
			
			// Compute destination paths
			string absPresetPath = ofToDataPath(presetFolderPath, true);
			string absSamplesFolder = absPresetPath + "/samples";
			
			// Create samples folder
			try {
				std::filesystem::path samplesDir(absSamplesFolder);
				if(!std::filesystem::exists(samplesDir)) {
					std::filesystem::create_directories(samplesDir);
				}
			} catch(const std::exception& e) {
				ofLogError("scBuffer") << "Failed to create samples directory: " << e.what();
				json["EmbedInProject"] = false;
				return;
			}
			
			// Get filename and destination
			std::filesystem::path sourcePath(absolutePath);
			string filename = sourcePath.filename().string();
			string absDestPath = absSamplesFolder + "/" + filename;
			
			// Copy file if source != destination
			try {
				std::filesystem::path srcCanonical = std::filesystem::weakly_canonical(sourcePath);
				std::filesystem::path destCanonical = std::filesystem::weakly_canonical(std::filesystem::path(absDestPath));
				
				if(srcCanonical != destCanonical) {
					// Store original path for unembed
					if(originalPath.empty()) {
						originalPath = currentPath;
					}
					
					// Copy the file
					if(std::filesystem::is_directory(sourcePath)) {
						std::filesystem::copy(sourcePath, absDestPath,
							std::filesystem::copy_options::recursive |
							std::filesystem::copy_options::overwrite_existing);
					} else {
						std::filesystem::copy_file(sourcePath, absDestPath,
							std::filesystem::copy_options::overwrite_existing);
					}
				}
			} catch(const std::exception& e) {
				ofLogError("scBuffer") << "Filesystem error during copy: " << e.what();
				json["EmbedInProject"] = false;
				return;
			}
			
			// Compute data-relative path for the embedded sample
			string embeddedRelativePath = computeDataRelativePath(absPresetPath) + "/samples/" + filename;
			
			// Update path parameter and JSON
			path = embeddedRelativePath;
			json["Path"] = embeddedRelativePath;
			
			if(!originalPath.empty()) {
				json["OriginalPath"] = originalPath;
			}
			json["EmbedInProject"] = true;
		} else {
			json["EmbedInProject"] = false;
		}
	}

	void macroLoad(ofJson &json, string presetFolderPath) override {
		if(json.count("EmbedInProject") > 0 && json["EmbedInProject"].get<bool>()) {
			embedInProject = true;
			if(json.count("OriginalPath") > 0) {
				originalPath = json["OriginalPath"].get<string>();
			}
		} else {
			embedInProject = false;
			originalPath = "";
		}
	}

	void presetRecallAfterSettingParameters(ofJson &json) override {
		// Nothing needed - path parameter already contains correct path
	}
	
private:
	
	// Resolve any path format to absolute path
	string resolveToAbsolutePath(const string& inputPath) {
		string sClean = inputPath;
		if(sClean.size() >= 2 && sClean[0] == '.' && sClean[1] == '/'){
			sClean = sClean.substr(2);
		}
		
		if(!sClean.empty() && sClean[0] == '/') {
			return sClean;
		}
		
		// Check for known data-relative prefixes
		bool isKnownDataRelative = (sClean.rfind("Macros/", 0) == 0) ||
								   (sClean.rfind("Presets/", 0) == 0) ||
								   (sClean.rfind("Supercollider/", 0) == 0);
		
		if(isKnownDataRelative) {
			return ofToDataPath(sClean, true);
		}
		
		// Try data-relative first
		string dataRelativePath = ofToDataPath(sClean, true);
		if(ofFile::doesFileExist(dataRelativePath)){
			return dataRelativePath;
		}
		
		// Fallback to Supercollider/Samples/
		return ofToDataPath("Supercollider/Samples/" + sClean, true);
	}
	
	// Compute data-relative path from absolute path
	string computeDataRelativePath(const string& absPath) {
		// Look for "Macros/" in the path
		size_t macrosPos = absPath.find("/Macros/");
		if(macrosPos != string::npos) {
			return absPath.substr(macrosPos + 1);
		}
		
		// Look for "Presets/" in the path
		size_t presetsPos = absPath.find("/Presets/");
		if(presetsPos != string::npos) {
			return absPath.substr(presetsPos + 1);
		}
		
		// Fallback: strip data path
		string dataPath = ofToDataPath("", true);
		if(absPath.find(dataPath) == 0) {
			string rel = absPath.substr(dataPath.length());
			if(!rel.empty() && rel[0] == '/') {
				rel = rel.substr(1);
			}
			return rel;
		}
		
		return ofFilePath::getBaseName(absPath);
	}
	
	bool findDataChunk(ofFile& file, uint32_t& dataSize) {
		char chunkID[4];
		uint32_t chunkSize;

		file.seekg(12, std::ios::beg);

		while (file.read(reinterpret_cast<char*>(&chunkID), 4).good()) {
			if(!file.read(reinterpret_cast<char*>(&chunkSize), 4).good()){
				return false;
			}

			if (std::strncmp(chunkID, "data", 4) == 0) {
				dataSize = chunkSize;
				return true;
			}

			file.seekg(chunkSize, std::ios::cur);

			if (chunkSize & 1u) {
				file.seekg(1, std::ios::cur);
			}
		}
		return false;
	}

	void getFileInfo(string filepath, int &numChannels, float &durationMs, float &sampleRate) {
		numChannels = 0;
		durationMs  = 0.0f;
		sampleRate  = 0.0f;

		ofFile file(filepath, ofFile::ReadOnly, true);
		if(!file.is_open()){
			ofLogError("scBuffer") << "Could not open file: " << filepath;
			return;
		}

		char riff[4], wave[4];
		uint32_t riffSize = 0;
		if(!file.read((char*)&riff, 4) || !file.read((char*)&riffSize, 4) || !file.read((char*)&wave, 4)){
			ofLogError("scBuffer") << "Could not read RIFF header: " << filepath;
			file.close();
			return;
		}
		if(std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0){
			ofLogError("scBuffer") << "Not a RIFF/WAVE file: " << filepath;
			file.close();
			return;
		}

		bool haveFmt  = false;
		bool haveData = false;
		uint16_t bitsPerSample = 0;
		uint32_t dataSize = 0;

		while(true){
			char chunkID[4];
			uint32_t chunkSize = 0;
			if(!file.read((char*)&chunkID, 4)) break;
			if(!file.read((char*)&chunkSize, 4)) { haveData |= false; break; }

			std::streampos payloadPos = file.tellg();

			if(std::strncmp(chunkID, "fmt ", 4) == 0){
				if(chunkSize >= 16){
					uint16_t formatType = 0;
					uint16_t channels   = 0;
					uint32_t srate      = 0;
					uint32_t byterate   = 0;
					uint16_t blockAlign = 0;
					uint16_t bits       = 0;

					file.read((char*)&formatType, 2);
					file.read((char*)&channels,   2);
					file.read((char*)&srate,      4);
					file.read((char*)&byterate,   4);
					file.read((char*)&blockAlign, 2);
					file.read((char*)&bits,       2);

					numChannels    = channels;
					sampleRate     = (float)srate;
					bitsPerSample  = bits;
					haveFmt        = true;

					auto consumed = 16u;
					if(chunkSize > consumed){
						file.seekg(chunkSize - consumed, std::ios::cur);
					}
				}else{
					file.seekg(chunkSize, std::ios::cur);
				}
			}else if(std::strncmp(chunkID, "data", 4) == 0){
				dataSize  = chunkSize;
				haveData  = true;
				file.seekg(chunkSize, std::ios::cur);
			}else{
				file.seekg(chunkSize, std::ios::cur);
			}

			if(chunkSize & 1u){
				file.seekg(1, std::ios::cur);
			}

			if(haveFmt && haveData) break;
		}

		if(haveFmt && haveData && sampleRate > 0.0f && numChannels > 0 && bitsPerSample > 0){
			int bytesPerSample = bitsPerSample / 8;
			int totalSamples   = (bytesPerSample > 0 && numChannels > 0) ? (dataSize / (bytesPerSample * numChannels)) : 0;
			durationMs         = (totalSamples > 0) ? (float(totalSamples) / sampleRate * 1000.0f) : 0.0f;
		}

		file.close();
	}

	
	ofEventListener listener;
	ofEventListener listener2;
	ofEventListener listener3;
	ofEventListener listener4;
	ofEventListener unembedListener;
	ofParameter<vector<int>> select;
	ofParameter<vector<int>> selectOut;
	ofParameter<vector<int>> buffersParam;
	ofParameter<vector<float>> durationsMs;
	ofParameter<vector<float>> sampleRates;
	vector<float> durations;
	
	vector<serverManager*> servers;
	
	ofParameter<string> path;
	ofParameter<void> openFileDialog;
	ofParameter<void> unembedButton;
	
	customGuiRegion filenamesList;
		
	std::vector<ofxSCBuffer*> buffers;
	std::map<string, int> files;
	
	ofParameter<bool> embedInProject;
	string originalPath = "";
};

#endif /* scBuffer_h */
