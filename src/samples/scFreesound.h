//
//  scFreesound.h
//  ofxOceanodeSuperCollider
//
//  freesound.org inside the sample browser (header-only, like scSampleBrowser):
//  text search, audition and download of the sounds' preview files.
//
//    • Key: an API key (token), no login. Read from data/config/freesound.md
//      ("api_key: your-key", the Requesty format) or the FREESOUND_API_KEY
//      environment variable; the panel can also write that file. The key goes
//      to curl in a private config file, never in arguments or logs.
//    • Filters from Freesound's own analysis of each sound (estimates, not
//      uploader tags): Note (as MIDI numbers: a note in any octave is an OR
//      of them, the name alone matches nothing) with note_confidence >=
//      noteConfidence, Tempo, One-shot, Loop.
//    • Search runs on Enter, on a worker thread. Workers share the state
//      through a shared_ptr and never touch the panel, so a node can be
//      deleted mid-request.
//    • Files: Freesound previews are MP3/OGG and the browsers and Boxes read
//      WAV/AIFF, so a preview is fetched as MP3 (preview-hq-mp3, ~128 kbps;
//      afconvert has no OGG decoder) and converted to a 16-bit WAV.
//        audition  →  ~/Library/Caches/Oceanode/Freesound/<id>_<name>.wav
//                     (local: the data folder may be a shared one; pruned
//                     after cacheDays). Sounds over maxSeconds are refused.
//        drag / Enter / double-click  →  copied to <Samples>/Freesound/, and
//                     that durable path is what the payload and presets get.
//      Each kept sound adds a line to <Samples>/Freesound/credits.txt
//      (author, licence, link): CC-BY sounds need the attribution.
//    • Full quality: "Log in" runs Freesound's OAuth2 login (with PKCE) in
//      the user's web browser; Freesound shows a code that is pasted back
//      here. Logged in, a sound is fetched as its uploaded original
//      (<id>_<name>_hq.wav/.aif; FLAC, MP3 and M4A are converted to WAV, OGG
//      stays on the preview route). Needs the credential's Client id, kept
//      beside the key. The tokens are personal and stay on this machine:
//      ~/Library/Application Support/Oceanode/freesound_token.json (0600).
//

#pragma once

#include "ofMain.h"
#include "imgui.h"
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

namespace scFreesound {

struct Sound {
    int         id = 0;
    std::string name, username, license, type, previewUrl;
    float       duration = 0;
    std::string note;              // estimated, "F#3"; "" when unknown or unsure
    int         bpm = 0;           // estimated, 0 when unknown
};

// The page Freesound offers to apps without a web server: it shows the login
// code for the user to copy. Must be the credential's registered callback URL
// (another one can be set with "redirect_uri:" in freesound.md).
inline const char* defaultRedirectUri() { return "https://freesound.org/home/app_permissions/permission_granted/"; }

// Login state: one for the whole app (every browser's panel and the workers),
// since a token refresh replaces both tokens for everybody.
struct Auth {
    std::mutex  mutex;          // the fields below; never held across a network request
    std::mutex  refreshMutex;   // one token request at a time (each one replaces both tokens); workers only
    std::string clientId, clientSecret, redirectUri, tokenFile;
    std::string accessToken, refreshToken;
    long long   expiresAt = 0;  // seconds since the epoch
    std::string message;        // login problem to show
    bool        busy = false;   // exchanging the code
    int         version = 0;    // counts logins and logouts: each panel compares its own copy
    bool        loaded = false; // config and token file were read (main thread, first panel)
};
inline std::shared_ptr<Auth> sharedAuth() {
    static std::shared_ptr<Auth> auth = std::make_shared<Auth>();
    return auth;
}

struct Query {
    std::string text;
    float       maxDuration = 0;   // seconds, 0 = any
    bool        cc0Only = false;
    int         note = -1;         // pitch class 0 (C) .. 11 (B), -1 = any
    int         octave = -2;       // -1 .. 9 (C4 = MIDI 60), -2 = any
    int         bpm = 0;           // 0 = any
    bool        oneShot = false, loopable = false;
    int         sort = 0;          // index into sortKeys()
    int         page = 1;
};

struct SearchResult {
    std::vector<Sound> sounds;
    int         count = 0;
    bool        hasNext = false;
    std::string error;             // "" when it worked
    bool        keyRejected = false;
};

inline const std::vector<std::pair<const char*, const char*>>& sortKeys() {
    static const std::vector<std::pair<const char*, const char*>> keys = {
        {"Relevance", "score"}, {"Rating", "rating_desc"}, {"Downloads", "downloads_desc"},
        {"Newest", "created_desc"}, {"Shortest", "duration_asc"}, {"Longest", "duration_desc"}};
    return keys;
}

inline const char* noteNames(int pitchClass) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((pitchClass % 12) + 12) % 12];
}
// Below this, Freesound's note estimate is not used to filter or shown
constexpr float noteConfidence = 0.8f;

// The API returns the licence as a Creative Commons URL (older answers: a name)
inline std::string licenseLabel(const std::string& license) {
    const std::string l = ofToLower(license);
    if(l.find("zero") != std::string::npos || l.find("creative commons 0") != std::string::npos) return "CC0";
    if(l.find("by-nc") != std::string::npos || l.find("noncommercial") != std::string::npos) return "BY-NC";
    if(l.find("sampling") != std::string::npos) return "S+";
    if(l.find("/by/") != std::string::npos || l.find("attribution") != std::string::npos) return "BY";
    return "?";
}

