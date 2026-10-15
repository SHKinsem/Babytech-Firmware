// Each case runs in a fresh process: CloudLink intentionally has MCU-lifetime
// singleton ownership and no shutdown API. Never expose private production state.
#include "CloudLink.h"
#include "FakeCloudIo.h"
#include "WiFi.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using fake::check;
using babytech::cloud::Freshness;
using babytech::cloud::SessionSnapshot;
using babytech::cloud::ProbeReply;
constexpr char kId[] = "bt-host-test";
constexpr char kChallenge[] = "1234567890abcdef1234567890abcdef";
const std::string kPrefix = std::string("devices/") + kId + "/";

std::string command(const std::string& action, const std::string& id) {
    return "{\"device_id\":\"" + std::string(kId) + "\",\"command\":\"" + action +
        "\",\"command_id\":\"" + id + "\"}";
}
std::string config(const std::string& type, unsigned version = 1) {
    return "{\"device_id\":\"" + std::string(kId) + "\",\"type\":\"" + type +
        "\",\"version\":" + std::to_string(version) + "}";
}
void receive(const char* suffix, const std::string& payload) {
    fake::io.clients.front()->deliver(kPrefix + suffix, payload);
}
void expectInbound(CloudLink& link, const std::string& payload) {
    CloudLink::Inbound message;
    check(link.take(message), "expected inbound message");
    check(message.payload == payload, "inbound payload/order mismatch");
    check(message.generation == link.sessionGeneration(), "inbound generation mismatch");
}
void expectEmpty(CloudLink& link) {
    CloudLink::Inbound message;
    check(!link.take(message), "unexpected inbound message");
}
void expectResult(CloudLink& link, const std::string& tag, bool accepted) {
    CloudLink::PublishResult result;
    check(link.takePublishResult(result), "missing tagged publish result");
    check(result.tag == tag && result.accepted == accepted, "wrong tagged publish result");
}
void expectNoResult(CloudLink& link) {
    CloudLink::PublishResult result;
    check(!link.takePublishResult(result), "unexpected publish result");
}
SessionSnapshot snapshot(CloudLink& link) {
    SessionSnapshot value;
    check(link.sessionSnapshot(value), "expected active session");
    return value;
}
void configure(CloudLink& link, WebServer& server) {
    link.registerRoutes(server, nullptr);
    server.arguments = {{"host", "broker.test"}, {"port", "1883"},
        {"username", "test-user"}, {"password", "host-only-test-secret"}};
    server.request(HTTP_POST);
    check(server.status == 200 && link.configured(), "configuration POST failed");
    check(fake::io.openPreferences == 0, "configuration did not close Preferences");
}
void start(CloudLink& link, WebServer& server) {
    check(link.beginV4(kId), "beginV4 failed");
    configure(link, server);
}
void expectResources() {
    check(fake::liveQueues() == 5 && fake::liveSemaphores() == 1, "wrong live resource count");
    check(fake::io.tasks.size() == 1, "wrong worker count");
    check(fake::io.tasks.front().stack == 6144 && fake::io.tasks.front().core == 0,
          "worker configuration changed");
}
void stop() { throw fake::StopWorker{}; }

// Fixture for the existing cloudcfg/record byte layout, not a settings implementation.
struct SettingsRecord {
    uint32_t magic = 0x42544331;
    char host[128] = "cached.test";
    char user[64] = "test-user";
    char password[128] = "host-only-test-secret";
    uint16_t port = 1884;
};

void seedSettings() {
    SettingsRecord record;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    fake::io.preferences["cloudcfg/record"] = std::vector<uint8_t>(bytes, bytes + sizeof(record));
}

void expectSaved(const std::string& host, uint16_t port, const std::string& user = "test-user",
                 const std::string& password = "host-only-test-secret") {
    SettingsRecord record;
    const auto& bytes = fake::io.preferences.at("cloudcfg/record");
    check(bytes.size() == sizeof(record), "existing record layout changed");
    std::memcpy(&record, bytes.data(), sizeof(record));
    check(record.magic == 0x42544331 && record.port == port &&
          std::memcmp(record.host, host.c_str(), host.size() + 1) == 0 &&
          std::memcmp(record.user, user.c_str(), user.size() + 1) == 0 &&
          std::memcmp(record.password, password.c_str(), password.size() + 1) == 0,
          "persisted configuration differs from successful candidate");
    check(fake::io.openPreferences == 0, "configuration leaked Preferences");
}

void expectUnstarted(CloudLink& link) {
    check(fake::io.allocationCalls == 0 && fake::liveQueues() == 0 && fake::liveSemaphores() == 0 &&
          fake::io.taskAttempts == 0 && fake::io.tasks.empty(), "pre-start configuration allocated resources");
    check(WiFi.calls.empty() && fake::io.connectCalls == 0 && fake::io.disconnectCalls == 0 &&
          fake::io.loopCalls == 0 && fake::io.published.empty() && fake::io.subscriptions.empty() &&
          fake::io.server.empty(), "pre-start configuration touched network");
    check(fake::io.clients.front()->callbackChanges() == 0, "pre-start configuration claimed callback");
    SessionSnapshot value;
    check(!link.configured() && link.host().isEmpty() && link.port() == 1883 &&
          !link.connected() && !link.sessionSnapshot(value) && link.sessionGeneration() == 1,
          "pre-start configuration became active before valid identity");
    check(fake::io.openPreferences == 0, "pre-start configuration leaked Preferences");
}

void configureValidation(bool unstarted = false) {
    CloudLink link;
    seedSettings();
    if (!unstarted) check(link.beginV4(kId), "begin failed");
    const auto original = fake::io.preferences;
    const auto generation = link.sessionGeneration();
    const auto rejected = [&](const char* host, uint16_t port, const char* user, const char* password) {
        check(!link.configure(host, port, user, password), "invalid configuration accepted");
        check(fake::io.preferenceWriteCalls == 0 && fake::io.preferences == original,
              "invalid configuration touched NVS");
        if (unstarted) expectUnstarted(link);
        else check(std::string(link.host().c_str()) == "cached.test" && link.port() == 1884 &&
                   link.sessionGeneration() == generation, "invalid configuration changed active settings/session");
    };
    for (const char* empty : {static_cast<const char*>(nullptr), ""}) {
        rejected(empty, 1883, "user", "password");
        rejected("broker.test", 1883, empty, "password");
        rejected("broker.test", 1883, "user", empty);
    }
    rejected("broker.test", 0, "user", "password");
    for (const char* host : {"mqtt://broker.test", "broker.test/path", "broker test", "broker_test", "host\""})
        rejected(host, 1883, "user", "password");
    for (unsigned c = 1; c <= 127; ++c) {
        if (c >= 32 && c != 127) continue;
        const std::string bad = std::string("a") + static_cast<char>(c) + "b";
        rejected(bad.c_str(), 1883, "user", "password");
        rejected("broker.test", 1883, bad.c_str(), "password");
        rejected("broker.test", 1883, "user", bad.c_str());
    }
    // Exact-sized unterminated inputs let ASan detect an unbounded strlen/read.
    std::array<char, 128> fullHost, fullPassword;
    std::array<char, 64> fullUser;
    fullHost.fill('h'); fullPassword.fill('p'); fullUser.fill('u');
    rejected(fullHost.data(), 1883, "user", "password");
    rejected("broker.test", 1883, fullUser.data(), "password");
    rejected("broker.test", 1883, "user", fullPassword.data());
    const std::string huge(4096, 'x');
    rejected(huge.c_str(), 1883, "user", "password");
    rejected("broker.test", 1883, huge.c_str(), "password");
    rejected("broker.test", 1883, "user", huge.c_str());
    const std::string host(127, 'h'), user(63, 'u'), password(127, 'p');
    check(link.configure(host.c_str(), 65535, user.c_str(), password.c_str()), "maximum fields rejected");
    expectSaved(host, 65535, user, password);
    check(link.configure("::1", 1, "user name", "pass word!\""), "valid printable values rejected");
    expectSaved("::1", 1, "user name", "pass word!\"");
    if (unstarted) expectUnstarted(link);
}

