//
//  scStart.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 24/01/2022.
//

#ifndef scStart_h
#define scStart_h

#include "ofThread.h"
#include "serverManager.h"
#include <atomic>
#include <mutex>
#include <vector>
#ifndef TARGET_WIN32
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
extern char **environ;
#endif

// this needs to be threaded otherwise it blocks the main thread
class scStart : public ofThread{
  
public:
    scStart(scPreferences _prefs){
        prefs = _prefs;
        
        if(ofDirectory::doesDirectoryExist(ofToDataPath("Supercollider/Scsynth/bin/"))){
            scPath = ofToDataPath("Supercollider/Scsynth/bin/scsynth", true);
        }else if(ofDirectory::doesDirectoryExist("/Applications/SuperCollider.app/Contents/Resources/")){
            scPath = "/Applications/SuperCollider.app/Contents/Resources/scsynth";
        }else{
            ofLog() << "No ScSynth found on the system";
        }
    }
    
    void start(){
        reboot = true;
        if(!isThreadRunning()) startThread();
    }

    void setPreferences(scPreferences _prefs){
        std::lock_guard<std::mutex> lock(preferencesMutex);
        prefs = _prefs;
    }

    void restartServer(scPreferences _prefs){
        {
            std::lock_guard<std::mutex> lock(preferencesMutex);
            prefs = _prefs;
        }
        reboot = true;
        if(isThreadRunning()){
            stopProcess();   // the thread's loop launches it again
        }else{
            startThread();
        }
    }
      
    void threadedFunction(){
        while(reboot){
            uint64_t launchTime = ofGetElapsedTimeMillis();
            scPreferences launchPrefs;
            {
                std::lock_guard<std::mutex> lock(preferencesMutex);
                launchPrefs = prefs;
            }
            std::vector<std::string> args = {scPath,
                "-u", ofToString(launchPrefs.udpPort),
                "-B", launchPrefs.bindAddress,
                "-c", ofToString(launchPrefs.numControlBusChannels),
                "-a", ofToString(launchPrefs.numAudioBusChannels),
                "-i", ofToString(launchPrefs.numInputBusChannels),
                "-o", ofToString(launchPrefs.numOutputBusChannels),
                "-z", ofToString(launchPrefs.blockSize),
                "-Z", ofToString(launchPrefs.hardwareBufferSize),
                "-S", ofToString(launchPrefs.hardwareSampleRate),
                "-b", ofToString(launchPrefs.numBuffers),
                "-n", ofToString(launchPrefs.maxNodes),
                "-d", ofToString(launchPrefs.maxSynthDefs),
                "-m", ofToString(launchPrefs.memSize),
                "-w", ofToString(launchPrefs.numWireBufs),
                "-r", ofToString(launchPrefs.numRGens),
                "-l", ofToString(launchPrefs.maxLogins),
                "-s", ofToString(launchPrefs.safetyClipThreshold)};
            const bool hasOutputDevice = launchPrefs.deviceName != "nil" && !launchPrefs.deviceName.empty();
            const bool hasInputDevice = launchPrefs.inputDeviceName != "nil" && !launchPrefs.inputDeviceName.empty();
            args.push_back("-H");
            if(!hasOutputDevice && !hasInputDevice){
                args.push_back("nil");
            }else{
                // scsynth's two-argument form: -H <inputDevice> <outputDevice>.
                // An empty string means "use the system default" for that side.
                args.push_back(hasInputDevice ? launchPrefs.inputDeviceName : "");
                args.push_back(hasOutputDevice ? launchPrefs.deviceName : "");
            }
            args.push_back("-D");
            args.push_back("0"); //Deactivate synthdefs
            const std::string pluginsPath = scPath.substr(0, scPath.size()-7);
            if(launchPrefs.ugensPlugins != ""){
                args.push_back("-U");
                args.push_back(ofToDataPath(launchPrefs.ugensPlugins, true) + ":" + pluginsPath);
            }

            std::string described;
            for(auto& arg : args) described += (arg.find(' ') != std::string::npos ? "'" + arg + "'" : arg) + " ";
            ofLogNotice("scStart") << "Launching scsynth: " << described;
            clearPort(launchPrefs.udpPort);
            runProcess(args);

            if(reboot && ofGetElapsedTimeMillis() - launchTime < 1000){
                ofSleepMillis(1000);
            }
        }
    }
      
      
    ~scStart(){
        killServer();
    }
    
