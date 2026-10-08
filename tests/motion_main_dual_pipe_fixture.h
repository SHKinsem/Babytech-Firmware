// Host SDK transport only: production Motion setup/loop owns UART and results.
// Historical terminals are initial Store fixtures, never simulated live feeds.
#pragma once
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace motion_main_dual_pipe {
static_assert(BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW == 0 ||
              BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW == 1, "invalid host execution switch");
constexpr const char* device = "bt-main-test";
constexpr size_t documentCapacity = 2 * 1024 * 1024;
constexpr size_t fileLimit = 1024 * 1024;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
void parse(DynamicJsonDocument& doc, const std::string& raw) {
    std::istringstream stream(raw);
    require(!deserializeJson(doc, stream), "invalid pipe JSON");
    stream >> std::ws;
    require(stream.peek() == std::char_traits<char>::eof(), "trailing pipe JSON bytes");
}
struct Options {
    bool seedHistory = false, prepare = false, installation = false;
    std::string stateFile, contextFile;
    uint32_t bootSeed = 0x16543;
};
uint32_t decimal(const std::string& text, uint32_t maximum) {
    require(!text.empty() && text.size() <= 10, "invalid boot seed");
    uint64_t value = 0;
    for (const auto ch : text) {
        require(ch >= '0' && ch <= '9', "invalid boot seed");
        value = value * 10 + unsigned(ch - '0');
        require(value <= maximum, "boot seed out of range");
    }
    require(value != 0, "boot seed must be nonzero");
    return uint32_t(value);
}
Options options(int argc, char** argv) {
    Options result;
    bool bootSeen = false;
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--install-fixture") {
            require(!result.installation, "duplicate install-fixture");
            result.installation = true;
        } else if (arg == "--prepare-fixture") {
            require(!result.prepare && BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW == 1,
                    "Prepare pipe requires explicitly built host fixture");
            result.prepare = true;
        } else if (arg == "--seed-history") {
            require(!result.seedHistory, "duplicate seed-history");
            result.seedHistory = true;
        } else {
            require(i + 1 < argc, "missing pipe option value");
            const std::string value = argv[++i];
            require(!value.empty(), "empty pipe option value");
            if (arg == "--state-file") {
                require(result.stateFile.empty(), "duplicate state-file");
                result.stateFile = value;
            } else if (arg == "--context-file") {
                require(result.contextFile.empty(), "duplicate context-file");
                result.contextFile = value;
            } else if (arg == "--boot-id") {
                require(!bootSeen, "duplicate boot-id");
                bootSeen = true;
                result.bootSeed = decimal(value, std::numeric_limits<uint32_t>::max() - 32);
            } else throw std::runtime_error("unknown pipe option: " + arg);
        }
    }
    require(!result.prepare || (!result.seedHistory && result.contextFile.empty()),
            "Prepare pipe cannot seed history/context");
    require(!result.installation || (!result.prepare && !result.seedHistory &&
            result.contextFile.empty() && !result.stateFile.empty()),
            "installation pipe requires only explicit state-file");
    return result;
}
std::string readFile(const std::string& path, size_t limit) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot open pipe input file");
    std::string result;
    char ch;
    while (input.get(ch)) {
        require(result.size() < limit, "pipe input file too large");
        result += ch;
    }
    require(input.eof(), "cannot read pipe input file");
    return result;
}
std::string hex(const uint8_t* data, size_t count) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        result += digits[data[i] >> 4]; result += digits[data[i] & 15];
    }
    return result;
}
fake_brain::Bytes unhex(const std::string& text, size_t maximum) {
    require(text.size() % 2 == 0 && text.size() / 2 <= maximum, "hex size invalid");
    auto nibble = [](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9') return uint8_t(ch - '0');
        if (ch >= 'a' && ch <= 'f') return uint8_t(ch - 'a' + 10);
        if (ch >= 'A' && ch <= 'F') return uint8_t(ch - 'A' + 10);
        throw std::runtime_error("invalid hex byte");
    };
    fake_brain::Bytes result;
    result.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
        result.push_back(uint8_t((nibble(text[i]) << 4) | nibble(text[i + 1])));
    return result;
}
v4::Pairing pairing() {
    v4::Pairing result;
    result.role = v4::Role::Motion;
    std::strcpy(result.deviceId, device);
    std::strcpy(result.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(result.localPhysicalId, "aabbccddeeff");
    std::strcpy(result.peerPhysicalId, "112233445566");
    return result;
}
ProductContext context(const Options& args) {
    ProductContext result;
    if (!args.contextFile.empty()) {
        const auto bytes = readFile(args.contextFile, v4::kMaxMessage);
        require(decodeProductContext(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                                     device, result) && !result.cleared,
                "context-file must contain active feeding_context for bt-main-test");
    } else {
        std::strcpy(result.deviceId, device); result.profileVersion = 1;
        std::strcpy(result.babyId, "baby-original"); std::strcpy(result.babyName, "Original baby");
        std::strcpy(result.formulaBrand, "Test formula");
        result.waterMl = 180; result.temperatureC = 45; result.powderGPer100Ml = 25;
    }
    require(validProductContext(result), "invalid initial pipe context");
    return result;
}
void seed(const Options& args) {
    const auto pair = pairing();
    const auto cached = args.prepare ? ProductContext{} : context(args);
    PairingInstaller installer;
    require(installer.installFirst(v4::Role::Motion, pair) == PairingInstall::Installed,
            "pipe pairing install failed");
    MotionStateStore store;
    require(store.installInitial(pair, args.prepare ? nullptr : &cached) == MotionWrite::Stored, "pipe Store install failed");
    if (!args.seedHistory) return;
    auto brainPair = pair;
    brainPair.role = v4::Role::Brain;
    std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
    for (unsigned i = 1; i <= 4; ++i) {
        ProductRequest request;
        request.source = v4::Source::LocalTouch; request.command = ProductCommand::Prepare;
        request.sequence = i;
        std::strcpy(request.deviceId, device); std::strcpy(request.babyId, cached.babyId);
        request.profileVersion = cached.profileVersion; request.waterMl = cached.waterMl;
        request.temperatureC = cached.temperatureC; request.powderGPer100Ml = cached.powderGPer100Ml;
        require(makeLocalCommandId(brainPair, i, request.commandId), "historical local ID failed");
        char execution[33];
        std::snprintf(execution, sizeof(execution), "%032x", i);
        require(store.recordDecision(request, true, "accepted", execution) == MotionWrite::Stored,
                "historical decision failed");
        const bool completed = i % 2 != 0;
        require(store.finishFeeding(execution, completed, completed ? "" : "motor_fault",
                    completed ? "" : "E_MOTOR", 100 + i) == MotionWrite::Stored &&
                store.archiveFeeding(true) == MotionWrite::Stored, "historical archive failed");
    }
    require(store.state().pendingResultCount == 4 && store.state().localSequence == 4 &&
            store.state().cloudSequence == 0 && store.state().slot.kind == MotionSlotKind::Empty,
            "historical fixture watermarks/slot invalid");
}
bool sdkType(unsigned type, size_t size, bool prepare) {
    return (type <= 2 && (type != 2 || size == 4)) ||
        (prepare && ((type == 6 && size == 2) || (type == 7 && size == 4)));
}
void snapshot(JsonArray output, bool prepare) {
    for (const auto& space : fake_brain::io.disk) {
        for (const auto& entry : space.second) {
            const auto type = unsigned(entry.second.type);
            require(sdkType(type, entry.second.bytes.size(), prepare), "unsupported SDK snapshot type/length");
            auto row = output.createNestedObject();
            row["namespace"] = space.first; row["key"] = entry.first;
            row["type"] = type;
            row["hex"] = hex(entry.second.bytes.data(), entry.second.bytes.size());
        }
    }
}
void restore(const std::string& path, bool prepare) {
    const auto raw = readFile(path, fileLimit);
    DynamicJsonDocument doc(documentCapacity);
    parse(doc, raw);
    require(doc.is<JsonArray>() && doc.size() <= 128,
            "state-file must be SDK snapshot array");
    fake_brain::Database disk;
    for (JsonVariantConst value : doc.as<JsonArrayConst>()) {
        require(value.is<JsonObjectConst>(), "snapshot row must be object");
        const auto row = value.as<JsonObjectConst>();
        require(row.size() == 4 && row["namespace"].is<std::string>() &&
                row["key"].is<std::string>() && row["type"].is<unsigned>() &&
                !row["type"].is<bool>() &&
                row["hex"].is<std::string>(), "snapshot row fields invalid");
        const auto space = row["namespace"].as<std::string>();
        const auto key = row["key"].as<std::string>();
        require(!space.empty() && space.size() <= 15 && space.find('\0') == std::string::npos &&
                !key.empty() && key.size() <= 15 && key.find('\0') == std::string::npos,
                "snapshot NVS name invalid");
        require(!disk[space].count(key), "duplicate snapshot NVS key");
        auto bytes = unhex(row["hex"].as<std::string>(), 4096);
        const auto type = row["type"].as<unsigned>();
        require(sdkType(type, bytes.size(), prepare), "unsupported restored SDK type/length");
        disk[space][key] = {std::move(bytes), static_cast<fake_brain::Type>(type)};
    }
    require(fake_brain::io.handles.empty(), "handles before snapshot restore");
    fake_brain::io.disk = std::move(disk);
}
void verify(bool prepare) {
    fake_motion_nvs::verifyNoEraseOrInit();
    require(fake_brain::io.handles.empty(), "pipe NVS handle leak");
    require(product.executionAuthorized() == prepare, "pipe execution switch disagrees with explicit fixture");
}
void persist(const std::string& path, JsonArrayConst state) {
    if (path.empty()) return;
    const auto temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        require(bool(file), "cannot open state-file temporary");
        serializeJson(state, file);
        file.flush();
        require(bool(file), "cannot write state-file");
    }
    std::filesystem::rename(temp, path);
}
struct Reporter {
    size_t uartCursor = 0, canCursor = 0, httpCursor = 0;
    void report(const Options& args, bool restored, bool quit = false) {
        verify(args.prepare);
        DynamicJsonDocument doc(documentCapacity);
        doc["now_ms"] = motion_io::now;
        doc["uart_tx"] = motion_io::uartTx.empty() ? std::string() :
            hex(motion_io::uartTx.data() + uartCursor, motion_io::uartTx.size() - uartCursor);
        const auto& state = productState.state();
        doc["pending_result_count"] = state.pendingResultCount;
        const auto kind = state.slot.kind;
        doc["execution_slot"] = unsigned(kind);
        doc["target_temp"] = product.targetTemp();
        if (args.prepare) {
            doc["prepare_fixture"] = true; doc["execution_authorized"] = product.executionAuthorized();
            doc["queue_run_id"] = queue.runId();
            doc["execution_id"] = state.slot.executionId;
            doc["demo_stage"] = babytech::display::displayStageKey(demo.stage());
            doc["can_start"] = product.canStart(); doc["stationary"] = productHardware.stationary();
            doc["scale_json"] = std::string(scaleStatusJson().c_str());
            doc["fixture_config"] = motion_main_prepare::config;
            const auto& state = productState.state();
            auto barrier = doc.createNestedObject("context_barrier");
            barrier["present"] = state.context.present; barrier["cleared"] = state.context.cleared;
            barrier["profile_version"] = state.context.profileVersion; barrier["baby_id"] = state.context.babyId;
            barrier["powder_g_per_100ml"] = state.context.powderGPer100Ml;
            barrier["digest"] = hex(state.context.digest, sizeof(state.context.digest));
            auto decision = doc.createNestedObject("cloud_decision");
            decision["kind"] = unsigned(state.cloudResult.kind);
            decision["accepted"] = state.cloudResult.accepted; decision["reason"] = state.cloudResult.reason;
            decision["outcome"] = unsigned(state.cloudResult.outcome);
            decision["digest"] = hex(state.cloudResult.digest, sizeof(state.cloudResult.digest));
            std::array<uint8_t, kRequestIdentityMaxSize> identity{};
            const auto count = state.cloudResult.kind == MotionResultKind::Ordinary
                ? encodeRequestIdentity(state.cloudResult.request, identity.data(), identity.size()) : 0;
            require(state.cloudResult.kind != MotionResultKind::Ordinary || count, "invalid stored request identity");
            decision["request_identity"] = hex(identity.data(), count);
            decision["stop_sequence"] = state.cloudResult.stopSequence;
            decision["stop_command_id"] = state.cloudResult.stopCommandId;
            decision["stop_execution_id"] = state.cloudResult.stopExecutionId;
            auto responses = doc.createNestedArray("http_responses");
            for (size_t i = httpCursor; i < server.responses.size(); ++i) {
                const auto& response = server.responses[i];
                auto row = responses.createNestedObject();
                row["path"] = response.uri; row["status"] = response.status; row["body"] = response.body;
                row["handler_called"] = response.handlerCalled; row["send_calls"] = response.sendCalls;
            }
        }
        doc["low_water_pin"] = BABYTECH_LOW_WATER_PIN;
        doc["low_water_valid"] = lowWaterValid; doc["low_water"] = lowWater;
        doc["cloud_sequence"] = state.cloudSequence; doc["local_sequence"] = state.localSequence;
        doc["recovery_pending"] = productRecovery.executionPending();
        doc["motion_pending"] = productRecovery.motionPending();
        doc["restored"] = restored; doc["quit"] = quit;
        doc["boot_seed"] = args.bootSeed;
        doc["diagnostic_boot_id"] = debugLog.bootId();
        const auto pair = productBoardLink.verifiedPairing();
        if (args.installation) {
            doc["store_ready"] = productState.ready(); doc["runtime_paired"] = pair != nullptr;
            doc["queue_run_id"] = queue.runId();
            doc["maintenance"] = productBoardLink.maintenance().active();
            v4::Pairing stored;
            const auto loaded = loadBoardPairing(v4::Role::Motion, stored);
            doc["pairing_load"] = unsigned(loaded);
            if (loaded == PairingLoad::Ready) {
                auto output = doc.createNestedObject("pairing");
                output["device_id"] = stored.deviceId; output["epoch"] = stored.epoch;
                output["local"] = stored.localPhysicalId; output["peer"] = stored.peerPhysicalId;
            }
            if (productState.ready() && state.context.present) {
                doc["context_digest"] = hex(state.context.digest, sizeof(state.context.digest));
                doc["context_version"] = state.context.profileVersion;
                doc["context_cleared"] = state.context.cleared;
                doc["baby_id"] = state.context.babyId;
                doc["powder_g_per_100ml"] = state.context.powderGPer100Ml;
            }
            require(state.pendingResultCount == 0 && state.slot.kind == MotionSlotKind::Empty,
                    "installation fixture unexpectedly executed product motion");
        } else require(pair && productState.ready(), "pipe pairing/Store unavailable after boot");
        auto ids = doc.createNestedArray("result_event_ids");
        auto results = doc.createNestedArray("events");
        for (size_t i = 0; i < state.pendingResultCount; ++i) {
            const auto& slot = state.pendingResults[i];
            ids.add(slot.eventId);
            TerminalEvent event; v4::Message message;
            require(terminalEventFromSlot(*pair, slot, event) && encodeTerminalEvent(*pair, event, message),
                    "cannot encode original historical terminal");
            auto result = results.createNestedObject();
            result["event_id"] = slot.eventId;
            result["payload"] = std::string(reinterpret_cast<const char*>(message.payload), message.length);
        }
        auto frames = doc.createNestedArray("can_tx");
        for (size_t i = canCursor; i < motion_io::canTx.size(); ++i) {
            const auto& frame = motion_io::canTx[i];
            auto row = frames.createNestedObject();
            row["identifier"] = frame.identifier; row["extd"] = bool(frame.extd);
            row["rtr"] = bool(frame.rtr);
            row["ss"] = bool(frame.ss);
            auto data = row.createNestedArray("data");
            for (size_t n = 0; n < frame.data_length_code; ++n) data.add(frame.data[n]);
        }
        snapshot(doc.createNestedArray("snapshot"), args.prepare);
        require(!doc.overflowed(), "pipe report overflow");
        persist(args.stateFile, doc["snapshot"].as<JsonArrayConst>());
        std::string encoded;
        serializeJson(doc, encoded);
        std::cout << "DUAL_MOTION=" << encoded << '\n' << std::flush;
        uartCursor = motion_io::uartTx.size(); canCursor = motion_io::canTx.size();
        httpCursor = server.responses.size();
    }
};
struct Input {
    fake_brain::Bytes uart;
    unsigned advance = 0, missing = 0, moving = 0, lowWaterLevel = HIGH;
    bool feedback = false, quit = false;
    int32_t hxRaw = motion_io::hxRaw;
    bool hxReady = motion_io::hxReady;
    std::optional<WebServer::Request> http;
};
Input input(const std::string& line) {
    DynamicJsonDocument doc(65536);
    parse(doc, line);
    require(doc.is<JsonObject>(), "pipe input must be JSON object");
    const auto object = doc.as<JsonObjectConst>();
    for (JsonPairConst field : object) {
        const std::string key = field.key().c_str();
        require(key == "uart_rx" || key == "advance_ms" || key == "feedback" ||
                key == "missing_axis" || key == "moving_axis" || key == "low_water_level" || key == "quit" ||
                (BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW == 1 &&
                 (key == "hx_raw" || key == "hx_ready" || key == "http")),
                "unknown pipe input field");
    }
    Input result;
    result.feedback = motion_io::automaticFeedback;
    result.missing = motion_io::missingId; result.moving = motion_io::movingId;
    result.lowWaterLevel = motion_io::lowWaterLevel;
    if (object.containsKey("uart_rx")) {
        require(object["uart_rx"].is<std::string>(), "uart_rx must be hex string");
        result.uart = unhex(object["uart_rx"].as<std::string>(), 8192);
    }
    auto number = [&](const char* key, unsigned& value, unsigned maximum) {
        if (!object.containsKey(key)) return;
        require(object[key].is<unsigned>() && !object[key].is<bool>() &&
                object[key].as<unsigned>() <= maximum, "pipe integer out of range/type");
        value = object[key].as<unsigned>();
    };
    number("advance_ms", result.advance, 1000);
    number("missing_axis", result.missing, 5); number("moving_axis", result.moving, 5);
    if (object.containsKey("low_water_level")) {
        require(BABYTECH_LOW_WATER_PIN == 21, "low_water_level requires explicit host water fixture");
        number("low_water_level", result.lowWaterLevel, 1);
    }
    for (const char* key : {"feedback", "quit"})
        if (object.containsKey(key)) require(object[key].is<bool>(), "pipe boolean type invalid");
    if (object.containsKey("feedback")) result.feedback = object["feedback"].as<bool>();
    if (object.containsKey("quit")) result.quit = object["quit"].as<bool>();
    if (object.containsKey("hx_raw")) {
        const auto value = object["hx_raw"];
        require(value.is<int32_t>() && !value.is<bool>() && value.as<int32_t>() > -8388608 &&
                value.as<int32_t>() < 8388607, "HX raw must be signed 24-bit sample");
        result.hxRaw = value.as<int32_t>();
    }
    if (object.containsKey("hx_ready")) {
        require(object["hx_ready"].is<bool>(), "HX ready must be bool");
        result.hxReady = object["hx_ready"].as<bool>();
    }
    if (object.containsKey("http")) {
        const auto request = object["http"];
        require(request.is<JsonObjectConst>() && request.size() == 3 &&
                request["path"].is<std::string>() && request["method"].is<std::string>() &&
                request["arguments"].is<JsonObjectConst>(), "invalid HTTP SDK request");
        WebServer::Request parsed;
        parsed.uri = request["path"].as<std::string>();
        const auto method = request["method"].as<std::string>();
        require((method == "GET" || method == "POST") && !parsed.uri.empty() &&
                parsed.uri.size() <= 128 && parsed.uri.find('\0') == std::string::npos,
                "invalid HTTP SDK method/path");
        parsed.method = method == "GET" ? HTTP_GET : HTTP_POST;
        for (JsonPairConst argument : request["arguments"].as<JsonObjectConst>()) {
            require(argument.value().is<std::string>(), "HTTP SDK argument must be text");
            const std::string key = argument.key().c_str(), value = argument.value().as<std::string>();
            require(!key.empty() && key.size() <= 64 && key.size() == argument.key().size() &&
                    value.size() <= 8192 && value.find('\0') == std::string::npos,
                    "invalid HTTP SDK argument length");
            parsed.arguments.emplace_back(key, value);
        }
        result.http = std::move(parsed);
    }
    require(!result.quit || (result.uart.empty() && !result.advance && object.size() == 1),
            "quit must be a standalone input");
    return result;
}
bool readLine(std::string& line) {
    line.clear();
    char ch;
    while (std::cin.get(ch)) {
        if (ch == '\n') return true;
        require(line.size() < 32768, "pipe input line too large");
        line += ch;
    }
    require(std::cin.eof(), "pipe stdin read failed");
    return !line.empty();
}
int run(int argc, char** argv) {
    const auto args = options(argc, argv);
    fake_motion_nvs::reset();
    const bool restored = !args.stateFile.empty() && std::filesystem::exists(args.stateFile);
    if (restored) restore(args.stateFile, args.prepare);
    else {
        require(!args.installation, "installation snapshot must exist");
        seed(args);
    }
    require(fake_brain::io.handles.empty(), "pipe fixture handles before setup");
    motion_io::randomCounter = args.bootSeed;
    motion_io::flowFeedback = args.prepare;
    setup();
    Reporter reporter;
    reporter.report(args, restored);
    std::string line;
    while (readLine(line)) {
        const auto next = input(line);
        if (next.quit) { reporter.report(args, restored, true); return 0; }
        motion_io::automaticFeedback = next.feedback;
        motion_io::missingId = next.missing; motion_io::movingId = next.moving;
        motion_io::lowWaterLevel = next.lowWaterLevel;
        if (args.installation && next.feedback) {
            // SDK receives actual driver packet shapes, as in the existing
            // maintenance main tests. No controller safety flags are assigned.
            for (uint8_t id = 1; id <= 5; ++id) {
                if (id == next.missing) continue;
                motion_io::reply(id, {0x36, 0, 0, 0, 0, 0, 0x6b});
                motion_io::reply(id, {0x35, 0, 0, uint8_t(id == next.moving ? 30 : 0), 0x6b});
                motion_io::reply(id, {0x3a, 1, 0x6b});
            }
        }
        motion_io::hxRaw = next.hxRaw; motion_io::hxReady = next.hxReady;
        if (next.http) server.enqueue(*next.http);
        motion_io::uartRx.insert(motion_io::uartRx.end(), next.uart.begin(), next.uart.end());
        const auto start = motion_io::now;
        // loop's actual SDK delay(1) counts toward the requested elapsed time.
        // Even advance_ms=0 runs one loop and therefore consumes that 1 ms.
        do {
            const auto elapsed = uint32_t(motion_io::now - start);
            const auto remaining = next.advance > elapsed ? next.advance - elapsed : 0;
            motion_io::now += std::min(remaining ? remaining - 1 : 0, 9u);
            loop();
        } while (uint32_t(motion_io::now - start) < next.advance);
        reporter.report(args, restored);
    }
    reporter.report(args, restored, true);
    return 0;
}
}  // namespace motion_main_dual_pipe