void configureLifecycle() {
    CloudLink link;
    seedSettings();
    const auto attempt = [&] { return link.configure("broker.test", 1883, "test-user", "host-only-test-secret"); };
    check(attempt(), "configuration before begin failed");
    expectSaved("broker.test", 1883);
    expectUnstarted(link);
    check(fake::io.preferenceOpens == std::vector<std::pair<std::string, bool>>{
          {"cloudcfg", false}, {"cloudcfg", true}}, "pre-start save did not verify existing record");
    for (const char* id : {static_cast<const char*>(nullptr), "", "bad/id"}) {
        check(!link.beginV4(id), "invalid identity accepted saved configuration");
        check(attempt(), "configuration after invalid identity could not retry");
        expectUnstarted(link);
    }
    fake::io.failTask = true;
    check(!link.beginV4(kId) && attempt(), "configuration after failed startup could not retry");
    check(fake::liveQueues() == 0 && fake::liveSemaphores() == 0 && fake::io.tasks.empty(),
          "failed begin/configure retry leaked resources");
    fake::io.failTask = false;
    check(link.beginV4(kId), "valid startup/configuration retry failed");
    check(link.configured() && std::string(link.host().c_str()) == "broker.test" && link.port() == 1883,
          "valid begin did not load pre-start configuration");
    expectResources();
    fake::io.onDelay = [&](unsigned) {
        check(link.connected() && fake::io.connectedId == kId && fake::io.server == "broker.test" &&
              fake::io.port == 1883 && fake::io.credentialsMatched, "worker did not use pre-start configuration");
        snapshot(link);
        stop();
    };
    fake::runWorker();
    expectSaved("broker.test", 1883);
}

void configurePrestartFailure(const std::string& mode) {
    CloudLink link;
    seedSettings();
    fake::io.preferences["unrelated/record"] = {1, 2, 3};
    const auto original = fake::io.preferences;
    fake::io.failPreferencesOpen = mode == "open";
    fake::io.failPreferencesWrite = mode == "write";
    fake::io.failPreferencesReadOpen = mode == "read-open";
    fake::io.failPreferencesLength = mode == "length";
    fake::io.failPreferencesRead = mode == "read";
    if (mode == "magic") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, magic);
    if (mode == "host") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, host);
    if (mode == "user") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, user);
    if (mode == "password") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, password);
    if (mode == "port") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, port);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        check(!link.configure("replacement.test", 2883, "test-user", "host-only-test-secret"),
              "pre-start save/verification failure succeeded");
        expectUnstarted(link);
        if (mode == "open" || mode == "write")
            check(fake::io.preferences == original, "failed pre-start write altered NVS");
        else expectSaved("replacement.test", 2883);
        check(fake::io.preferences.at("unrelated/record") == std::vector<uint8_t>({1, 2, 3}),
              "pre-start configuration erased unrelated NVS");
    }
    fake::io.failPreferencesOpen = fake::io.failPreferencesWrite = false;
    fake::io.failPreferencesReadOpen = fake::io.failPreferencesLength = fake::io.failPreferencesRead = false;
    fake::io.corruptPreferencesReadOffset = -1;
    check(link.configure("retried.test", 3883, "test-user", "host-only-test-secret"),
          "pre-start storage failure permanently blocked retry");
    expectSaved("retried.test", 3883);
    expectUnstarted(link);
    check(link.beginV4(kId), "retried pre-start configuration did not start");
    fake::io.onDelay = [&](unsigned) {
        check(link.connected() && fake::io.server == "retried.test" && fake::io.port == 3883 &&
              fake::io.credentialsMatched, "worker did not load retried pre-start configuration");
        snapshot(link);
        stop();
    };
    fake::runWorker();
}

void configureFailure(const std::string& mode) {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.preferences["unrelated/record"] = {1, 2, 3};
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            const auto before = snapshot(link);
            const auto original = fake::io.preferences;
            fake::io.failPreferencesOpen = mode == "open";
            fake::io.failPreferencesWrite = mode == "write";
            fake::io.failPreferencesReadOpen = mode == "read-open";
            fake::io.failPreferencesLength = mode == "length";
            fake::io.failPreferencesRead = mode == "read";
            if (mode == "magic") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, magic);
            if (mode == "host") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, host);
            if (mode == "user") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, user);
            if (mode == "password") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, password);
            if (mode == "port") fake::io.corruptPreferencesReadOffset = offsetof(SettingsRecord, port);
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                check(!link.configure("replacement.test", 2883, "test-user", "host-only-test-secret"),
                      "injected save/verification failure succeeded");
                check(link.configured() && std::string(link.host().c_str()) == "broker.test" &&
                      link.port() == 1883 && snapshot(link).generation == before.generation,
                      "failed configure replaced RAM or disconnected working session");
                check(fake::io.openPreferences == 0, "failed configure leaked Preferences");
                if (mode == "open" || mode == "write")
                    check(fake::io.preferences == original, "failed write erased/changed existing NVS");
                else expectSaved("replacement.test", 2883);
                check(fake::io.preferences.at("unrelated/record") == std::vector<uint8_t>({1, 2, 3}),
                      "configuration erased unrelated NVS");
            }
            fake::io.failPreferencesOpen = fake::io.failPreferencesWrite = false;
            fake::io.failPreferencesReadOpen = fake::io.failPreferencesLength = fake::io.failPreferencesRead = false;
            fake::io.corruptPreferencesReadOffset = -1;
            check(link.configure("replacement.test", 2883, "test-user", "host-only-test-secret"),
                  "configuration could not retry after storage failure");
            check(!link.connected() && link.sessionGeneration() == before.generation + 1,
                  "successful configuration did not request reconnect");
        } else {
            snapshot(link);
            expectSaved("replacement.test", 2883);
            check(fake::io.server == "replacement.test" && fake::io.port == 2883 &&
                  fake::io.credentialsMatched, "worker did not apply retried configuration");
            stop();
        }
    };
    fake::runWorker();
}

