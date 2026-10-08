// Host SDK transport only: production Motion setup/loop owns UART and results.
// Historical terminals are initial Store fixtures, never simulated live feeds.
#pragma once
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace motion_main_dual_pipe {
static_assert(BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW == 0, "host Motion must not enable consumable execution");
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
    bool seedHistory = false;
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
        if (arg == "--seed-history") {
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
    const auto cached = context(args);
    PairingInstaller installer;
    require(installer.installFirst(v4::Role::Motion, pair) == PairingInstall::Installed,
            "pipe pairing install failed");
    MotionStateStore store;
    require(store.installInitial(pair, &cached) == MotionWrite::Stored, "pipe Store install failed");
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
void snapshot(JsonArray output) {
    for (const auto& space : fake_brain::io.disk) {
        for (const auto& entry : space.second) {
            const auto type = unsigned(entry.second.type);
            require(type <= 2, "snapshot has unsupported SDK type (expected blob/string/u32)");
            auto row = output.createNestedObject();
            row["namespace"] = space.first; row["key"] = entry.first;
            row["type"] = type;
            row["hex"] = hex(entry.second.bytes.data(), entry.second.bytes.size());
        }
    }
}
void restore(const std::string& path) {
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
                !row["type"].is<bool>() && row["type"].as<unsigned>() <= 2 &&
                row["hex"].is<std::string>(), "snapshot row fields invalid");
        const auto space = row["namespace"].as<std::string>();
        const auto key = row["key"].as<std::string>();
        require(!space.empty() && space.size() <= 15 && space.find('\0') == std::string::npos &&
                !key.empty() && key.size() <= 15 && key.find('\0') == std::string::npos,
                "snapshot NVS name invalid");
        require(!disk[space].count(key), "duplicate snapshot NVS key");
        disk[space][key] = {unhex(row["hex"].as<std::string>(), 4096),
                            static_cast<fake_brain::Type>(row["type"].as<unsigned>())};
    }
    require(fake_brain::io.handles.empty(), "handles before snapshot restore");
    fake_brain::io.disk = std::move(disk);
}
void verify() {
    fake_motion_nvs::verifyNoEraseOrInit();
    require(fake_brain::io.handles.empty(), "pipe NVS handle leak");
    require(!product.executionAuthorized(), "pipe non-consumable macro must remain zero");
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
    size_t uartCursor = 0, canCursor = 0;
    void report(const Options& args, bool restored, bool quit = false) {
        verify();
        DynamicJsonDocument doc(documentCapacity);
        doc["now_ms"] = motion_io::now;
        doc["uart_tx"] = motion_io::uartTx.empty() ? std::string() :
            hex(motion_io::uartTx.data() + uartCursor, motion_io::uartTx.size() - uartCursor);
        const auto& state = productState.state();
        doc["pending_result_count"] = state.pendingResultCount;
        const auto kind = state.slot.kind;
        doc["execution_slot"] = unsigned(kind);
        doc["cloud_sequence"] = state.cloudSequence; doc["local_sequence"] = state.localSequence;
        doc["recovery_pending"] = productRecovery.executionPending();
        doc["motion_pending"] = productRecovery.motionPending();
        doc["restored"] = restored; doc["quit"] = quit;
        doc["boot_seed"] = args.bootSeed;
        doc["diagnostic_boot_id"] = debugLog.bootId();
        const auto pair = productBoardLink.verifiedPairing();
        require(pair && productState.ready(), "pipe pairing/Store unavailable after boot");
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
        snapshot(doc.createNestedArray("snapshot"));
        require(!doc.overflowed(), "pipe report overflow");
        persist(args.stateFile, doc["snapshot"].as<JsonArrayConst>());
        std::string encoded;
        serializeJson(doc, encoded);
        std::cout << "DUAL_MOTION=" << encoded << '\n' << std::flush;
        uartCursor = motion_io::uartTx.size(); canCursor = motion_io::canTx.size();
    }
};
struct Input {
    fake_brain::Bytes uart;
    unsigned advance = 0, missing = 0, moving = 0;
    bool feedback = false, quit = false;
};
Input input(const std::string& line) {
    DynamicJsonDocument doc(65536);
    parse(doc, line);
    require(doc.is<JsonObject>(), "pipe input must be JSON object");
    const auto object = doc.as<JsonObjectConst>();
    for (JsonPairConst field : object) {
        const std::string key = field.key().c_str();
        require(key == "uart_rx" || key == "advance_ms" || key == "feedback" ||
                key == "missing_axis" || key == "moving_axis" || key == "quit", "unknown pipe input field");
    }
    Input result;
    result.feedback = motion_io::automaticFeedback;
    result.missing = motion_io::missingId; result.moving = motion_io::movingId;
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
    for (const char* key : {"feedback", "quit"})
        if (object.containsKey(key)) require(object[key].is<bool>(), "pipe boolean type invalid");
    if (object.containsKey("feedback")) result.feedback = object["feedback"].as<bool>();
    if (object.containsKey("quit")) result.quit = object["quit"].as<bool>();
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
    if (restored) restore(args.stateFile);
    else seed(args);
    require(fake_brain::io.handles.empty(), "pipe fixture handles before setup");
    motion_io::randomCounter = args.bootSeed;
    setup();
    Reporter reporter;
    reporter.report(args, restored);
    std::string line;
    while (readLine(line)) {
        const auto next = input(line);
        if (next.quit) { reporter.report(args, restored, true); return 0; }
        motion_io::automaticFeedback = next.feedback;
        motion_io::missingId = next.missing; motion_io::movingId = next.moving;
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
