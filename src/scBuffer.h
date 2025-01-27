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

        addInspectorParameter(filenamesList.set([this](){
            int i = 0;
            for(auto &file : files){
                for(int j = 0; j < file.second; j++){
                    ImGui::Text((ofToString(i) + " // " + file.first + " // ch" + ofToString(j+1) + " // " + ofToString(durations[i]) + "ms").c_str());
                    i++;
                }
            }
        }));

        listener2 = openFileDialog.newListener([this]{
            auto result = ofSystemLoadDialog("Select sample file or folder", true, ofToDataPath("Supercollider/Samples", true));
            if(result.bSuccess){
                string pathWidthData = result.getPath();
                ofStringReplace(pathWidthData, ofToDataPath("Supercollider/Samples/", true), "");
                path = pathWidthData;
            }
        });

        listener3 = path.newListener([this](string &s){
            vector<int> newIndices;
            vector<float> newDurations;
            durations.clear();

            if(s != ""){
                // Clear previous buffers
                for(auto b : buffers){
                    if(b != nullptr){
                        b->free();
                        delete b;
                    }
                }
                buffers.clear();
                files.clear();

                string absolutePath;
                if(s[0] == '/'){//Path is absolute
                    absolutePath = s;
                }
                else{ //Path is relative
                    absolutePath = ofToDataPath("Supercollider/Samples/" + s, true);
                }

                if(!ofFile::doesFileExist(absolutePath)){
                    ofLogError("scBuffer") << "Path does not exist: " << absolutePath;
                    buffersParam = newIndices;
                    durationsMs = newDurations;
                    return;
                }

                if(ofFilePath::getFileExt(absolutePath) == ""){ //is a folder
                    ofDirectory dir;
                    dir.open(absolutePath);
                    if(dir.exists()){
                        dir.sort();
                        for(auto f = dir.begin(); f < dir.end(); f++){
                            if(f->getExtension() == "wav"){
                                string wavPath = f->getAbsolutePath();
                                int numChannels = 0;
                                float durationMs = 0;
                                getFileInfo(wavPath, numChannels, durationMs);
                                
                                if(numChannels > 0){
                                    for(int i = 0; i < numChannels; i++){
                                        try {
                                            auto bufref = new ofxSCBuffer(0, 0, servers[0]->getServer());
                                            bufref->readChannel(wavPath, {i});
                                            buffers.push_back(bufref);
                                            newIndices.push_back(bufref->index);
                                            newDurations.push_back(durationMs);
                                            durations.push_back(durationMs);
                                        }
                                        catch(const std::exception& e){
                                            ofLogError("scBuffer") << "Exception reading file: " << e.what();
                                            continue;
                                        }
                                    }
                                    files[f->getFileName()] = numChannels;
                                }
                            }
                        }
                    }
                }else if(ofFilePath::getFileExt(absolutePath) == "wav"){ //is a file
                    of::filesystem::path wavPath = absolutePath;
                    int numChannels = 0;
                    float durationMs = 0;
                    getFileInfo(wavPath, numChannels, durationMs);
                    
                    if(numChannels > 0){
                        for(int i = 0; i < numChannels; i++){
                            try {
                                auto bufref = new ofxSCBuffer(0, 0, servers[0]->getServer());
                                bufref->readChannel(wavPath, {i});
                                buffers.push_back(bufref);
                                newIndices.push_back(bufref->index);
                                newDurations.push_back(durationMs);
                                durations.push_back(durationMs);
                            }
                            catch(const std::exception& e){
                                ofLogError("scBuffer") << "Exception reading file: " << e.what();
                                continue;
                            }
                        }
                        files[wavPath.filename()] = numChannels;
                    }
                }
                buffersParam = newIndices;
                durationsMs = newDurations;
            }
        });
    }
    
private:
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

    void getFileInfo(string filepath, int &numChannels, float &durationMs) {
        numChannels = 0;
        durationMs = 0;
        
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
            ofLogNotice("scBuffer") << "  Sample Rate: " << header.sample_rate;
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
    vector<float> durations;  // Store durations for inspector display
    
    vector<serverManager*> servers;
    
    ofParameter<string> path;
    ofParameter<void> openFileDialog;
    
    customGuiRegion filenamesList;
        
    std::vector<ofxSCBuffer*> buffers;
    std::map<string, int> files;
};

#endif /* scBuffer_h */