void configureThreads(const std::string& mode) {
    CloudLink link;
    WebServer server;
    start(link, server);
    std::promise<void> written, resume, waiting;
    auto writtenFuture = written.get_future();
    auto resumeFuture = resume.get_future().share();
    auto waitingFuture = waiting.get_future();
    const bool failFirst = mode == "failure";
    fake::io.failPreferencesRead = failFirst;
    unsigned writes = 0;
    std::atomic<bool> signaled{false};
    fake::io.onPreferencesWrite = [&] {
        if (++writes != 1) {
            fake::io.failPreferencesRead = false;
            return;
        }
        written.set_value();
        resumeFuture.wait();
    };
    fake::io.onSemaphoreWait = [&] {
        if (!signaled.exchange(true)) waiting.set_value();
    };
    bool firstOk = false, secondOk = false;
    std::exception_ptr firstError, secondError;
    std::thread first([&] {
        try { firstOk = link.configure("first.test", 2883, "first-user", "first-password"); }
        catch (...) { firstError = std::current_exception(); }
    });
    const bool paused = writtenFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    std::thread second([&] {
        try {
            if (mode == "api" || failFirst) {
                secondOk = link.configure("second.test", 3883, "second-user", "second-password");
            } else if (mode == "http") {
                server.arguments = {{"host", "second.test"}, {"port", "3883"},
                    {"username", "second-user"}, {"password", "second-password"}};
                server.request(HTTP_POST);
                secondOk = server.status == 200;
            } else {
                secondOk = std::string(link.host().c_str()) == "first.test" && link.port() == 2883;
            }
        } catch (...) { secondError = std::current_exception(); }
    });
    const bool serialized = waitingFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    resume.set_value();
    first.join();
    second.join();
    fake::io.onPreferencesWrite = nullptr;
    fake::io.onSemaphoreWait = nullptr;
    if (firstError) std::rethrow_exception(firstError);
    if (secondError) std::rethrow_exception(secondError);
    check(paused && serialized && firstOk == !failFirst && secondOk,
          "configuration transaction was not serialized/retryable");
    const bool reader = mode == "reader";
    const char* host = reader ? "first.test" : "second.test";
    const uint16_t port = reader ? 2883 : 3883;
    expectSaved(host, port, reader ? "first-user" : "second-user", reader ? "first-password" : "second-password");
    check(std::string(link.host().c_str()) == host && link.port() == port, "RAM/NVS diverged across threads");
    check(link.configure("retry.test", 4883, "test-user", "host-only-test-secret"), "lock remained held after threads");
    expectSaved("retry.test", 4883);
}

void configureWorker(bool duringConnect) {
    CloudLink link;
    WebServer server;
    start(link, server);
    uint32_t previous = 0;
    bool changed = false;
    const auto update = [&] {
        previous = link.sessionGeneration();
        const auto disconnects = fake::io.disconnectCalls;
        bool saved = false;
        std::thread caller([&] {
            saved = link.configure("replacement.test", 2883, "test-user", "host-only-test-secret");
        });
        caller.join();
        check(saved && !link.connected() && link.sessionGeneration() == previous + 1,
              "configure did not invalidate session immediately");
        check(fake::io.disconnectCalls == disconnects, "configure touched worker-owned socket");
        changed = true;
    };
    if (duringConnect) fake::io.onConnect = [&] { if (!changed) update(); };
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            if (duringConnect) {
                check(changed && !link.connected(), "old in-flight connect reopened invalidated session");
            } else {
                const auto current = snapshot(link);
                check(link.publishForSession("status", "old", current.generation, "old"), "enqueue failed");
                receive("command", command("prepare", "old"));
                update();
                expectEmpty(link);
                check(!link.publishForSession("status", "old", current.generation), "old session accepted publish");
            }
        } else {
            check(snapshot(link).generation != previous, "worker reused old session");
            check(fake::io.server == "replacement.test" && fake::io.port == 2883 &&
                  fake::io.credentialsMatched, "worker did not switch validated settings");
            if (!duringConnect) expectResult(link, "old", false);
            check(fake::io.published.empty(), "old queued payload reached new broker");
            expectSaved("replacement.test", 2883);
            stop();
        }
    };
    fake::runWorker();
}

void startupFailure(const std::string& failure, bool prestored = false) {
    CloudLink link;
    WebServer server;
    seedSettings();
    if (prestored) {
        check(link.configure("broker.test", 1883, "test-user", "host-only-test-secret"),
              "configuration before resource failure failed");
        expectUnstarted(link);
    }
    const auto saved = fake::io.preferences;
    const unsigned reads = fake::io.preferenceReads;
    if (failure.compare(0, 6, "alloc-") == 0)
        fake::io.failAllocation = static_cast<unsigned>(std::stoul(failure.substr(6)));
    else if (failure == "packet") fake::io.failBuffer = true;
    else fake::io.failTask = true;
    const unsigned failureSlot = fake::io.failAllocation;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        fake::io.allocationCalls = 0;
        check(!link.beginV4(kId), "injected startup failure succeeded");
        check(fake::liveQueues() == 0 && fake::liveSemaphores() == 0,
              "startup failure leaked queue/mutex before retry");
        check(fake::io.tasks.empty(), "startup failure registered worker");
        check(!link.configured() && link.host().isEmpty() && link.port() == 1883,
              "startup failure retained settings");
        check(!link.connected(), "startup failure marked connected");
        check(fake::io.openPreferences == 0, "startup failure leaked Preferences");
        check(fake::io.clients.front()->bufferSize() == (failure == "task" ? 2560u : 0u),
              "packet buffer grew across startup failures");
        check(fake::io.allocationCalls == 6, "allocation matrix no longer covers all resources");
        if (failureSlot || failure == "packet")
            check(fake::io.taskAttempts == 0, "started task with unavailable resources");
        if (!failureSlot)
            check(fake::io.preferenceReads == reads + attempt + 1, "failure did not exercise loaded settings");
        check(fake::io.preferences == saved, "startup failure changed saved configuration");
        SessionSnapshot value;
        check(!link.sessionSnapshot(value), "startup failure left active session");
        check(!link.publishForSession("status", "{}", 1, "failed"), "failed link accepted publish");
        expectEmpty(link);
        expectNoResult(link);
        link.registerRoutes(server, nullptr);
        server.request(HTTP_POST);
        check(server.status == 503, "failed link configuration should be unavailable");
    }
    fake::io.failAllocation = 0;
    fake::io.failBuffer = false;
    fake::io.failTask = false;
    if (!prestored) fake::io.preferences.clear();
    check(link.beginV4(kId), "startup failure was not retryable");
    if (prestored) {
        check(link.configured() && std::string(link.host().c_str()) == "broker.test" && link.port() == 1883,
              "startup retry did not reload pre-start settings");
        expectSaved("broker.test", 1883);
    } else {
        check(!link.configured() && link.host().isEmpty() && link.port() == 1883,
              "retry resurrected settings from failed startup");
        configure(link, server);
    }
    expectResources();
    fake::io.onDelay = [&](unsigned) {
        check(link.connected(), "startup retry did not connect");
        check(fake::io.server == "broker.test" && fake::io.port == 1883 && fake::io.credentialsMatched,
              "startup retry used wrong saved settings");
        snapshot(link);
        receive("command", command("stop", "after-retry"));
        expectInbound(link, command("stop", "after-retry"));
        stop();
    };
    fake::runWorker();
}

