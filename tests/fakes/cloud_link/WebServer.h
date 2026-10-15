#pragma once
#include "Arduino.h"
#include "WiFiClient.h"
#include <functional>
#include <map>
#include <stdexcept>
#include <algorithm>
#include <deque>
#include <vector>

enum HTTPMethod { HTTP_GET, HTTP_POST };
class WebServer {
public:
    explicit WebServer(int port = 80) { (void)port; instances.push_back(this); }
    ~WebServer() { instances.erase(std::remove(instances.begin(), instances.end(), this), instances.end()); }
    static inline std::vector<WebServer*> instances;
    void begin() { listening = true; }
    void stop() { listening = false; }
    void collectHeaders(const char**, size_t) {}
    String header(const char* name) const {
        const auto found = requestHeaders.find(name);
        return found == requestHeaders.end() ? String() : String(found->second);
    }
    void onNotFound(std::function<void()> callback) { notFound_ = std::move(callback); }
    void send_P(int code, const char* type, const char* body) { send(code, type, String(body)); }
    void handleClient() { if (queued) { auto callback = std::move(queued); queued = nullptr; callback(); } }
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
        request("/api/cloud", method);
    }
    void request(const char* path, HTTPMethod method) {
        status = 0;
        response.clear();
        headers.clear();
        const auto route = routes_.find({path, method});
        if (route != routes_.end()) route->second();
        else if (notFound_) notFound_();
        if (!status) throw std::runtime_error("HTTP handler did not respond");
    }
    std::map<std::string, std::string> arguments;
    std::map<std::string, std::string> requestHeaders;
    std::map<std::string, std::string> headers;
    int status = 0;
    std::string contentType;
    std::string response;
    bool listening = false;
    std::function<void()> queued;
private:
    WiFiClient client_;
    std::map<std::pair<std::string, HTTPMethod>, std::function<void()>> routes_;
    std::function<void()> notFound_;
};
