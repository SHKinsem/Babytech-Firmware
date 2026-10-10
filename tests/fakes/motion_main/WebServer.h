#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

enum HTTPMethod {
    HTTP_ANY, HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT, HTTP_PATCH,
    HTTP_DELETE, HTTP_OPTIONS
};
enum HTTPUploadStatus {
    UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED
};
#ifndef HTTP_UPLOAD_BUFLEN
#define HTTP_UPLOAD_BUFLEN 1436
#endif
struct HTTPUpload {
    HTTPUploadStatus status = UPLOAD_FILE_START;
    String filename;
    String name;
    String type;
    size_t totalSize = 0;
    size_t currentSize = 0;
    uint8_t buf[HTTP_UPLOAD_BUFLEN]{};
};

// Share the Motion SDK client/IP types without including another WebServer.
using FakeMotionHttpClient = WiFiClient;

class WebServer {
public:
    using Handler = std::function<void()>;
    using Arguments = std::vector<std::pair<std::string, std::string>>;
    struct UploadEvent {
        HTTPUploadStatus status = UPLOAD_FILE_START;
        std::vector<uint8_t> bytes;
        size_t totalSize = 0;
        std::string name = "firmware";
        std::string filename = "firmware.bin";
        std::string type = "application/octet-stream";
    };
    struct Request {
        HTTPMethod method = HTTP_GET;
        std::string uri = "/";
        // SDK-parsed arguments: ordered, with duplicate names preserved.
        Arguments arguments;
        std::map<std::string, std::string> headers;
        std::vector<UploadEvent> uploads;
        IPAddress remote{192, 168, 4, 2};
        IPAddress local{192, 168, 4, 1};
        // A disconnected multipart upload may never reach the final handler.
        bool complete = true;
    };
    struct Response {
        HTTPMethod method;
        std::string uri;
        int status;
        std::string contentType;
        std::string body;
        std::map<std::string, std::string> headers;
        std::vector<std::pair<std::string, std::string>> headerLines;
        unsigned sendCalls;
        unsigned uploadCalls;
        bool handlerCalled;
    };

    explicit WebServer(int port = 80) : port_(port) {}
    void on(const char* path, HTTPMethod method, Handler handler) {
        on(path, method, std::move(handler), Handler{});
    }
    void on(const char* path, Handler handler) {
        on(path, HTTP_ANY, std::move(handler));
    }
    void on(const char* path, HTTPMethod method, Handler handler, Handler upload) {
        routes_.push_back({path, method, std::move(handler), std::move(upload)});
    }
    void onNotFound(Handler handler) { notFound_ = std::move(handler); }
    void begin() { begun = true; }
    void begin(uint16_t port) { port_ = port; begin(); }
    void stop() { begun = false; }
    void close() { stop(); }
    int port() const { return port_; }
    void enqueue(Request request) { requests_.push_back(std::move(request)); }
    size_t pendingRequests() const { return requests_.size(); }

    // One SDK-parsed request per poll, never a real socket or a direct test call
    // into a production handler. Upload records precede the final handler.
    void handleClient() {
        if (!begun || requests_.empty()) return;
        if (dispatching_) throw std::logic_error("recursive HTTP dispatch");
        active_ = std::move(requests_.front());
        requests_.pop_front();
        arguments.clear();
        for (const auto& item : active_.arguments) arguments.emplace(item);
        headers.clear();
        headerLines.clear();
        status = 0;
        response.clear();
        contentType.clear();
        sendCalls = 0;
        uploadCalls = 0;
        client_.remote = active_.remote;
        client_.local = active_.local;
        upload_ = HTTPUpload{};
        Route selected;
        bool matched = false;
        for (const auto& route : routes_) {
            if (route.path == active_.uri &&
                (route.method == active_.method || route.method == HTTP_ANY)) {
                selected = route;
                matched = true;
                break;
            }
        }
        dispatching_ = true;
        bool handlerCalled = false;
        try {
            if (matched) {
                for (const auto& event : active_.uploads) {
                    if (!selected.upload) throw std::logic_error("route has no upload handler");
                    if (event.bytes.size() > sizeof(upload_.buf))
                        throw std::length_error("upload chunk exceeds SDK buffer");
                    upload_.status = event.status;
                    upload_.name = event.name.c_str();
                    upload_.filename = event.filename.c_str();
                    upload_.type = event.type.c_str();
                    upload_.currentSize = event.bytes.size();
                    upload_.totalSize = event.totalSize;
                    std::memset(upload_.buf, 0, sizeof(upload_.buf));
                    if (!event.bytes.empty())
                        std::memcpy(upload_.buf, event.bytes.data(), event.bytes.size());
                    ++uploadCalls;
                    selected.upload();
                }
                if (active_.complete && selected.handler) {
                    handlerCalled = true;
                    selected.handler();
                }
            } else if (notFound_) {
                handlerCalled = true;
                notFound_();
            } else {
                send(404, "text/plain", "Not found");
            }
        } catch (...) {
            dispatching_ = false;
            throw;
        }
        dispatching_ = false;
        responses.push_back({active_.method, active_.uri, status, contentType,
                             response, headers, headerLines, sendCalls,
                             uploadCalls, handlerCalled});
    }