void ownership() {
    CloudLink owner;
    CloudLink second;
    WebServer server;
    check(!owner.beginV4("bad/id"), "invalid identity accepted");
    check(fake::io.allocationCalls == 0, "invalid identity allocated resources");
    start(owner, server);
    const auto saved = fake::io.preferences;
    const unsigned writes = fake::io.preferenceWriteCalls;
    check(!second.configure("other.test", 2883, "other-user", "other-secret"),
          "unstarted instance configured another active owner's record");
    check(fake::io.preferences == saved && fake::io.preferenceWriteCalls == writes &&
          !second.configured(), "rejected other-owner configuration changed settings");
    const auto task = fake::io.tasks.front();
    const unsigned allocations = fake::io.allocationCalls;
    check(!owner.beginV4("replacement"), "repeated beginV4 accepted");
    owner.begin("replacement");
    check(!second.beginV4("second"), "second owner accepted");
    second.begin("second");
    check(fake::io.allocationCalls == allocations, "reentrant begin allocated resources");
    check(fake::io.taskAttempts == 1 && fake::io.tasks.front().context == task.context &&
          task.context == &owner, "begin replaced worker context");
    check(std::string(owner.deviceId()) == kId, "begin replaced owner identity");
    check(fake::io.clients[0]->callbackChanges() == 1 &&
          fake::io.clients[1]->callbackChanges() == 0, "begin replaced callback registration");
    fake::io.onDelay = [&](unsigned) {
        receive("command", command("stop", "owner"));
        expectInbound(owner, command("stop", "owner"));
        expectEmpty(second);
        snapshot(owner);
        stop();
    };
    fake::runWorker();
}

void legacy() {
    CloudLink link;
    WebServer server;
    link.begin(kId);
    configure(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        SessionSnapshot value;
        ProbeReply reply;
        check(link.connected(), "legacy begin did not connect");
        check(!link.sessionSnapshot(value), "legacy begin exposed v4 session");
        check(!link.probeReply(kChallenge, kChallenge, link.sessionGeneration(), reply),
              "legacy begin exposed v4 probe");
        check(link.checkFreshness(kChallenge, link.sessionGeneration(), millis(), 5000) ==
              Freshness::Disconnected, "legacy begin authorized v4 command");
        check(!link.publishForSession("status", "{}", link.sessionGeneration()),
              "legacy begin allowed session publication");
        check(!link.publishStatusForSession("{}", link.sessionGeneration(), CloudLink::StatusPublishOptions{}),
              "legacy begin allowed v4 status publication");
        check(fake::io.randomCalls == 0, "legacy begin created token");
        if (tick == 1) {
            receive("config", config("feeding_context"));
            receive("config", config("command_session_probe"));
            expectInbound(link, config("feeding_context"));
            expectEmpty(link);
            receive("command", std::string(1535, 'x'));
            expectInbound(link, std::string(1535, 'x'));
            receive("command", std::string(1536, 'x'));
            expectEmpty(link);
            check(link.publish("status", "legacy-body", "legacy"), "legacy publish rejected");
        } else {
            check(fake::io.published.size() == 1, "legacy publish not sent");
            expectResult(link, "legacy", true);
            stop();
        }
    };
    fake::runWorker();
}

void session() {
    CloudLink link;
    WebServer server;
    start(link, server);
    SessionSnapshot first;
    uint32_t openedAt = fake::io.now;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            first = snapshot(link);
            check(std::strlen(first.id) == 32 && first.generation != 0, "invalid session token");
            check(std::all_of(first.id, first.id + 32, [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            }), "token not lowercase hexadecimal");
            check(fake::io.credentialsMatched && fake::io.connectedId == kId,
                  "MQTT connection identity/credentials mismatch");
            check(fake::io.subscriptions == std::vector<std::string>{kPrefix + "command", kPrefix + "config"},
                  "MQTT subscription topics mismatch");
            check(fake::io.timeout == 1 && fake::io.keepAlive == 15, "MQTT timing configuration mismatch");
            ProbeReply reply;
            check(link.probeReply(first.id, kChallenge, first.generation, reply), "valid probe rejected");
            check(std::string(reply.challenge) == kChallenge &&
                  std::string(reply.session.id) == first.id, "probe response mismatch");
            check(!link.probeReply(first.id, kChallenge, first.generation + 1, reply), "old generation probe accepted");
            check(!link.probeReply(kChallenge, kChallenge, first.generation, reply), "wrong token probe accepted");
            check(!link.probeReply(first.id, "bad", first.generation, reply), "bad challenge accepted");
            check(link.checkFreshness(first.id, first.generation, millis(), 5000) == Freshness::Current,
                  "fresh command rejected");
            check(link.checkFreshness(first.id, first.generation, millis(), 1) == Freshness::InvalidTtl,
                  "invalid TTL accepted");
            check(link.checkFreshness(first.id, first.generation, millis() + 1, 5000) == Freshness::Expired,
                  "future sample accepted");
            fake::io.now = openedAt + babytech::cloud::kSessionLifetimeMs - 1;
            snapshot(link);
            fake::io.now += 1;
            SessionSnapshot expired;
            check(!link.sessionSnapshot(expired), "session valid at exact 24-hour boundary");
            check(link.checkFreshness(first.id, first.generation, millis(), 5000) == Freshness::Disconnected,
                  "expired session authorized command");
            check(!link.probeReply(first.id, kChallenge, first.generation, reply), "expired session answered probe");
        } else {
            const auto next = snapshot(link);
            check(next.generation != first.generation && std::string(next.id) != first.id,
                  "24-hour reconnect reused session");
            check(fake::io.connectCalls == 2, "24-hour expiry did not reconnect");
            check(link.checkFreshness(first.id, first.generation, millis(), 5000) == Freshness::WrongSession,
                  "new connection accepted expired token");
            stop();
        }
    };
    fake::runWorker();
}

void inbound() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            // Arrival through loop() exercises PubSubClient's registered callback.
            fake::io.incoming.push_back({kPrefix + "config", config("feeding_context", 1)});
            fake::io.incoming.push_back({kPrefix + "config", config("command_session_probe")});
            fake::io.incoming.push_back({kPrefix + "command", command("prepare", "work")});
            fake::io.incoming.push_back({kPrefix + "command", command("stop", "stop")});
        } else {
            expectInbound(link, command("stop", "stop"));
            expectInbound(link, config("feeding_context", 1));
            expectInbound(link, config("command_session_probe"));
            expectInbound(link, command("prepare", "work"));
            expectEmpty(link);
            check(link.droppedConfig() == 0 && link.droppedInbound() == 0, "probe overwrote retained config");
            receive("config", config("feeding_context", 2));
            receive("config", config("feeding_context", 3));
            receive("config", config("feeding_event_receipt"));
            expectInbound(link, config("feeding_context", 3));
            expectInbound(link, config("feeding_event_receipt"));
            check(link.droppedConfig() == 1 && link.droppedInbound() == 1, "config overwrite counters wrong");
            for (unsigned i = 0; i < 8; ++i) receive("command", command("prepare", std::to_string(i)));
            receive("config", config("feeding_context", 4));
            receive("config", config("command_session_probe"));
            receive("command", command("prepare", "overflow"));
            for (unsigned i = 0; i < 3; ++i) receive("command", command("stop", std::to_string(i)));
            expectInbound(link, command("stop", "1"));
            expectInbound(link, command("stop", "2"));
            expectInbound(link, config("feeding_context", 4));
            for (unsigned i = 0; i < 8; ++i) expectInbound(link, command("prepare", std::to_string(i)));
            expectEmpty(link);
            check(link.droppedInbound() == 4 && link.droppedConfig() == 1,
                  "queue pressure counters or stop eviction wrong");
            fake::io.clients.front()->deliver("devices/other/command", command("stop", "wrong-topic"));
            receive("status", "{}");
            receive("config", "{invalid");
            receive("config", "{\"device_id\":\"other\",\"type\":\"feeding_context\"}");
            receive("command", std::string(2048, 'x'));
            receive("command", std::string("a\0b", 3));
            expectEmpty(link);
            for (size_t size : {size_t(1535), size_t(1536), size_t(2047)}) {
                receive("command", std::string(size, 'x'));
                expectInbound(link, std::string(size, 'x'));
            }
            stop();
        }
    };
    fake::runWorker();
}