    // Stops this server's scsynth only. It used to `killall scsynth`, which
    // also took down every other server of the app (and any SuperCollider
    // running beside it); they then rebooted and lost their graphs.
    void killServer(){
        reboot = false;
        stopProcess();
        waitForThread(false);
    }
    
private:
#ifdef TARGET_WIN32
    void runProcess(const std::vector<std::string>& args){
        std::string command;
        for(auto& arg : args) command += shellQuote(arg) + " ";
        FILE* pipe = _popen(command.c_str(), "r");
        if(pipe == nullptr){
            ofLogError("scStart") << "Could not start scsynth";
            return;
        }
        readOutput(pipe);
        _pclose(pipe);
    }
    void stopProcess(){
        ofSystem("taskkill /IM scsynth.exe /F");
    }
    void clearPort(int){}
#else
    // posix_spawn with an argument vector: no shell, no quoting, and the pid
    // of this server's own scsynth to stop it with
    void runProcess(const std::vector<std::string>& args){
        int fds[2];
        if(pipe(fds) != 0){
            ofLogError("scStart") << "Could not create a pipe for scsynth";
            return;
        }
        std::vector<char*> argv;
        for(auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        pid_t pid = -1;
        const int error = posix_spawn(&pid, scPath.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        if(error != 0){
            close(fds[0]);
            ofLogError("scStart") << "Could not start " << scPath << ": " << strerror(error);
            return;
        }
        processId = pid;
        FILE* output = fdopen(fds[0], "r");
        if(output != nullptr){
            readOutput(output);
            fclose(output);
        }else{
            close(fds[0]);
        }
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR){}
        processId = -1;
    }
    void stopProcess(){
        const pid_t pid = processId.load();
        if(pid > 0) terminate(pid);
    }
    // SIGTERM, then SIGKILL if it is still there after a second: scsynth can
    // sit in audio-device setup without reacting, and killServer() waits for
    // the thread (the UI with it) until the process is gone
    static void terminate(pid_t pid){
        kill(pid, SIGTERM);
        for(int i = 0; i < 40 && kill(pid, 0) == 0; i++) ofSleepMillis(25);
        if(kill(pid, 0) == 0) kill(pid, SIGKILL);
    }
    // An scsynth left over from a crashed session still holds the port, and a
    // new one could not bind it: stop whatever scsynth listens on this
    // server's port, and nothing else
    void clearPort(int port){
        const std::string pids = ofSystem("lsof -a -t -c scsynth -iUDP:" + ofToString(port) + " 2>/dev/null");
        for(auto& line : ofSplitString(pids, "\n", true, true)){
            const pid_t pid = (pid_t)ofToInt(line);
            if(pid <= 0 || pid == processId.load()) continue;
            ofLogNotice("scStart") << "Stopping a leftover scsynth (pid " << pid << ") on port " << port;
            terminate(pid);
        }
    }
#endif

    // scsynth's console, line by line, until it exits
    void readOutput(FILE* stream){
        std::string line;
        int c;
        while((c = fgetc(stream)) != EOF){
            if(c == '\n'){
                ofLog() << line;
                line.clear();
            }else{
                line += (char)c;
            }
        }
        if(!line.empty()) ofLog() << line;
    }

    static std::string shellQuote(const std::string& value){
        std::string quoted = "'";
        for(char c : value){
            if(c == '\''){
                quoted += "'\\''";
            }else{
                quoted += c;
            }
        }
        quoted += "'";
        return quoted;
    }

    string scPath = "";
    scPreferences prefs;
    std::mutex preferencesMutex;
    std::atomic<bool> reboot{true};
#ifndef TARGET_WIN32
    std::atomic<pid_t> processId{-1};
#endif
};

#endif /* scStart_h */
