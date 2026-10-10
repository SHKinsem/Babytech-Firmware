#pragma once
#include "PubSubClient.h"
#include "freertos/FreeRTOS.h"
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fake {
struct StopWorker {};
struct Task {
    TaskFunction_t entry;
    void* context;
    uint32_t stack;
    BaseType_t core;
};
struct Packet { std::string topic; std::string payload; bool retained = false; };
struct Io {
    bool inWorker = false;
    unsigned queueSendCalls = 0;
    unsigned queueSendFailures = 0;
    unsigned preferenceWriteCalls = 0;
    std::vector<std::pair<std::string, bool>> preferenceOpens;
    uint32_t now = 1000;
    unsigned delayLimit = 1000;
    unsigned allocationCalls = 0;
    unsigned failAllocation = 0;
    bool failBuffer = false;
    bool failTask = false;
    bool failPreferencesOpen = false;
    bool failPreferencesWrite = false;
    bool failPreferencesReadOpen = false;
    bool failPreferencesLength = false;
    bool failPreferencesRead = false;
    int corruptPreferencesReadOffset = -1;
    unsigned openPreferences = 0;
    unsigned preferenceReads = 0;
    unsigned taskAttempts = 0;
    unsigned delays = 0;
    unsigned randomCalls = 0;
    bool zeroRandom = false;
    unsigned connectCalls = 0;
    unsigned disconnectCalls = 0;
    unsigned loopCalls = 0;
    bool connectOk = true;
    bool loopOk = true;
    bool publishOk = true;
    std::string rejectedPublishTopic;
    bool deferNextQueueSend = false;
    std::deque<bool> subscriptionResults;
    std::vector<Task> tasks;
    std::vector<PubSubClient*> clients;
    std::vector<Packet> published;
    std::deque<Packet> incoming;
    std::vector<std::string> subscriptions;
    std::vector<std::string> serial;
    std::string server;
    uint16_t port = 0;
    uint16_t timeout = 0;
    uint16_t keepAlive = 0;
    std::string connectedId;
    bool credentialsMatched = false;
    std::map<std::string, std::vector<uint8_t>> preferences;
    std::function<void(unsigned)> onDelay;
    std::function<void()> onLoop;
    std::function<void()> onPreferencesWrite;
    std::function<void()> onSemaphoreWait;
    std::function<void()> onConnect;
};
extern Io io;
size_t liveQueues();
size_t queuedItems();
size_t liveSemaphores();
void check(bool condition, const char* message);
void runWorker();
// Model a producer preempted just before queue insertion, then resumed after
// the worker has changed sessions. This is an explicit scheduling fault only.
void completeDeferredSends();
// Only call after the simulated lifetime-long worker has stopped. Failure-path
// assertions must run BEFORE this final host-owned resource cleanup.
void cleanupLifetimeResources();
}  // namespace fake