inline std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if(first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string keyFilePath() { return ofToDataPath("config/freesound.md", true); }

// Main thread (ofToDataPath). One "name: value" line of freesound.md, unquoted.
inline std::string loadConfigValue(const std::string& name) {
    std::ifstream file(keyFilePath());
    std::string line;
    while(std::getline(file, line)) {
        line = trim(line);
        if(line.rfind(name + ":", 0) != 0) continue;
        std::string value = trim(line.substr(name.size() + 1));
        if(value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = trim(value.substr(1, value.size() - 2));
        return value;
    }
    return "";
}

inline bool isToken(const std::string& text) {
    if(text.empty()) return false;
    for(char c : text) if(!std::isalnum(static_cast<unsigned char>(c))) return false;
    return true;
}

// Main thread. "" when there is no usable key.
inline std::string loadKey() {
    std::string key = loadConfigValue("api_key");
    if(key.empty())
        if(const char* env = std::getenv("FREESOUND_API_KEY")) key = trim(env);
    // A key is one token of letters and digits; anything else is not a key
    return isToken(key) ? key : "";
}
inline std::string loadClientId() {
    const std::string id = loadConfigValue("client_id");
    return isToken(id) ? id : "";
}

// Writes the key and the client id (either may be ""), keeping a redirect_uri line
inline bool saveConfig(const std::string& key, const std::string& clientId) {
    try {
        const std::string redirect = loadConfigValue("redirect_uri");
        const std::filesystem::path path = keyFilePath();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::trunc);
        if(!key.empty())      file << "api_key: \"" << key << "\"\n";
        if(!clientId.empty()) file << "client_id: \"" << clientId << "\"\n";
        if(!redirect.empty()) file << "redirect_uri: \"" << redirect << "\"\n";
        file.close();
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        return file.good();
    } catch(const std::exception&) { return false; }
}

inline std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for(char c : value) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    return quoted + '\'';
}

inline std::string configQuote(const std::string& value) {
    std::string quoted = "\"";
    for(char c : value) {
        switch(c) {
            case '\\': quoted += "\\\\"; break;
            case '"':  quoted += "\\\""; break;
            case '\n': quoted += "\\n";  break;
            case '\r': quoted += "\\r";  break;
            case '\t': quoted += "\\t";  break;
            default:   quoted += c;
        }
    }
    return quoted + '"';
}

// Runs a shell command; its stdout, and whether it exited with status 0
inline bool runCommand(const std::string& command, std::string& output, size_t maxBytes = 8 * 1024 * 1024) {
    FILE* pipe = popen(command.c_str(), "r");
    if(!pipe) return false;
    char chunk[4096];
    size_t got;
    while((got = fread(chunk, 1, sizeof(chunk), pipe)) > 0)
        if(output.size() + got <= maxBytes) output.append(chunk, got);
    return pclose(pipe) == 0;
}

// curl with a private (0600) config file, so nothing secret is an argument.
// `config` holds everything but the write-out; the HTTP status (0: no answer)
// is returned and the answer's body goes to `body` (unless the config has an
// "output" line of its own).
inline int curlWithConfig(const std::string& config, std::string& body, int maxSeconds = 30) {
    std::string tempDir = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp/";
    if(!tempDir.empty() && tempDir.back() != '/') tempDir += '/';
    const std::string pattern = tempDir + "oceanode_freesound_XXXXXX";
    std::vector<char> filename(pattern.begin(), pattern.end());
    filename.push_back('\0');
    const int fd = mkstemp(filename.data());
    if(fd < 0) return 0;
    struct TempFile { std::string path; ~TempFile() { std::remove(path.c_str()); } } temp{filename.data()};
    const bool written = write(fd, config.data(), config.size()) == (ssize_t)config.size();
    close(fd);
    if(!written) return 0;
    std::string output;
    // A transfer cut short still reports its HTTP status: curl's own exit
    // status is what says the answer is whole (HTTP errors exit 0 here)
    if(!runCommand("/usr/bin/curl --disable -sS --proto '=https' --connect-timeout 10 --max-time " + ofToString(maxSeconds) +
                   " --config " + shellQuote(temp.path) + " --write-out '\\n%{http_code}' 2>/dev/null", output))
        return 0;
    const auto marker = output.rfind('\n');
    if(marker == std::string::npos) return 0;
    body = output.substr(0, marker);
    return std::atoi(output.c_str() + marker + 1);
}

