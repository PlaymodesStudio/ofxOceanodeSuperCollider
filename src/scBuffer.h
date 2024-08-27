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

// WAVE file header format (https://github.com/BridgesUNCC/bridges-cxx/blob/master/src/AudioClip.h)
// Modified to comform with spec https://docs.fileformat.com/audio/wav/
struct WaveHeader {
    unsigned char riff[4];                // RIFF string
    unsigned int overall_size    ;        // overall size of file in bytes
    unsigned char wave[4];                // WAVE string
    unsigned char fmt_chunk_marker[4];    // fmt string with trailing null char
    unsigned int length_of_fmt;            // length of the format data
    unsigned short int format_type;            // format type. 1-PCM, 3- IEEE float, 6 - 8bit A law, 7 - 8bit mu law
    unsigned short int channels;                // no.of channels
    unsigned int sample_rate;            // sampling rate (blocks per second)
    unsigned int byterate;                // SampleRate * NumChannels * BitsPerSample/8
    unsigned int block_align;            // NumChannels * BitsPerSample/8
    unsigned int bits_per_sample;        // bits per sample, 8- 8bits, 16- 16 bits etc
    unsigned char data_chunk_header[4];    // DATA string or FLLR string
    unsigned int data_size;                // NumSamples * NumChannels *
    // BitsPerSample/8 - size of the
    // next chunk that will be read
    
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

        addInspectorParameter(filenamesList.set([this](){
            int i = 0;
            for(auto &file : files){
                for(int j = 0; j < file.second; j++){
                    ImGui::Text((ofToString(i) + " // " + file.first + " // ch" + ofToString(j+1)).c_str());
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

            if(s != ""){
                for(auto b : buffers){
                    b->free();
                    delete b;
                }
                buffers.clear();
                buffers.clear();
                files.clear();
                string absolutePath;
                if(s[0] == '/'){//Path is absolute
                    absolutePath = s;
                }
                else{ //Path is relative
                    absolutePath = ofToDataPath("Supercollider/Samples/" + s, true);
                }
                if(ofFilePath::getFileExt(absolutePath) == ""){ //is a folder
                    ofDirectory dir;
                    dir.open(absolutePath);
                    if(dir.exists()){
                        dir.sort();
                        for(auto f = dir.begin(); f < dir.end(); f++){
                            if(f->getExtension() == "wav"){
                                string wavPath = f->getAbsolutePath();
                                int numChannels = getFileNameNumChannels(wavPath);
                                for(int i = 0; i < numChannels; i++){
                                    auto bufref = buffers.emplace_back(new ofxSCBuffer(0, 0, servers[0]->getServer()));
                                    bufref->readChannel(wavPath, {i});
                                    
                                    newIndices.push_back(bufref->index);
                                }
                                if(numChannels > 0) files[f->getFileName()] = numChannels;
                            }
                        }
                    }
                }else if(ofFilePath::getFileExt(absolutePath) == "wav"){ //is a file
                    of::filesystem::path wavPath = absolutePath;
                    int numChannels = getFileNameNumChannels(wavPath);
                    for(int i = 0; i < numChannels; i++){
                        auto bufref = buffers.emplace_back(new ofxSCBuffer(0, 0, servers[0]->getServer()));
                        bufref->readChannel(wavPath, {i});
                        newIndices.push_back(bufref->index);
                    }
                    if(numChannels > 0) files[wavPath.filename()] = numChannels;
                }
                buffersParam = newIndices;
            }
        });
    }
    
private:
    int getFileNameNumChannels(string filepath){
        ofFile file;
        if(file.open(filepath, ofFile::ReadOnly, true)){
            WaveHeader *header;
            
            ofBuffer buffer(file, sizeof(WaveHeader));
            header = (WaveHeader*)buffer.getData();
            return header->channels;
        }
        return 0;
    }
    
    ofEventListener listener;
    ofEventListener listener2;
    ofEventListener listener3;
    ofParameter<vector<int>> buffersParam;
    
    vector<serverManager*> servers;
    
    ofParameter<string> path;
    ofParameter<void> openFileDialog;
    
    customGuiRegion filenamesList;
        
    std::vector<ofxSCBuffer*> buffers;
    std::map<string, int> files;
};

#endif /* scBuffer_h */