void outbound() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        const auto current = snapshot(link);
        if (tick == 1) {
            check(!link.publish("status", "unsafe", "unsafe"), "v4 allowed legacy publish");
            check(!link.publishForSession(nullptr, "{}", current.generation), "null suffix accepted");
            check(!link.publishForSession("status", std::string(2048, 'x'), current.generation),
                  "oversized payload accepted");
            check(!link.publishForSession(std::string(128, 'x').c_str(), "{}", current.generation),
                  "oversized topic accepted");
            for (unsigned i = 0; i < 8; ++i) {
                const std::string tag = std::to_string(i);
                check(link.publishForSession("status", i == 0 ? std::string(2047, 'x') : tag,
                      current.generation, tag.c_str()), "outbound queue filled early");
            }
            check(!link.publishForSession("status", "overflow", current.generation), "outbound queue overflow accepted");
        } else {
            check(fake::io.published.size() == (tick - 1) * 2, "worker publish budget not two per iteration");
            const unsigned first = (tick - 2) * 2;
            expectResult(link, std::to_string(first), true);
            expectResult(link, std::to_string(first + 1), true);
            expectNoResult(link);
            const auto& packet = fake::io.published.back();
            check(packet.topic == kPrefix + "status" && !packet.retained, "wrong outbound topic/retain flag");
            if (tick == 5) {
                check(fake::io.published.front().payload == std::string(2047, 'x'), "maximum payload corrupted");
                stop();
            }
        }
    };
    fake::runWorker();
}

void disconnect(const std::string& mode) {
    CloudLink link;
    WebServer server;
    start(link, server);
    SessionSnapshot previous;
    std::string oldPayload;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            previous = snapshot(link);
            oldPayload = std::string("{\"session\":\"") + previous.id + "\"}";
            receive("command", command("prepare", "old-command"));
            receive("command", command("stop", "old-stop"));
            receive("config", config("feeding_context"));
            check(link.publishForSession("status", oldPayload, previous.generation, "old"),
                  "current generation enqueue rejected");
            if (mode == "wifi") WiFi.state = 0;
            else if (mode == "socket") fake::io.clients.front()->dropConnection();
            else if (mode == "loop") fake::io.loopOk = false;
            else if (mode == "settings") {
                server.arguments["host"] = "replacement.test";
                server.request(HTTP_POST);
                check(server.status == 200, "settings update failed");
            } else if (mode == "expiry") fake::io.now += babytech::cloud::kSessionLifetimeMs;
            else {
                link.requestReconnect();
                check(!link.connected(), "requestReconnect did not invalidate immediately");
                check(!link.publishForSession("status", oldPayload, previous.generation),
                      "disconnected link accepted old payload");
            }
        } else if (link.sessionGeneration() != previous.generation) {
            if (!link.connected()) {
                SessionSnapshot unavailable;
                check(!link.sessionSnapshot(unavailable), "disconnected link exposed session");
                WiFi.state = WL_CONNECTED;
                fake::io.loopOk = true;
                fake::io.now += 5000;
                return;
            }
            const auto current = snapshot(link);
            check(std::string(current.id) != previous.id, "reconnect reused token");
            check(!link.publishForSession("status", oldPayload, previous.generation, "late-old"),
                  "old payload enqueued after reconnect");
            expectEmpty(link);
            check(link.droppedInbound() == 3, "old generation not filtered from every inbound lane");
            check(fake::io.published.empty(), "old queued payload published across connection");
            expectResult(link, "old", false);
            expectNoResult(link);
            receive("command", command("stop", "new"));
            expectInbound(link, command("stop", "new"));
            if (mode == "settings") check(fake::io.server == "replacement.test", "new settings not applied");
            stop();
        }
    };
    fake::runWorker();
}

void sendTimeGeneration() {
    CloudLink link;
    WebServer server;
    start(link, server);
    SessionSnapshot previous;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            previous = snapshot(link);
            fake::io.deferNextQueueSend = true;
            check(link.publishForSession("status", previous.id, previous.generation, "delayed-old"),
                  "deferred producer rejected valid session");
            link.requestReconnect();
        } else if (tick == 2) {
            const auto current = snapshot(link);
            check(current.generation != previous.generation, "worker did not replace session");
            // The old producer resumes AFTER failPendingPublishes has drained.
            // Only the production send-time guard can reject this queued item.
            fake::completeDeferredSends();
            check(link.publishForSession("status", current.id, current.generation, "current"),
                  "new session publish rejected");
        } else {
            expectResult(link, "delayed-old", false);
            expectResult(link, "current", true);
            check(fake::io.published.size() == 1 && fake::io.published.front().payload != previous.id,
                  "send-time generation guard leaked stale payload");
            stop();
        }
    };
    fake::runWorker();
}

void publishFailure() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            const auto current = snapshot(link);
            check(link.publishForSession("status", "one", current.generation, "one"), "enqueue failed");
            check(link.publishForSession("status", "two", current.generation, "two"), "enqueue failed");
            fake::io.publishOk = false;
        } else if (tick == 2) {
            check(!link.connected(), "publish failure did not invalidate session");
            expectResult(link, "one", false);
            check(fake::io.published.size() == 1, "worker continued sending after failure");
        } else {
            expectResult(link, "two", false);
            expectNoResult(link);
            check(fake::io.published.size() == 1, "pending publish survived disconnect");
            stop();
        }
    };
    fake::runWorker();
}

void sendTimeExpiry() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            const auto current = snapshot(link);
            ProbeReply reply;
            check(link.probeReply(current.id, kChallenge, current.generation, reply), "probe failed");
            const std::string payload = std::string("{\"session\":\"") + reply.session.id + "\"}";
            check(link.publishForSession("status", payload, reply.session.generation, "expired-probe"),
                  "probe response enqueue failed");
            // Expire after run()'s pre-loop snapshot, before the actual send.
            fake::io.onLoop = [] { fake::io.now += babytech::cloud::kSessionLifetimeMs; };
        } else {
            expectResult(link, "expired-probe", false);
            check(fake::io.published.empty(), "probe published after session expired inside MQTT loop");
            stop();
        }
    };
    fake::runWorker();
}

