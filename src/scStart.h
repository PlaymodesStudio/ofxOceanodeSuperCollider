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
#include <fcntl.h>
#include <poll.h>
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
            std::string ugenPaths;
            try {
                ugenPaths = scUgenPluginPaths(launchPrefs.ugensPlugins, scPath);
            } catch(const std::exception& error) {
                ofLogError("scStart") << "Cannot prepare scsynth plugin paths: " << error.what();
                break;
            }
            if(!ugenPaths.empty()){
                args.push_back("-U");
                args.push_back(ugenPaths);
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
    // scsynth's console goes to the Log window. A UGen can print the same line
    // thousands of times a second (GrainBuf's "Too many grains!" on every
    // rejected trigger), so a run of identical lines is logged once, followed
    // by a count at most once a second and when the run ends.
    struct ConsoleLines {
        std::string last;
        uint64_t repeats = 0;
        uint64_t lastSummary = 0;

        void add(const std::string& line){
            if(!last.empty() && line == last){
                if(repeats++ == 0) lastSummary = ofGetElapsedTimeMillis();
                return;
            }
            summarize();
            last = line;
            ofLog() << line;
        }
        // Called while the console is quiet too, so a flood that never ends
        // still reports its size.
        void tick(){
            if(repeats > 0 && ofGetElapsedTimeMillis() - lastSummary >= 1000) summarize();
        }
        void summarize(){
            if(repeats == 0) return;
            ofLog() << last << " (repeated " << repeats << (repeats == 1 ? " more time)" : " more times)");
            repeats = 0;
            lastSummary = ofGetElapsedTimeMillis();
        }
    };

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
        pid_t pid = -1;
        int error = 0;
        {
            // One launch at a time, and a child that inherits nothing but its
            // stdio: the servers start together, and a scsynth that inherited
            // another server's pipe kept it open after that server was stopped,
            // so its reader never saw the end and quitting the app hung
            static std::mutex launchMutex;
            std::lock_guard<std::mutex> lock(launchMutex);
            if(pipe(fds) != 0){
                ofLogError("scStart") << "Could not create a pipe for scsynth";
                return;
            }
            std::vector<char*> argv;
            for(auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
            posix_spawnattr_t attributes;
            posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
            error = posix_spawn(&pid, scPath.c_str(), &actions, &attributes, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attributes);
            close(fds[1]);
        }
        if(error != 0){
            close(fds[0]);
            ofLogError("scStart") << "Could not start " << scPath << ": " << strerror(error);
            return;
        }
        processId = pid;
        readOutput(fds[0], pid);
        close(fds[0]);
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR){}
        processId = -1;
    }

    // scsynth's console, line by line, until it exits. Polled rather than
    // blocking: if anything else still holds the pipe open, the end of the
    // process ends the reading anyway.
    void readOutput(int fd, pid_t pid){
        std::string line;
        ConsoleLines console;
        char buffer[512];
        while(true){
            console.tick();
            pollfd p{fd, POLLIN, 0};
            const int ready = poll(&p, 1, 250);
            if(ready > 0){
                const ssize_t n = read(fd, buffer, sizeof(buffer));
                if(n <= 0) break;   // end of output
                for(ssize_t i = 0; i < n; i++){
                    if(buffer[i] == '\n'){
                        console.add(line);
                        line.clear();
                    }else{
                        line += buffer[i];
                    }
                }
            }else if(ready < 0 && errno != EINTR){
                break;
            }else if(waitpid(pid, nullptr, WNOHANG) == pid){
                // Exited (and reaped here); nothing more will come
                processId = -1;
                break;
            }
        }
        if(!line.empty()) console.add(line);
        console.summarize();
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

    // scsynth's console, line by line, until it exits (Windows: a popen stream)
    void readOutput(FILE* stream){
        std::string line;
        ConsoleLines console;
        int c;
        while((c = fgetc(stream)) != EOF){
            if(c == '\n'){
                console.add(line);
                console.tick();
                line.clear();
            }else{
                line += (char)c;
            }
        }
        if(!line.empty()) console.add(line);
        console.summarize();
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
