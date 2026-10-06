#pragma once
#include "Arduino.h"
#include "WiFiClient.h"
#include <functional>
#include <map>
#include <stdexcept>

enum HTTPMethod { HTTP_GET, HTTP_POST };
class WebServer {
public:
    void on(const char* path, HTTPMethod method, std::function<void()> callback) {
        routes_[{path, method}] = std::move(callback);
    }
    WiFiClient& client() { return client_; }
    String arg(const char* key) const {
        const auto found = arguments.find(key);
        return found == arguments.end() ? String() : String(found->second);
    }
    void send(int code, const char* type = "", const String& body = String()) {
        status = code;
        contentType = type;
        response = body.c_str();
    }
    void sendHeader(const char* name, const char* value) { headers[name] = value; }
    void request(HTTPMethod method) {
        status = 0;
        response.clear();
        headers.clear();
        routes_.at({"/api/cloud", method})();
        if (!status) throw std::runtime_error("HTTP handler did not respond");
    }
    std::map<std::string, std::string> arguments;
    std::map<std::string, std::string> headers;
    int status = 0;
    std::string contentType;
    std::string response;
private:
    WiFiClient client_;
    std::map<std::pair<std::string, HTTPMethod>, std::function<void()>> routes_;
};