void connectFailure(const std::string& mode) {
    CloudLink link;
    WebServer server;
    start(link, server);
    if (mode == "auth") fake::io.connectOk = false;
    else if (mode == "subscribe-command") fake::io.subscriptionResults = {false};
    else if (mode == "subscribe-config") fake::io.subscriptionResults = {true, false};
    else fake::io.zeroRandom = true;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            SessionSnapshot value;
            check(!link.connected() && !link.sessionSnapshot(value), "failed handshake authorized session");
            if (mode != "random") check(fake::io.randomCalls == 0, "opened session before subscription success");
            fake::io.connectOk = true;
            fake::io.zeroRandom = false;
        } else if (tick == 2) {
            check(fake::io.connectCalls == 1, "connection retry ignored backoff");
            fake::io.now += 5000;
        } else {
            snapshot(link);
            check(fake::io.connectCalls == 2, "handshake not retried");
            stop();
        }
    };
    fake::runWorker();
}

void routes() {
    CloudLink link;
    WebServer server;
    start(link, server);
    server.request(HTTP_GET);
    check(server.status == 200 && server.headers["Cache-Control"] == "no-store", "GET status/cache header wrong");
    DynamicJsonDocument document(1024);
    check(!deserializeJson(document, server.response), "status is not valid JSON");
    check(document["configured"] == true && document["host"] == "broker.test", "status configuration missing");
    check(!document.containsKey("password") && !document.containsKey("username"), "status exposed credential field");
    check(server.response.find("host-only-test-secret") == std::string::npos &&
          server.response.find("test-user") == std::string::npos, "status exposed credential value");
    fake::io.failPreferencesWrite = true;
    server.arguments["host"] = "unsaved.test";
    server.request(HTTP_POST);
    check(server.status == 500 && std::string(link.host().c_str()) == "broker.test", "failed NVS save replaced settings");
    fake::io.failPreferencesWrite = false;
    server.arguments["host"] = "invalid/host";
    server.request(HTTP_POST);
    check(server.status == 400, "invalid host accepted");
    server.client().local = IPAddress(0xC0A80102);
    server.request(HTTP_GET);
    check(server.status == 403, "non-AP status access accepted");
    server.request(HTTP_POST);
    check(server.status == 403, "non-AP configuration accepted");
    check(fake::io.openPreferences == 0, "route Preferences handle leak");
}

void settingsLoad() {
    CloudLink link;
    seedSettings();
    check(link.beginV4(kId), "cached settings begin failed");
    check(link.configured() && std::string(link.host().c_str()) == "cached.test" && link.port() == 1884,
          "valid persisted settings fixture was not loaded");
    check(fake::io.preferenceReads == 1, "settings were not read exactly once");
    fake::io.onDelay = [&](unsigned) {
        snapshot(link);
        check(fake::io.server == "cached.test" && fake::io.port == 1884 && fake::io.credentialsMatched,
              "MQTT did not use loaded settings");
        stop();
    };
    fake::runWorker();
}

CloudLink::StatusPublishOptions sampledStatus(uint32_t receivedAt, bool probe = false) {
    CloudLink::StatusPublishOptions options;
    options.probeReply = probe;
    options.hasMotionSample = true;
    options.motionReceivedAtMs = receivedAt;
    return options;
}

void statusBoundary(const std::string& mode) {
    if (mode == "zero") fake::io.now = 0;
    if (mode == "wrap") fake::io.now = UINT32_MAX - 1000;
    CloudLink link;
    WebServer server;
    start(link, server);
    uint32_t receivedAt = 0;
    fake::io.onDelay = [&](unsigned tick) {
        const auto current = snapshot(link);
        if (tick == 1) {
            receivedAt = mode == "zero" ? 0 : millis();
            check(!link.publishStatusForSession("future", current.generation,
                  sampledStatus(millis() + 1)), "future mechanical sample accepted");
            fake::io.now = receivedAt + 1498;
            check(link.publishStatusForSession("fresh", current.generation,
                  sampledStatus(receivedAt), "fresh"), "1498 ms sample rejected");
            fake::io.onLoop = [&] { fake::io.now = receivedAt + 1499; };
        } else {
            expectResult(link, "fresh", true);
            check(fake::io.published.size() == 1 && fake::io.published[0].payload == "fresh",
                  "1499 ms sample was not sent");
            fake::io.now = receivedAt + 1500;
            for (bool probe : {false, true})
                check(!link.publishStatusForSession("expired", current.generation,
                      sampledStatus(receivedAt, probe)), "sample accepted at exact 1500 ms boundary");
            expectNoResult(link);
            stop();
        }
    };
    fake::runWorker();
}

void statusSendExpiry(bool probe, bool deferred) {
    CloudLink link;
    WebServer server;
    start(link, server);
    uint32_t receivedAt = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            const auto current = snapshot(link);
            receivedAt = millis();
            fake::io.now += 1400;
            ProbeReply reply;
            check(link.probeReply(current.id, kChallenge, current.generation, reply), "probe failed");
            check(reply.session.uptimeMs != receivedAt, "fixture did not age original sample");
            if (deferred) fake::io.deferNextQueueSend = true;
            check(link.publishStatusForSession("expired", reply.session.generation,
                  sampledStatus(receivedAt, probe), "expired"), "live sample enqueue failed");
            // Session remains current; only the original mechanical receipt expires.
            fake::io.onLoop = [&] {
                fake::io.now = receivedAt + 1500;
                if (deferred) fake::completeDeferredSends();
            };
        } else {
            snapshot(link);
            expectResult(link, "expired", false);
            check(fake::io.published.empty(), "same-session stale status/probe reached MQTT");
            check(link.connected() && fake::io.connectCalls == 1, "expiry disconnected healthy MQTT");
            stop();
        }
    };
    fake::runWorker();
}

void statusLatest() {
    CloudLink link;
    WebServer server;
    start(link, server);
    uint32_t receivedAt = 0;
    fake::io.onDelay = [&](unsigned tick) {
        const auto current = snapshot(link);
        if (tick == 1) {
            receivedAt = millis();
            const auto periodic = sampledStatus(millis());
            const auto probe = sampledStatus(millis(), true);
            check(link.publishStatusForSession("old", current.generation, periodic, "old"), "enqueue old failed");
            check(link.publishStatusForSession("new", current.generation, periodic, "new"), "replace failed");
            expectResult(link, "old", false);
            for (unsigned i = 0; i < 8; ++i)
                check(link.publishStatusForSession(std::to_string(i), current.generation, probe),
                      "probe FIFO filled early");
            check(!link.publishStatusForSession("overflow", current.generation, probe), "probe FIFO unbounded");
            // Latest slot must remain writable independently of a saturated probe FIFO.
            check(link.publishStatusForSession("latest", current.generation, periodic, "latest"),
                  "full FIFO blocked periodic replacement");
            expectResult(link, "new", false);
        } else if (tick == 2) {
            check(fake::io.published.size() == 2 && fake::io.published[0].payload == "latest" &&
                  fake::io.published[1].payload == "0", "latest status/probe lanes not isolated");
            expectResult(link, "latest", true);
            // Repeated periodic updates must not starve the probe lane.
            check(link.publishStatusForSession("next", current.generation, sampledStatus(receivedAt)),
                  "second periodic enqueue failed");
        } else if (tick == 3) {
            check(fake::io.published.size() == 4 && fake::io.published[2].payload == "next" &&
                  fake::io.published[3].payload == "1", "periodic pressure starved probe");
        } else if (tick == 6) {
            check(fake::io.published.size() == 10, "probe FIFO not drained");
            for (size_t i = 4; i < 10; ++i)
                check(fake::io.published[i].payload == std::to_string(i - 2), "probe FIFO reordered");
            for (const auto& packet : fake::io.published)
                check(packet.topic == kPrefix + "status" && !packet.retained, "status topic/retain wrong");
            expectNoResult(link);
            stop();
        }
    };
    fake::runWorker();
}