    HTTPMethod method() const { return active_.method; }
    String uri() const { return String(active_.uri.c_str()); }
    int args() const { return static_cast<int>(active_.arguments.size()); }
    bool hasArg(const char* name) const { return arguments.count(name) != 0; }
    bool hasArg(const String& name) const { return hasArg(name.c_str()); }
    String arg(const char* name) const {
        const auto found = arguments.find(name);
        return found == arguments.end() ? String() : String(found->second.data(), unsigned(found->second.size()));
    }
    String arg(const String& name) const { return arg(name.c_str()); }
    String arg(int index) const {
        return index < 0 || static_cast<size_t>(index) >= active_.arguments.size()
            ? String() : String(active_.arguments[index].second.data(), unsigned(active_.arguments[index].second.size()));
    }
    String argName(int index) const {
        return index < 0 || static_cast<size_t>(index) >= active_.arguments.size()
            ? String() : String(active_.arguments[index].first.c_str());
    }
    void collectHeaders(const char* keys[], size_t count) {
        collectedHeaders_.clear();
        for (size_t i = 0; i < count; ++i) collectedHeaders_.push_back(keys[i]);
    }
    String header(const char* name) const {
        const auto key = lower(name);
        bool collected = false;
        for (const auto& item : collectedHeaders_) if (lower(item) == key) collected = true;
        if (!collected) return String();
        for (const auto& item : active_.headers)
            if (lower(item.first) == key) return String(item.second.c_str());
        return String();
    }
    String header(const String& name) const { return header(name.c_str()); }
    bool hasHeader(const char* name) const { return header(name).length() != 0; }
    HTTPUpload& upload() { return upload_; }
    FakeMotionHttpClient& client() { return client_; }
    const FakeMotionHttpClient& client() const { return client_; }
    void sendHeader(const String& name, const String& value, bool first = false) {
        headers[name.c_str()] = value.c_str();
        const std::pair<std::string, std::string> line{name.c_str(), value.c_str()};
        if (first) headerLines.insert(headerLines.begin(), line);
        else headerLines.push_back(line);
    }
    void send(int code, const char* type = "", const String& body = String()) {
        status = code;
        contentType = type ? type : "";
        response.assign(body.c_str(), body.length());
        ++sendCalls;
    }
    void send_P(int code, const char* type, const char* body) {
        send_P(code, type, body, body ? std::strlen(body) : 0);
    }
    void send_P(int code, const char* type, const char* body, size_t length) {
        if (!body && length) throw std::invalid_argument("null HTTP payload");
        status = code;
        contentType = type ? type : "";
        response.assign(body ? body : "", length);
        ++sendCalls;
    }

    // Match the simple cloud-link fake's observable response field names.
    std::map<std::string, std::string> arguments;
    std::map<std::string, std::string> headers;
    std::vector<std::pair<std::string, std::string>> headerLines;
    int status = 0;
    std::string contentType;
    std::string response;
    unsigned sendCalls = 0;
    unsigned uploadCalls = 0;
    bool begun = false;
    std::vector<Response> responses;

private:
    struct Route {
        std::string path;
        HTTPMethod method = HTTP_ANY;
        Handler handler;
        Handler upload;
    };
    static std::string lower(std::string value) {
        for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }
    int port_;
    bool dispatching_ = false;
    std::vector<Route> routes_;
    Handler notFound_;
    std::deque<Request> requests_;
    Request active_;
    HTTPUpload upload_;
    FakeMotionHttpClient client_;
    std::vector<std::string> collectedHeaders_;
};
