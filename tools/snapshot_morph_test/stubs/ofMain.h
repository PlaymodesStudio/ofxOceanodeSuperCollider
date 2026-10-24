#pragma once
#include <nlohmann/json.hpp>
#include <string>
using ofJson = nlohmann::json;
struct ofAbstractParameter { virtual ~ofAbstractParameter() = default; };
template<class T> struct ofParameter : ofAbstractParameter {
    std::string key;
    T data;
    ofParameter(std::string k, T v) : key(std::move(k)), data(std::move(v)) {}
    const T& get() const { return data; }
    std::string getEscapedName() const { return key; }
    void set(T v) { data = std::move(v); }
};