void statusDisconnect(const std::string& mode) {
    CloudLink link;
    WebServer server;
    start(link, server);
    uint32_t previous = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            previous = snapshot(link).generation;
            check(link.publishStatusForSession("old-periodic", previous, sampledStatus(millis()), "periodic"),
                  "periodic enqueue failed");
            check(link.publishStatusForSession("old-probe", previous, sampledStatus(millis(), true), "probe"),
                  "probe enqueue failed");
            if (mode == "wifi") WiFi.state = 0;
            else if (mode == "socket") fake::io.clients.front()->dropConnection();
            else if (mode == "loop") fake::io.loopOk = false;
            else if (mode == "settings") {
                server.arguments["host"] = "replacement.test";
                server.request(HTTP_POST);
                check(server.status == 200, "settings update failed");
            } else if (mode == "expiry") fake::io.now += babytech::cloud::kSessionLifetimeMs;
            else link.requestReconnect();
        } else if (!link.connected()) {
            WiFi.state = WL_CONNECTED;
            fake::io.loopOk = true;
            fake::io.now += 5000;
        } else {
            const auto current = snapshot(link);
            check(current.generation != previous, "session not replaced");
            check(!link.publishStatusForSession("late-old", previous, sampledStatus(millis())),
                  "old session replaced latest status");
            check(fake::io.published.empty(), "old periodic/probe survived reconnect");
            // Disconnect drains FIFO first, then the latest slot.
            expectResult(link, "probe", false);
            expectResult(link, "periodic", false);
            expectNoResult(link);
            stop();
        }
    };
    fake::runWorker();
}

void statusLimits() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            const auto current = snapshot(link);
            CloudLink::StatusPublishOptions options;
            for (bool probe : {false, true}) {
                options.probeReply = probe;
                check(!link.publishStatusForSession(std::string(2048, 'x'), current.generation, options),
                      "2048 byte status accepted");
                check(link.publishStatusForSession(std::string(2047, probe ? 'p' : 's'),
                      current.generation, options), "2047 byte status rejected");
            }
            fake::io.onLoop = [] { fake::io.now += 2000; };
        } else {
            check(fake::io.published.size() == 2, "no-sample conservative statuses expired");
            check(fake::io.published[0].payload == std::string(2047, 's') &&
                  fake::io.published[1].payload == std::string(2047, 'p'), "maximum payload truncated");
            stop();
        }
    };
    fake::runWorker();
}

void statusPublishFailure() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            const auto current = snapshot(link);
            check(link.publishStatusForSession("periodic", current.generation, sampledStatus(millis()), "periodic"),
                  "periodic enqueue failed");
            check(link.publishStatusForSession("probe", current.generation, sampledStatus(millis(), true), "probe"),
                  "probe enqueue failed");
            fake::io.publishOk = false;
        } else if (tick == 2) {
            expectResult(link, "periodic", false);
            check(!link.connected(), "failed periodic publish left session live");
        } else {
            expectResult(link, "probe", false);
            check(fake::io.published.size() == 1, "sent probe after failed periodic publish");
            stop();
        }
    };
    fake::runWorker();
}

void statusSendGeneration() {
    CloudLink link;
    WebServer server;
    start(link, server);
    uint32_t previous = 0;
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            previous = snapshot(link).generation;
            fake::io.deferNextQueueSend = true;
            check(link.publishStatusForSession("old-probe", previous, sampledStatus(millis(), true), "old"),
                  "deferred probe enqueue failed");
            link.requestReconnect();
        } else if (tick == 2) {
            const auto current = snapshot(link);
            check(current.generation != previous, "session did not change");
            check(link.publishStatusForSession("new-periodic", current.generation, sampledStatus(millis()), "new"),
                  "new periodic enqueue failed");
            check(!link.publishStatusForSession("old-periodic", previous, sampledStatus(millis())),
                  "old generation overwrote new periodic status");
            fake::completeDeferredSends();
        } else {
            expectResult(link, "new", true);
            expectResult(link, "old", false);
            check(fake::io.published.size() == 1 && fake::io.published[0].payload == "new-periodic",
                  "deferred old probe published on new session");
            stop();
        }
    };
    fake::runWorker();
}

void statusOrdering(const std::string& mode) {
    const bool wrap = mode == "wrap";
    const bool stale = mode == "stale" || mode == "deferred";
    const bool recover = mode == "recovery";
    const bool newerProbe = mode == "newer-probe";
    if (wrap) fake::io.now = UINT32_MAX - 10;
    CloudLink link;
    WebServer server;
    start(link, server);
    uint32_t oldReceivedAt = 0;
    uint32_t newestReceivedAt = 0;
    fake::io.onDelay = [&](unsigned tick) {
        const auto current = snapshot(link);
        if (tick == 1) {
            oldReceivedAt = millis();
            auto oldOptions = sampledStatus(oldReceivedAt, !newerProbe);
            if (recover) oldOptions.hasMotionSample = false;
            if (mode == "deferred") fake::io.deferNextQueueSend = true;
            check(link.publishStatusForSession(recover ? "old-stale" : "old-normal", current.generation,
                  oldOptions, "old"), "old projection enqueue failed");
            fake::io.now += 1;
            newestReceivedAt = millis();
            auto newOptions = sampledStatus(newestReceivedAt, newerProbe);
            if (stale) newOptions.hasMotionSample = false;
            check(link.publishStatusForSession(stale ? "stale" : "new-fault", current.generation,
                  newOptions, "new"), "new projection enqueue failed");
            if (mode == "deferred") fake::completeDeferredSends();
            // Even a late enqueue cannot resurrect a younger-than-1500ms old sample.
            check(!link.publishStatusForSession("late-old-normal", current.generation,
                  sampledStatus(oldReceivedAt, true)), "late old sample accepted");
        } else if (tick == 2) {
            expectResult(link, newerProbe ? "old" : "new", !newerProbe);
            expectResult(link, newerProbe ? "new" : "old", newerProbe);
            check(fake::io.published.size() == 1 &&
                  fake::io.published[0].payload == (stale ? "stale" : "new-fault"),
                  "old projection regressed Cloud after new projection");
            check(uint32_t(millis() - oldReceivedAt) < 1500, "ordering test accidentally relied on expiry");
            if (stale) {
                check(!link.publishStatusForSession("same-ms-old", current.generation,
                      sampledStatus(newestReceivedAt, true)), "same-ms sample undid stale fence");
                newestReceivedAt = millis();
            }
            // Equal samples can satisfy distinct challenges without renewing their receipt.
            check(link.publishStatusForSession("retry-1", current.generation,
                  sampledStatus(newestReceivedAt, true)), "current retry rejected");
            check(link.publishStatusForSession("retry-2", current.generation,
                  sampledStatus(newestReceivedAt, true)), "same-sample challenge rejected");
        } else {
            check(fake::io.published.size() == 3 && fake::io.published[1].payload == "retry-1" &&
                  fake::io.published[2].payload == "retry-2", "valid probe retries not delivered");
            check(fake::io.connectCalls == 1, "ordering guard unnecessarily reconnected");
            stop();
        }
    };
    fake::runWorker();
}