inline std::string urlEncode(const std::string& text) {
    std::string out;
    char hex[4];
    for(char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if(std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { std::snprintf(hex, sizeof(hex), "%%%02X", u); out += hex; }
    }
    return out;
}

// 64 random characters: a PKCE code verifier, or a state value
inline std::string randomToken() {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device device;
    std::string token;
    for(int i = 0; i < 64; i++) token += chars[device() % (sizeof(chars) - 1)];
    return token;
}

// PKCE S256: base64url(SHA-256(verifier)) without padding. "" off macOS.
inline std::string pkceChallenge(const std::string& verifier) {
#ifdef __APPLE__
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(verifier.data(), (CC_LONG)verifier.size(), digest);
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for(int i = 0; i < CC_SHA256_DIGEST_LENGTH; i += 3) {
        const int left = CC_SHA256_DIGEST_LENGTH - i;
        const unsigned v = (digest[i] << 16) | (left > 1 ? digest[i + 1] << 8 : 0) | (left > 2 ? digest[i + 2] : 0);
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        if(left > 1) out += table[(v >> 6) & 63];
        if(left > 2) out += table[v & 63];
    }
    return out;
#else
    return "";
#endif
}

inline std::string authorizeUrl(const Auth& auth, const std::string& verifier, const std::string& stateValue) {
    return "https://freesound.org/apiv2/oauth2/authorize/?client_id=" + urlEncode(auth.clientId) +
           "&response_type=code&redirect_uri=" + urlEncode(auth.redirectUri) +
           "&code_challenge=" + pkceChallenge(verifier) + "&code_challenge_method=S256&state=" + stateValue;
}

inline void saveTokens(const Auth& auth) {
    try {
        const std::filesystem::path path = auth.tokenFile;
        std::filesystem::create_directories(path.parent_path());
        if(auth.refreshToken.empty()) { std::filesystem::remove(path); return; }
        ofJson json = {{"access_token", auth.accessToken}, {"refresh_token", auth.refreshToken}, {"expires_at", auth.expiresAt}};
        { std::ofstream file(path, std::ios::trunc); }
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        std::ofstream file(path, std::ios::trunc);
        file << json.dump();
    } catch(const std::exception&) {}
}

inline void loadTokens(Auth& auth) {
    std::ifstream file(auth.tokenFile);
    const ofJson json = ofJson::parse(file, nullptr, false);
    if(!json.is_object()) return;
    if(json.contains("access_token") && json["access_token"].is_string())   auth.accessToken  = json["access_token"].get<std::string>();
    if(json.contains("refresh_token") && json["refresh_token"].is_string()) auth.refreshToken = json["refresh_token"].get<std::string>();
    if(json.contains("expires_at") && json["expires_at"].is_number())       auth.expiresAt    = json["expires_at"].get<long long>();
}

// Token endpoint, for a login code (with its verifier) or a refresh. Worker
// thread, with auth.refreshMutex held and auth.mutex not. "" when the tokens
// were replaced and saved.
inline std::string requestTokens(Auth& auth, const std::string& code, const std::string& verifier, int* httpStatus = nullptr) {
    std::string clientId, clientSecret, redirectUri, refreshToken;
    {
        std::lock_guard<std::mutex> lock(auth.mutex);
        clientId = auth.clientId; clientSecret = auth.clientSecret; redirectUri = auth.redirectUri; refreshToken = auth.refreshToken;
    }
    std::string config = "url = \"https://freesound.org/apiv2/oauth2/access_token/\"\n";
    config += "data-urlencode = " + configQuote("client_id=" + clientId) + '\n';
    if(!clientSecret.empty()) config += "data-urlencode = " + configQuote("client_secret=" + clientSecret) + '\n';
    if(!code.empty()) {
        config += "data-urlencode = \"grant_type=authorization_code\"\n";
        config += "data-urlencode = " + configQuote("code=" + code) + '\n';
        config += "data-urlencode = " + configQuote("redirect_uri=" + redirectUri) + '\n';
        config += "data-urlencode = " + configQuote("code_verifier=" + verifier) + '\n';
    } else {
        config += "data-urlencode = \"grant_type=refresh_token\"\n";
        config += "data-urlencode = " + configQuote("refresh_token=" + refreshToken) + '\n';
    }
    std::string body;
    const int status = curlWithConfig(config, body);
    if(httpStatus) *httpStatus = status;
    if(status == 0) return "No answer from Freesound. Check your connection.";
    const ofJson json = ofJson::parse(body, nullptr, false);
    auto text = [&](const char* field) {
        return json.is_object() && json.contains(field) && json[field].is_string() ? json[field].get<std::string>() : std::string();
    };
    if(status != 200 || text("access_token").empty()) {
        std::string why = text("error_description");
        if(why.empty()) why = text("error");
        if(why.empty()) why = text("detail");
        return "Freesound refused the login (HTTP " + ofToString(status) + (why.empty() ? "" : ": " + why) + ").";
    }
    std::lock_guard<std::mutex> lock(auth.mutex);
    auth.accessToken = text("access_token");
    if(!text("refresh_token").empty()) auth.refreshToken = text("refresh_token");
    long long seconds = 3600;
    if(json.contains("expires_in") && json["expires_in"].is_number()) seconds = json["expires_in"].get<long long>();
    auth.expiresAt = (long long)std::time(nullptr) + seconds;
    auth.version++;
    saveTokens(auth);
    return "";
}

// Any thread. A usable access token, refreshed when it is about to expire
// (or when `force`: the server said it was no good). "" = not logged in.
inline std::string accessToken(Auth& auth, bool force = false) {
    std::lock_guard<std::mutex> oneAtATime(auth.refreshMutex);
    bool refresh;
    {
        std::lock_guard<std::mutex> lock(auth.mutex);
        if(auth.refreshToken.empty()) return "";
        refresh = force || auth.accessToken.empty() || (long long)std::time(nullptr) > auth.expiresAt - 120;
    }
    if(refresh) {
        int status = 0;
        const std::string error = requestTokens(auth, "", "", &status);
        if(!error.empty()) {
            // A refused refresh token is gone for good: log in again. Any
            // other failure (no network, limits, server trouble) keeps the login.
            if(status == 400 || status == 401) {
                std::lock_guard<std::mutex> lock(auth.mutex);
                auth.accessToken.clear(); auth.refreshToken.clear(); auth.version++;
                auth.message = "The Freesound login expired. Log in again.";
                saveTokens(auth);
            }
            return "";
        }
    }
    std::lock_guard<std::mutex> lock(auth.mutex);
    return auth.accessToken;
}

// Where a sound's cached file goes. Logged in, wav/aif keep their format and
// FLAC, MP3 and M4A become WAV; OGG (no decoder) keeps the preview route.
inline bool originalWanted(const Sound& sound, bool loggedIn) {
    const std::string type = ofToLower(sound.type);
    return loggedIn && (type == "wav" || type == "aif" || type == "aiff" || type == "flac" || type == "mp3" || type == "m4a");
}
inline std::string cacheFileName(const Sound& sound, bool loggedIn);

// <id>_<name>: Freesound names are free text (slashes, dots, any length)
inline std::string fileStem(const Sound& sound) {
    std::string base = sound.name;
    const auto dot = base.find_last_of('.');
    if(dot != std::string::npos && base.size() - dot <= 5) base.erase(dot);   // "kick.wav"
    std::string clean;
    for(char c : base) {
        const unsigned char u = static_cast<unsigned char>(c);
        if(std::isalnum(u) || c == '-' || c == '_') clean += c;
        else if(!clean.empty() && clean.back() != '_') clean += '_';
        if(clean.size() >= 60) break;
    }
    while(!clean.empty() && clean.back() == '_') clean.pop_back();
    return ofToString(sound.id) + (clean.empty() ? "" : "_" + clean);
}

inline std::string cacheFileName(const Sound& sound, bool loggedIn) {
    if(!originalWanted(sound, loggedIn)) return fileStem(sound) + ".wav";
    const std::string type = ofToLower(sound.type);
    return fileStem(sound) + "_hq" + (type == "aif" || type == "aiff" ? ".aif" : ".wav");
}

// Search answer (JSON text + HTTP status) → result. Separate from the
// transport so it can be tested from a saved answer.
inline SearchResult parseSearch(const std::string& body, int status) {
    SearchResult result;
    const ofJson json = ofJson::parse(body, nullptr, false);
    if(status != 200) {
        if(status == 401 || status == 403) { result.error = "Freesound rejected the API key."; result.keyRejected = true; }
        else if(status == 429) result.error = "Freesound request limit reached. Try again in a minute.";
        else if(status == 0) result.error = "No answer from Freesound. Check your connection.";
        else {
            result.error = "Freesound HTTP " + ofToString(status);
            if(json.is_object() && json.contains("detail") && json["detail"].is_string())
                result.error += ": " + json["detail"].get<std::string>();
        }
        return result;
    }
    if(!json.is_object() || !json.contains("results") || !json["results"].is_array()) {
        result.error = "Freesound sent an answer that could not be read.";
        return result;
    }
    if(json.contains("count") && json["count"].is_number()) result.count = json["count"].get<int>();
    result.hasNext = json.contains("next") && json["next"].is_string();
    auto text = [](const ofJson& item, const char* field) {
        return item.contains(field) && item[field].is_string() ? item[field].get<std::string>() : std::string();
    };
    for(const auto& item : json["results"]) {
        if(!item.is_object() || !item.contains("id") || !item["id"].is_number_integer()) continue;
        Sound sound;
        sound.id       = item["id"].get<int>();
        sound.name     = text(item, "name");
        sound.username = text(item, "username");
        sound.license  = text(item, "license");
        sound.type     = text(item, "type");
        if(item.contains("duration") && item["duration"].is_number()) sound.duration = item["duration"].get<float>();
        if(item.contains("previews") && item["previews"].is_object())
            sound.previewUrl = text(item["previews"], "preview-hq-mp3");
        if(item.contains("note_confidence") && item["note_confidence"].is_number() &&
           item["note_confidence"].get<float>() >= noteConfidence) sound.note = text(item, "note_name");
        if(item.contains("bpm") && item["bpm"].is_number()) sound.bpm = (int)item["bpm"].get<float>();
        if(sound.name.empty()) sound.name = "sound " + ofToString(sound.id);
        result.sounds.push_back(std::move(sound));
    }
    return result;
}

// Any thread, blocking (up to ~30 s).
inline SearchResult search(const std::string& key, const Query& query) {
    std::string filter;
    if(query.maxDuration > 0) filter += "duration:[0 TO " + ofToString(query.maxDuration, 2) + "]";
    auto add = [&](const std::string& term) { filter += (filter.empty() ? "" : " ") + term; };
    if(query.cc0Only) add("license:\"Creative Commons 0\"");
    if(query.note >= 0) {
        // C4 = 60. One octave: one number; any octave: every octave's number.
        std::string numbers;
        for(int octave = -1; octave <= 9; octave++) {
            const int midi = 12 * (octave + 1) + query.note;
            if(midi > 127 || (query.octave > -2 && octave != query.octave)) continue;
            numbers += (numbers.empty() ? "" : " OR ") + ofToString(midi);
        }
        if(!numbers.empty()) add("note_midi:(" + numbers + ") note_confidence:[" + ofToString(noteConfidence, 2) + " TO *]");
    }
    if(query.bpm > 0) add("bpm:[" + ofToString(query.bpm - 1) + " TO " + ofToString(query.bpm + 1) + "]");
    if(query.oneShot)  add("single_event:true");
    if(query.loopable) add("loopable:true");
    const int sort = ofClamp(query.sort, 0, (int)sortKeys().size() - 1);

    std::string config = "url = \"https://freesound.org/apiv2/search/\"\nget\n";
    config += "header = " + configQuote("Authorization: Token " + key) + '\n';
    config += "data-urlencode = " + configQuote("query=" + query.text) + '\n';
    if(!filter.empty()) config += "data-urlencode = " + configQuote("filter=" + filter) + '\n';
    config += "data-urlencode = \"fields=id,name,username,license,duration,type,previews,note_name,note_confidence,bpm\"\n";
    config += "data-urlencode = " + configQuote(std::string("sort=") + sortKeys()[sort].second) + '\n';
    config += "data-urlencode = \"page_size=50\"\n";
    config += "data-urlencode = " + configQuote("page=" + ofToString(std::max(1, query.page))) + '\n';
    std::string body;
    const int status = curlWithConfig(config, body);
    return parseSearch(body, status);
}

// Any thread, blocking: preview MP3 → `wavPath`. "" when it worked.
inline std::string fetchPreview(const Sound& sound, const std::string& wavPath) {
    namespace fs = std::filesystem;
    // The URL comes from the network: only Freesound's own hosts, over https
    const bool trusted = sound.previewUrl.rfind("https://cdn.freesound.org/", 0) == 0 ||
                         sound.previewUrl.rfind("https://freesound.org/", 0) == 0;
    if(!trusted) return "This sound has no preview file.";
    std::error_code ignored;
    fs::create_directories(fs::path(wavPath).parent_path(), ignored);
    const std::string mp3 = wavPath + ".part.mp3", partWav = wavPath + ".part.wav";
    struct Cleanup { std::string a, b; ~Cleanup() { std::remove(a.c_str()); std::remove(b.c_str()); } } cleanup{mp3, partWav};
    std::string output;
    if(!runCommand("/usr/bin/curl --disable -sSfL --proto '=https' --connect-timeout 10 --max-time 240 --max-filesize 16000000 -o " +
                   shellQuote(mp3) + " " + shellQuote(sound.previewUrl) + " 2>/dev/null", output))
        return "The download failed.";
    if(!runCommand("/usr/bin/afconvert -f WAVE -d LEI16 " + shellQuote(mp3) + " " + shellQuote(partWav) + " 2>/dev/null", output))
        return "The preview could not be converted to WAV.";
    fs::rename(partWav, wavPath, ignored);   // complete or absent, never half a file
    return ignored ? "The file could not be written." : "";
}

// Any thread, blocking: the uploaded original → `path` (.wav or .aif, see
// cacheFileName). Needs the login. "" when it worked.
inline std::string fetchOriginal(const Sound& sound, const std::string& path, Auth& auth) {
    namespace fs = std::filesystem;
    std::error_code ignored;
    fs::create_directories(fs::path(path).parent_path(), ignored);
    const std::string type = ofToLower(sound.type);
    const bool convert = type == "flac" || type == "mp3" || type == "m4a";
    // afconvert picks its decoder from the extension
    const std::string part = path + ".part." + type, partWav = path + ".part.wav";
    struct Cleanup { std::string a, b; ~Cleanup() { std::remove(a.c_str()); std::remove(b.c_str()); } } cleanup{part, partWav};
    int status = 0;
    for(int attempt = 0; attempt < 2; attempt++) {
        const std::string token = accessToken(auth, attempt == 1);
        if(token.empty()) return "Not logged in to Freesound.";
        std::string config = "url = " + configQuote("https://freesound.org/apiv2/sounds/" + ofToString(sound.id) + "/download/") + '\n';
        config += "header = " + configQuote("Authorization: Bearer " + token) + '\n';
        config += "location\nmax-filesize = 400000000\n";
        config += "output = " + configQuote(part) + '\n';
        std::string body;
        status = curlWithConfig(config, body, 600);
        if(status != 401) break;   // 401: the token went stale early, refresh once
    }
    if(status == 429) return "Freesound download limit reached. Try again later.";
    if(status == 404) return "Freesound no longer has this sound.";
    if(status != 200) return status == 0 ? "The download failed." : "The download failed (HTTP " + ofToString(status) + ").";
    if(convert) {
        std::string output;
        // 24 bit keeps what a FLAC can hold; for MP3/M4A it only costs disk
        const std::string format = type == "flac" ? "LEI24" : "LEI16";
        if(!runCommand("/usr/bin/afconvert -f WAVE -d " + format + " " + shellQuote(part) + " " + shellQuote(partWav) + " 2>/dev/null", output))
            return "The file could not be converted to WAV.";
        fs::rename(partWav, path, ignored);
    } else {
        fs::rename(part, path, ignored);
    }
    return ignored ? "The file could not be written." : "";
}

// ─────────────────────────────────────────────────────────────────────────────
// The panel: owned by scSampleBrowser, drawn in place of the folder list.
// ─────────────────────────────────────────────────────────────────────────────
class Panel {
public:
    // What the panel needs from its browser
    struct Host {
        std::function<void(const std::string&)> preview;      // play a local file
        std::function<void()>                   stopPreview;
        std::function<void(const std::string&)> activate;     // may be empty
        std::string                             payloadType;
    };

    int   cacheDays  = 7;
    float maxSeconds = 300;   // longer sounds are listed but not fetched

    // `samplesDir`: the folder that gets the Freesound/ subfolder
    void draw(const Host& host, const std::string& samplesDir) {
        if(!started) start(samplesDir);

        if(key.empty()) { drawKeySetup(); return; }

        // ── Search bar ───────────────────────────────────────────────────────
        ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Search").x
                                                - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x));
        bool go = ImGui::InputTextWithHint("##fsQuery", "search freesound.org", queryBuf, sizeof(queryBuf),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        go |= ImGui::Button("Search");

        const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
        ImGui::SetNextItemWidth(std::max(30.0f, third));
        if(ImGui::BeginCombo("##fsSort", sortKeys()[query.sort].first)) {
            for(int i = 0; i < (int)sortKeys().size(); i++)
                if(ImGui::Selectable(sortKeys()[i].first, i == query.sort)) { query.sort = i; go = true; }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Order of the results");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(30.0f, third));
        ImGui::DragFloat("##fsMax", &query.maxDuration, 0.1f, 0.0f, 600.0f, query.maxDuration > 0 ? "max %.1f s" : "any length");
        if(ImGui::IsItemDeactivatedAfterEdit()) go = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Longest sound to list, in seconds (0 = any length)");
        ImGui::SameLine();
        if(ImGui::Checkbox("CC0", &query.cc0Only)) go = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Only public-domain sounds (no attribution needed)");

        // Second row: what Freesound's analysis estimated for each sound
        ImGui::SetNextItemWidth(std::max(30.0f, third));
        if(ImGui::BeginCombo("##fsNote", query.note < 0 ? "any note" : noteNames(query.note))) {
            if(ImGui::Selectable("any note", query.note < 0)) { query.note = -1; go = true; }
            for(int i = 0; i < 12; i++)
                if(ImGui::Selectable(noteNames(i), i == query.note)) { query.note = i; go = true; }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Only sounds Freesound hears as this note.\nAn estimate: good on single sustained notes, poor on chords and noise.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(30.0f, third));
        const std::string octaveLabel = query.octave < -1 ? std::string("any oct") : "oct " + ofToString(query.octave);
        ImGui::BeginDisabled(query.note < 0);
        if(ImGui::BeginCombo("##fsOct", octaveLabel.c_str())) {
            if(ImGui::Selectable("any oct", query.octave < -1)) { query.octave = -2; go = true; }
            for(int i = 0; i <= 8; i++)
                if(ImGui::Selectable(("oct " + ofToString(i)).c_str(), i == query.octave)) { query.octave = i; go = true; }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Octave of the note (middle C is C4). Pick a note first.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::DragInt("##fsBpm", &query.bpm, 0.5f, 0, 300, query.bpm > 0 ? "%d bpm" : "any bpm");
        if(ImGui::IsItemDeactivatedAfterEdit()) go = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Only sounds at about this tempo, as estimated by Freesound (0 = any)");
        if(ImGui::Checkbox("One-shot", &query.oneShot)) go = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Only sounds with a single event (one hit, one note)");
        ImGui::SameLine();
        if(ImGui::Checkbox("Loop", &query.loopable)) go = true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Only sounds that repeat smoothly");

        drawLogin();

        if(go) startSearch(1);

        // ── Take what the workers produced ───────────────────────────────────
        std::string status;
        bool searching, hasNext;
        int count;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            status = state->status; searching = state->searching; hasNext = state->hasNext; count = state->count;
            if(state->resultsChanged) {
                results = state->results;
                state->resultsChanged = false;
                if(state->page == 1) selected = -1;
                page = state->page;   // the last page that did load
            }
            fetch = state->fetch;
            if(!state->fetchError.empty()) { status = state->fetchError; }
            if(state->keyRejected) {
                // Back to the key screen, or a mistyped key could only be fixed by hand
                state->keyRejected = false;
                state->status.clear();
                key.clear();
                keyMessage = "Freesound rejected that API key. Paste it again.";
            }
        }
        if(key.empty()) return;
        if(searching) ImGui::TextDisabled("Searching...");
        else if(!status.empty()) ImGui::TextWrapped("%s", status.c_str());
        else if(!results.empty()) ImGui::TextDisabled("%d of %d sounds", (int)results.size(), count);
        else ImGui::TextDisabled("Click a sound to hear it, drag it to use it");
        ImGui::Separator();

        // An arrow key held down passes many rows: fetch only where it stops
        if(pendingPlay > 0 && fetchState(pendingPlay) == NotFetched && ofGetElapsedTimef() - pendingSince > 0.25f)
            for(auto& s : results) if(s.id == pendingPlay) { startFetch(s); break; }
        // A sound asked for plays as soon as its file is there
        if(pendingPlay > 0 && fetchState(pendingPlay) == Ready) {
            for(auto& s : results) if(s.id == pendingPlay && host.preview) host.preview(cachePath(s));
            pendingPlay = 0;
        }

        // ── Results ──────────────────────────────────────────────────────────
        ImGui::BeginChild("##fsList", ImVec2(0, 0), false);
        const int n = (int)results.size();
        selected = std::min(selected, n - 1);
        std::string activatePath;
        bool scrollToSelected = false;
        if((ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) && !ImGui::GetIO().WantTextInput && n > 0) {
            int step = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
            if(step != 0) {
                selected = ofClamp(selected + step, 0, n - 1);
                audition(results[selected], host, true);
                scrollToSelected = true;
            }
            if(ImGui::IsKeyPressed(ImGuiKey_Enter) && selected >= 0 && host.activate && fetchState(results[selected].id) == Ready)
                activatePath = keep(results[selected]);
        }

        ImGuiListClipper clipper;
        clipper.Begin(n);
        if(scrollToSelected && selected >= 0) clipper.IncludeItemByIndex(selected);
        while(clipper.Step()) {
            for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const Sound& s = results[i];
                const int st = fetchState(s.id);
                ImGui::PushID(s.id);
                const char* mark = st == Ready ? " * " : st == Fetching ? "..." : st == Failed ? " ! " : "   ";
                const std::string label = std::string(mark) + " " + s.name;
                if(ImGui::Selectable(label.c_str(), i == selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected = i;
                    audition(s, host);
                    if(host.activate && st == Ready && ImGui::IsMouseDoubleClicked(0)) activatePath = keep(s);
                }
                if(scrollToSelected && i == selected) ImGui::SetScrollHereY(0.5f);
                // Read before the drag tooltip below, which replaces the "last item"
                const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
                const bool rowHovered = ImGui::IsItemHovered();

                if(ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    if(st == Ready) {
                        const std::string path = keep(s);
                        if(!path.empty()) ImGui::SetDragDropPayload(host.payloadType.c_str(), path.c_str(), path.size() + 1);
                        ImGui::TextUnformatted(("  " + s.name).c_str());
                    } else {
                        // Not on disk yet: fetch it, the next drag will carry it
                        if(st == NotFetched) startFetch(s);
                        ImGui::TextUnformatted("Downloading: drag again in a moment");
                    }
                    ImGui::EndDragDropSource();
                } else if(rowHovered) {
                    std::string analysis;
                if(!s.note.empty()) analysis += "   note " + s.note;
                if(s.bpm > 0) analysis += "   " + ofToString(s.bpm) + " bpm";
                ImGui::SetTooltip("%s\n%s   %s   by %s   (%s, %s)%s%s%s", s.name.c_str(), formatDuration(s.duration).c_str(),
                                      licenseLabel(s.license).c_str(), s.username.c_str(), s.type.c_str(),
                                      originalWanted(s, loggedIn) ? "original file" : "preview quality",
                                      analysis.empty() ? "" : "\nFreesound's estimate:", analysis.c_str(),
                                      st == Failed ? "\nThe download failed: click to try again" : "");
                }
                // Length and licence at the right edge of the row
                const std::string info = (s.note.empty() ? "" : s.note + " ") + formatDuration(s.duration) + " " + licenseLabel(s.license);
                const float infoW = ImGui::CalcTextSize(info.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(rowMax.x - infoW - 4, rowMin.y), rowMax,
                                  ImGui::GetColorU32(rowHovered ? ImGuiCol_HeaderHovered
                                                     : i == selected ? ImGuiCol_Header : ImGuiCol_WindowBg));
                dl->AddText(ImVec2(rowMax.x - infoW, rowMin.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), info.c_str());
                ImGui::PopID();
            }
        }
        clipper.End();
        if(hasNext && !searching && n > 0 && ImGui::Button("More results", ImVec2(-1, 0))) startSearch(page + 1);
        ImGui::EndChild();

        if(!activatePath.empty() && host.activate) host.activate(activatePath);
    }

private:
    enum { NotFetched = 0, Fetching, Ready, Failed };

    struct State {
        std::mutex         mutex;
        bool               searching = false, resultsChanged = false, hasNext = false;
        int                generation = 0, count = 0, page = 1;
        bool               keyRejected = false;
        std::string        status, fetchError;
        std::vector<Sound> results;
        std::map<int, int> fetch;   // sound id → NotFetched..Failed
    };

    std::shared_ptr<State> state = std::make_shared<State>();
    std::shared_ptr<Auth>  auth  = sharedAuth();
    int                authVersion = -1;      // auth->version this panel has acted on
    bool               loggedIn = false;      // main-thread copy
    bool               loginOpen = false;     // the login steps are shown
    char               clientIdBuf[128] = "";
    char               codeBuf[256] = "";
    std::string        loginVerifier, loginMessage;
    bool               started = false;
    std::string        key, keepDir, cacheDir;
    char               queryBuf[256] = "";
    char               keyBuf[128] = "";
    std::string        keyMessage;
    Query              query;
    int                page = 1;
    std::vector<Sound> results;     // main-thread copies
    std::map<int, int> fetch;
    int                selected = -1;
    int                pendingPlay = 0;   // sound id to play once fetched
    float              pendingSince = 0;

    static std::string formatDuration(float seconds) {
        char text[32];
        if(seconds < 60) std::snprintf(text, sizeof(text), "%.1fs", seconds);
        else std::snprintf(text, sizeof(text), "%d:%02d", (int)seconds / 60, (int)seconds % 60);
        return text;
    }

    int fetchState(int id) const {
        const auto it = fetch.find(id);
        return it == fetch.end() ? NotFetched : it->second;
    }
    std::string cachePath(const Sound& s) const { return cacheDir + "/" + cacheFileName(s, loggedIn); }

    // Logged in or out: the cached file of every sound is another one now
    void quality(bool nowLoggedIn) {
        loggedIn = nowLoggedIn;
        pendingPlay = 0;
        std::map<int, int> known;
        std::error_code ignored;
        for(auto& s : results) if(std::filesystem::exists(cachePath(s), ignored)) known[s.id] = Ready;
        std::lock_guard<std::mutex> lock(state->mutex);
        for(auto& f : state->fetch) if(f.second == Fetching) known[f.first] = Fetching;
        state->fetch = known;
        fetch = known;
    }

    // One line under the search bar; "Log in" opens the three steps
    void drawLogin() {
        std::string message;
        bool busy;
        {
            std::lock_guard<std::mutex> lock(auth->mutex);
            if(auth->version != authVersion) {
                authVersion = auth->version;
                const bool now = !auth->refreshToken.empty();
                if(now) { loginOpen = false; loginVerifier.clear(); std::memset(codeBuf, 0, sizeof(codeBuf)); }
                if(now != loggedIn) quality(now);
            }
            message = auth->message; busy = auth->busy;
        }
        if(loggedIn) {
            ImGui::TextDisabled("Full quality (logged in)");
            ImGui::SameLine();
            if(ImGui::SmallButton("Log out")) {
                {
                    std::lock_guard<std::mutex> lock(auth->mutex);
                    auth->accessToken.clear(); auth->refreshToken.clear(); auth->expiresAt = 0; auth->message.clear();
                    authVersion = ++auth->version;
                    saveTokens(*auth);
                }
                quality(false);
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Forget the Freesound login on this machine.\nSounds are then the site's preview quality again.");
            return;
        }
        ImGui::TextDisabled("Preview quality");
        ImGui::SameLine();
        if(ImGui::SmallButton(loginOpen ? "Cancel" : "Log in for full quality")) { loginOpen = !loginOpen; loginMessage.clear(); }
        if(!loginOpen) { if(!message.empty()) ImGui::TextWrapped("%s", message.c_str()); return; }

        if(auth->clientId.empty()) {
            ImGui::TextWrapped("Paste the \"Client id\" of your credential (same table as the key, at freesound.org/apiv2/apply).");
            ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Save").x
                                                    - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x));
            ImGui::InputTextWithHint("##fsClientId", "Client id", clientIdBuf, sizeof(clientIdBuf));
            ImGui::SameLine();
            if(ImGui::Button("Save")) {
                const std::string typed = trim(clientIdBuf);
                if(!isToken(typed)) loginMessage = "A client id is one word of letters and digits.";
                else if(!saveConfig(loadConfigValue("api_key"), typed)) loginMessage = "Could not write data/config/freesound.md.";
                else { std::lock_guard<std::mutex> lock(auth->mutex); auth->clientId = typed; loginMessage.clear(); }
            }
        } else {
            if(ImGui::Button("1. Open the Freesound login page", ImVec2(-1, 0))) {
                loginVerifier = randomToken();
                std::string url;
                { std::lock_guard<std::mutex> lock(auth->mutex); auth->message.clear(); url = authorizeUrl(*auth, loginVerifier, randomToken()); }
                std::memset(codeBuf, 0, sizeof(codeBuf));
                ofLaunchBrowser(url);
                loginMessage.clear();
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Opens freesound.org in your web browser. Log in there and press Authorize:\nFreesound then shows a code (valid for 10 minutes).");
            ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Finish").x
                                                    - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x));
            ImGui::InputTextWithHint("##fsCode", "2. paste the code shown", codeBuf, sizeof(codeBuf));
            ImGui::SameLine();
            if(ImGui::Button("Finish") && !busy) {
                const std::string code = trim(codeBuf);
                if(loginVerifier.empty()) loginMessage = "Open the login page first (step 1).";
                else if(!isToken(code)) loginMessage = "Paste the code Freesound shows after you authorize.";
                else {
                    { std::lock_guard<std::mutex> lock(auth->mutex); auth->busy = true; auth->message.clear(); }
                    loginMessage.clear();
                    std::thread([auth = auth, code, verifier = loginVerifier]() {
                        std::lock_guard<std::mutex> oneAtATime(auth->refreshMutex);
                        const std::string error = requestTokens(*auth, code, verifier);
                        std::lock_guard<std::mutex> lock(auth->mutex);
                        auth->message = error;
                        auth->busy = false;
                    }).detach();
                }
            }
            if(busy) ImGui::TextDisabled("Logging in...");
            // Freesound refuses the login unless this is the credential's callback URL, to the letter
            ImGui::TextDisabled("Callback URL of the credential:");
            ImGui::TextWrapped("%s", auth->redirectUri.c_str());
        }
        if(!loginMessage.empty()) ImGui::TextWrapped("%s", loginMessage.c_str());
        else if(!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    }

    void start(const std::string& samplesDir) {
        namespace fs = std::filesystem;
        started = true;
        key = loadKey();
        keepDir  = (fs::path(samplesDir) / "Freesound").string();
        cacheDir = (fs::path(ofFilePath::getUserHomeDir()) / "Library/Caches/Oceanode/Freesound").string();
        {
            std::lock_guard<std::mutex> lock(auth->mutex);
            if(!auth->loaded) {
                auth->loaded = true;
                auth->clientId     = loadClientId();
                auth->clientSecret = key;
                auth->redirectUri  = loadConfigValue("redirect_uri");
                const bool usable = auth->redirectUri.rfind("https://", 0) == 0 || auth->redirectUri.rfind("http://localhost", 0) == 0 ||
                                    auth->redirectUri.rfind("http://127.0.0.1", 0) == 0;
                if(!usable) auth->redirectUri = defaultRedirectUri();
                auth->tokenFile = (fs::path(ofFilePath::getUserHomeDir()) / "Library/Application Support/Oceanode/freesound_token.json").string();
                loadTokens(*auth);
            }
            authVersion = auth->version;
            loggedIn = !auth->refreshToken.empty();
        }
        // Auditioned and never used: gone after cacheDays
        std::error_code ignored;
        if(fs::is_directory(cacheDir, ignored)) {
            const auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * cacheDays);
            std::vector<fs::path> old;
            for(auto& e : fs::directory_iterator(cacheDir, ignored))
                if(e.is_regular_file(ignored) && e.last_write_time(ignored) < limit) old.push_back(e.path());
            for(auto& p : old) fs::remove(p, ignored);
        }
    }

    void drawKeySetup() {
        ImGui::TextWrapped("Freesound needs a free API key. Create one at freesound.org/apiv2/apply "
                           "(log in, fill the form) and paste the \"Client secret/Api key\" here.");
        if(ImGui::Button("Open freesound.org/apiv2/apply")) ofLaunchBrowser("https://freesound.org/apiv2/apply");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##fsKey", "API key", keyBuf, sizeof(keyBuf), ImGuiInputTextFlags_Password);
        if(ImGui::Button("Save key")) {
            const std::string typed = trim(keyBuf);
            bool valid = !typed.empty();
            for(char c : typed) valid = valid && std::isalnum(static_cast<unsigned char>(c));
            if(!valid) keyMessage = "A key is one word of letters and digits.";
            else if(!saveConfig(typed, loadClientId())) keyMessage = "Could not write data/config/freesound.md.";
            else {
                key = loadKey(); keyMessage.clear(); std::memset(keyBuf, 0, sizeof(keyBuf));
                std::lock_guard<std::mutex> lock(auth->mutex);
                auth->clientSecret = key;
            }
        }
        ImGui::SameLine();
        if(ImGui::Button("Reload")) key = loadKey();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Read data/config/freesound.md again");
        if(!keyMessage.empty()) ImGui::TextWrapped("%s", keyMessage.c_str());
        ImGui::TextDisabled("Stored in data/config/freesound.md");
    }

    void startSearch(int wantedPage) {
        query.text = trim(queryBuf);
        query.page = wantedPage;
        int generation;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            generation = ++state->generation;
            state->searching = true;
            state->status.clear();
            state->fetchError.clear();
        }
        // Which of these are already on disk is looked up on the worker too
        std::thread([state = state, key = key, query = query, cacheDir = cacheDir, loggedIn = loggedIn, generation]() {
            SearchResult result = search(key, query);
            std::map<int, int> known;
            std::error_code ignored;
            for(auto& s : result.sounds)
                if(std::filesystem::exists(cacheDir + "/" + cacheFileName(s, loggedIn), ignored)) known[s.id] = Ready;
            std::lock_guard<std::mutex> lock(state->mutex);
            if(generation != state->generation) return;   // a newer search replaced this one
            state->searching = false;
            state->status = result.error;
            state->keyRejected = result.keyRejected;
            if(!result.error.empty()) return;
            if(query.page <= 1) state->results.clear();
            state->results.insert(state->results.end(), result.sounds.begin(), result.sounds.end());
            for(auto& k : known) if(state->fetch[k.first] != Fetching) state->fetch[k.first] = Ready;
            state->count = result.count;
            state->hasNext = result.hasNext;
            state->page = query.page;
            state->resultsChanged = true;
            if(state->results.empty()) state->status = "No sounds found.";
        }).detach();
    }

    void startFetch(const Sound& sound) {
        if(sound.duration > maxSeconds) {
            if(pendingPlay == sound.id) pendingPlay = 0;
            std::lock_guard<std::mutex> lock(state->mutex);
            state->fetchError = sound.name + ": longer than " + ofToString((int)(maxSeconds / 60)) + " minutes, not downloaded.";
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if(state->fetch[sound.id] == Fetching) return;
            state->fetch[sound.id] = Fetching;
            state->fetchError.clear();
        }
        fetch[sound.id] = Fetching;
        std::thread([state = state, auth = auth, sound, path = cachePath(sound), original = originalWanted(sound, loggedIn)]() {
            const std::string error = original ? fetchOriginal(sound, path, *auth) : fetchPreview(sound, path);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->fetch[sound.id] = error.empty() ? Ready : Failed;
            if(!error.empty()) state->fetchError = sound.name + ": " + error;
        }).detach();
    }

    // Click or arrow key: play it, fetching it first when needed
    // `delayed`: from the keyboard, the fetch waits until the selection rests
    void audition(const Sound& sound, const Host& host, bool delayed = false) {
        const int st = fetchState(sound.id);
        if(st == Ready) {
            pendingPlay = 0;
            if(host.preview) host.preview(cachePath(sound));
        } else {
            if(host.stopPreview) host.stopPreview();
            pendingPlay = sound.id;
            pendingSince = ofGetElapsedTimef();
            if(st != Fetching && !delayed) startFetch(sound);
        }
    }

    // The sound is being used: copy it out of the cache to its durable place
    // and note the credit. Returns that path ("" if the copy failed).
    std::string keep(const Sound& sound) {
        namespace fs = std::filesystem;
        const std::string file = cacheFileName(sound, loggedIn);
        const std::string dest = keepDir + "/" + file;
        std::error_code error;
        if(fs::exists(dest, error)) return dest;
        fs::create_directories(keepDir, error);   // first sound ever kept
        fs::copy_file(cachePath(sound), dest, error);
        if(error) {
            ofLogError("scFreesound") << "keep " << dest << ": " << error.message();
            std::lock_guard<std::mutex> lock(state->mutex);
            state->fetchError = "Could not copy the sound into " + keepDir;
            return "";
        }
        std::ofstream credits(keepDir + "/credits.txt", std::ios::app);
        credits << file << " | \"" << sound.name << "\" by " << sound.username
                << " | " << licenseLabel(sound.license) << " " << sound.license
                << " | https://freesound.org/s/" << sound.id << "/\n";
        return dest;
    }
};

} // namespace scFreesound