void statusOrderQueueFull() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        if (tick == 1) {
            const auto current = snapshot(link);
            const uint32_t receivedAt = millis();
            check(link.publishStatusForSession("periodic", current.generation,
                  sampledStatus(receivedAt)), "periodic enqueue failed");
            for (unsigned i = 0; i < 8; ++i)
                check(link.publishStatusForSession("probe", current.generation,
                      sampledStatus(receivedAt, true)), "FIFO filled early");
            fake::io.now += 1;
            check(!link.publishStatusForSession("rejected-new", current.generation,
                  sampledStatus(millis(), true)), "full FIFO accepted new sample");
        } else {
            check(fake::io.published.size() == 2 && fake::io.published[0].payload == "periodic" &&
                  fake::io.published[1].payload == "probe", "failed enqueue advanced ordering watermark");
            stop();
        }
    };
    fake::runWorker();
}

void statusOrderStaleFence() {
    CloudLink link;
    WebServer server;
    start(link, server);
    fake::io.onDelay = [&](unsigned tick) {
        const auto current = snapshot(link);
        if (tick == 1) {
            CloudLink::StatusPublishOptions stale;
            check(link.publishStatusForSession("stale-1", current.generation, stale), "stale enqueue failed");
            fake::io.now += 1;
            const uint32_t between = millis();
            fake::io.now += 1;
            check(link.publishStatusForSession("stale-2", current.generation, stale), "repeat stale failed");
            check(!link.publishStatusForSession("old", current.generation, sampledStatus(between, true)),
                  "repeated stale observation failed to advance fence");
        } else if (tick == 2) {
            check(fake::io.published.size() == 1 && fake::io.published[0].payload == "stale-2",
                  "stale replacement regressed");
            link.requestReconnect();
        } else if (tick == 3) {
            // Per-session ordering must not retain the previous session's stale fence.
            check(link.publishStatusForSession("new-session", current.generation, sampledStatus(1000)),
                  "old session fence survived reconnect");
        } else {
            check(fake::io.published.size() == 2 && fake::io.published[1].payload == "new-session",
                  "new session status was suppressed");
            stop();
        }
    };
    fake::runWorker();
}

struct ServiceFixture {
    unsigned calls = 0;
    static void run(void* context) {
        auto& fixture = *static_cast<ServiceFixture*>(context);
        check(fake::io.inWorker, "network service ran outside worker");
        ++fixture.calls;
        WiFi.state = WL_CONNECTED;
    }
};

void networkService(bool retry) {
    CloudLink link;
    ServiceFixture rejected, active;
    seedSettings();
    if (retry) {
        fake::io.failTask = true;
        check(!link.beginV4(kId, ServiceFixture::run, &rejected), "task failure accepted");
        check(rejected.calls == 0 && fake::liveQueues() == 0 && fake::liveSemaphores() == 0,
              "failed startup ran service or leaked resources");
        fake::io.failTask = false;
    }
    check(link.beginV4(kId, ServiceFixture::run, &active), "service begin failed");
    check(active.calls == 0, "begin ran service synchronously");
    check(!link.beginV4(kId, ServiceFixture::run, &rejected), "repeated begin replaced hook");
    WiFi.state = 0;
    fake::io.onDelay = [&](unsigned tick) {
        snapshot(link);
        check(active.calls == tick && rejected.calls == 0, "service lifecycle/context wrong");
        if (tick == 3) stop();
    };
    fake::runWorker();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string name = argv[1];
    try {
        if (name.compare(0, 6, "alloc-") == 0 || name == "packet" || name == "task") startupFailure(name);
        else if (name == "ownership") ownership();
        else if (name == "legacy") legacy();
        else if (name == "session") session();
        else if (name == "inbound") inbound();
        else if (name == "outbound") outbound();
        else if (name.compare(0, 11, "disconnect-") == 0) disconnect(name.substr(11));
        else if (name == "send-time-generation") sendTimeGeneration();
        else if (name == "send-time-expiry") sendTimeExpiry();
        else if (name == "publish-failure") publishFailure();
        else if (name.compare(0, 8, "connect-") == 0) connectFailure(name.substr(8));
        else if (name == "routes") routes();
        else if (name == "settings-load") settingsLoad();
        else if (name == "configure-validation") configureValidation();
        else if (name == "configure-prestart-validation") configureValidation(true);
        else if (name == "configure-lifecycle") configureLifecycle();
        else if (name.compare(0, 27, "configure-prestart-failure-") == 0) configurePrestartFailure(name.substr(27));
        else if (name.compare(0, 25, "configure-prestart-begin-") == 0) startupFailure(name.substr(25), true);
        else if (name == "configure-worker") configureWorker(false);
        else if (name == "configure-connect-race") configureWorker(true);
        else if (name.compare(0, 18, "configure-failure-") == 0) configureFailure(name.substr(18));
        else if (name.compare(0, 18, "configure-threads-") == 0) configureThreads(name.substr(18));
        else if (name.compare(0, 16, "status-boundary-") == 0) statusBoundary(name.substr(16));
        else if (name == "status-expiry-periodic") statusSendExpiry(false, false);
        else if (name == "status-expiry-probe") statusSendExpiry(true, false);
        else if (name == "status-deferred-probe") statusSendExpiry(true, true);
        else if (name == "status-latest") statusLatest();
        else if (name.compare(0, 18, "status-disconnect-") == 0) statusDisconnect(name.substr(18));
        else if (name == "status-limits") statusLimits();
        else if (name == "status-publish-failure") statusPublishFailure();
        else if (name == "status-send-generation") statusSendGeneration();
        else if (name.compare(0, 16, "status-ordering-") == 0) statusOrdering(name.substr(16));
        else if (name == "status-order-full") statusOrderQueueFull();
        else if (name == "status-order-stale-fence") statusOrderStaleFence();
        else if (name == "network-service") networkService(false);
        else if (name == "network-service-retry") networkService(true);
        else throw std::runtime_error("unknown test case");
        for (const auto& line : fake::io.serial)
            check(line.find("host-only-test-secret") == std::string::npos, "serial exposed test password");
        check(fake::io.clients.empty(), "PubSubClient lifetime resource leak");
        fake::cleanupLifetimeResources();
        std::cout << "PASS cloud-link " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL cloud-link " << name << ": " << error.what() << '\n';
        return 1;
    }
}
