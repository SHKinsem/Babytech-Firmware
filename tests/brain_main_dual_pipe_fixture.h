#pragma once
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace {
constexpr size_t dualInputLimit = 196608;
constexpr size_t dualSnapshotLimit = 262144;
constexpr size_t dualDiskBytesLimit = 65536;
constexpr size_t dualUartLimit = 8192;
constexpr size_t dualOutputLimit = 524288;
constexpr uint32_t dualSeedMax = std::numeric_limits<uint32_t>::max() - 32;

// ArduinoJson also accepts non-JSON keys/quotes and duplicate object keys.
// Keep orchestration fixtures strict before the SDK-facing typed validation.
class DualJsonSyntax {
public:
    explicit DualJsonSyntax(const std::string& text) : text_(text) {}
    void validate() {
        value(0); whitespace(); check(at_ == text_.size(), "trailing real pipe JSON");
    }
private:
    const std::string& text_;
    size_t at_ = 0;
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    void whitespace() {
        while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') ++at_;
    }
    bool take(char byte) {
        if (peek() != byte) return false;
        ++at_; return true;
    }
    void expect(char byte) { check(take(byte), "invalid real pipe JSON syntax"); }
    bool digit() const { return peek() >= '0' && peek() <= '9'; }
    std::string stringToken() {
        const auto start = at_;
        expect('"');
        while (peek() != '"') {
            check(at_ < text_.size() && static_cast<unsigned char>(peek()) >= 32,
                  "invalid real pipe JSON string");
            if (take('\\')) {
                const char escape = peek();
                check(escape && std::strchr("\"\\/bfnrtu", escape), "invalid real pipe JSON escape");
                ++at_;
                if (escape == 'u') {
                    for (unsigned i = 0; i < 4; ++i) {
                        const char byte = peek();
                        check((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
                              (byte >= 'A' && byte <= 'F'), "invalid real pipe JSON unicode escape");
                        ++at_;
                    }
                }
            } else ++at_;
        }
        expect('"');
        return text_.substr(start, at_ - start);
    }
    void value(unsigned depth) {
        check(depth <= 8, "real pipe JSON nesting budget exceeded");
        whitespace();
        if (take('{')) {
            std::set<std::string> keys;
            whitespace();
            if (take('}')) return;
            do {
                whitespace();
                const auto token = stringToken();
                check(token.size() <= 256, "oversized real pipe JSON key");
                DynamicJsonDocument key(1024);
                check(!deserializeJson(key, token), "invalid real pipe JSON key");
                const auto decoded = key.as<JsonString>();
                check(keys.emplace(decoded.c_str(), decoded.size()).second, "duplicate real pipe JSON key");
                whitespace(); expect(':'); value(depth + 1); whitespace();
            } while (take(','));
            expect('}'); return;
        }
        if (take('[')) {
            whitespace();
            if (take(']')) return;
            do { value(depth + 1); whitespace(); } while (take(','));
            expect(']'); return;
        }
        if (peek() == '"') { stringToken(); return; }
        for (const auto literal : {"true", "false", "null"}) {
            const auto length = std::strlen(literal);
            if (text_.compare(at_, length, literal) == 0) { at_ += length; return; }
        }
        take('-');
        check(digit(), "invalid real pipe JSON value");
        if (!take('0')) while (digit()) ++at_;
        if (take('.')) { check(digit(), "invalid real pipe JSON fraction"); while (digit()) ++at_; }
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            check(digit(), "invalid real pipe JSON exponent"); while (digit()) ++at_;
        }
    }
};

std::string dualReadBounded(std::istream& stream, size_t limit, bool line, bool& present) {
    std::string result;
    char byte;
    present = false;
    while (stream.get(byte)) {
        present = true;
        if (line && byte == '\n') break;
        check(result.size() < limit, "real pipe input exceeds test budget");
        result.push_back(byte);
    }
    check(!stream.bad(), "real pipe input read failed");
    return result;
}
DynamicJsonDocument dualJson(const std::string& value, size_t limit) {
    check(value.size() <= limit, "real pipe JSON exceeds test budget");
    DualJsonSyntax(value).validate();
    DynamicJsonDocument doc(dualOutputLimit);
    std::istringstream stream(value);
    check(!deserializeJson(doc, stream, DeserializationOption::NestingLimit(8)), "invalid real pipe JSON");
    stream >> std::ws;
    check(stream.eof() && !doc.overflowed(), "trailing or oversized real pipe JSON");
    return doc;
}
std::string dualText(JsonVariantConst value, size_t limit) {
    check(value.is<const char*>(), "real pipe text must be a string");
    const auto text = value.as<JsonString>();
    check(text.size() <= limit && std::strlen(text.c_str()) == text.size(),
          "real pipe text is oversized or contains NUL");
    return std::string(text.c_str(), text.size());
}
bool dualName(const std::string& name) {
    if (name.empty() || name.size() > 15) return false;
    for (const unsigned char byte : name)
        if (byte < 33 || byte > 126) return false;
    return true;
}
int dualNibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}
nvs::Bytes dualHexBytes(JsonVariantConst value, size_t limit) {
    const auto text = dualText(value, limit * 2);
    check(text.size() % 2 == 0, "real pipe hex must contain complete bytes");
    nvs::Bytes bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = dualNibble(text[i]), lo = dualNibble(text[i + 1]);
        check(hi >= 0 && lo >= 0, "real pipe hex must be lowercase hexadecimal");
        bytes.push_back(uint8_t((hi << 4) | lo));
    }
    return bytes;
}
std::string dualHex(const uint8_t* bytes, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result.push_back(digits[bytes[i] >> 4]); result.push_back(digits[bytes[i] & 15]);
    }
    return result;
}
nvs::Database dualParseSnapshot(const std::string& value) {
    auto doc = dualJson(value, dualSnapshotLimit);
    check(doc.is<JsonArray>(), "real pipe snapshot must be an array");
    const auto records = doc.as<JsonArrayConst>();
    check(records.size() <= 256, "real pipe snapshot has too many keys");
    nvs::Database disk;
    size_t total = 0;
    for (const auto record : records) {
        check(record.is<JsonObjectConst>() && record.size() == 4 &&
              record["type"].is<unsigned>() && record["type"].as<unsigned>() <= 2,
              "invalid real pipe snapshot record");
        const auto space = dualText(record["namespace"], 15), key = dualText(record["key"], 15);
        check(dualName(space) && dualName(key), "invalid real pipe NVS name");
        auto bytes = dualHexBytes(record["hex"], dualUartLimit);
        total += bytes.size();
        check(total <= dualDiskBytesLimit, "real pipe snapshot byte budget exceeded");
        const auto type = record["type"].as<unsigned>();
        check(type != 2 || bytes.size() == sizeof(uint32_t), "invalid real pipe U32 bytes");
        check(disk[space].emplace(key, nvs::Value{std::move(bytes), static_cast<nvs::Type>(type)}).second,
              "duplicate real pipe NVS key");
        check(disk.size() <= 32 && disk[space].size() <= 64, "real pipe snapshot namespace budget exceeded");
    }
    return disk;
}
void dualWriteSnapshot(JsonArray records, const nvs::Database& disk = nvs::io.disk) {
    size_t count = 0, total = 0;
    check(disk.size() <= 32, "real pipe disk namespace budget exceeded");
    for (const auto& space : disk) {
        check(dualName(space.first) && space.second.size() <= 64, "invalid real pipe disk namespace");
        for (const auto& entry : space.second) {
            const auto& bytes = entry.second.bytes;
            total += bytes.size();
            const auto type = static_cast<unsigned>(entry.second.type);
            check(++count <= 256 && total <= dualDiskBytesLimit && bytes.size() <= dualUartLimit &&
                  dualName(entry.first) && type <= 2 && (type != 2 || bytes.size() == sizeof(uint32_t)),
                  "real pipe disk exceeds snapshot budget");
            auto record = records.createNestedObject();
            record["namespace"] = space.first; record["key"] = entry.first;
            record["type"] = type; record["hex"] = dualHex(bytes.data(), bytes.size());
        }
    }
}
struct DualStep {
    uint32_t advance = 0;
    std::optional<bool> connected;
    std::vector<fake::Packet> incoming;
    nvs::Bytes uart;
    bool quit = false;
};
DualStep dualParseStep(const std::string& value) {
    auto input = dualJson(value, dualInputLimit);
    check(input.is<JsonObject>(), "real pipe step must be an object");
    for (const auto item : input.as<JsonObjectConst>()) {
        const std::string key = item.key().c_str();
        check(key.size() == item.key().size(), "invalid real pipe step key");
        check(key == "advance_ms" || key == "connected" || key == "incoming" ||
              key == "uart_rx" || key == "quit", "unknown real pipe step field");
    }
    DualStep step;
    if (input.containsKey("advance_ms")) {
        check(input["advance_ms"].is<uint32_t>() && input["advance_ms"].as<uint32_t>() <= 1000,
              "invalid real pipe advance_ms");
        step.advance = input["advance_ms"].as<uint32_t>();
    }
    if (input.containsKey("connected")) {
        check(input["connected"].is<bool>(), "invalid real pipe connected flag");
        step.connected = input["connected"].as<bool>();
    }
    if (input.containsKey("quit")) {
        check(input["quit"].is<bool>(), "invalid real pipe quit flag");
        step.quit = input["quit"].as<bool>();
    }
    if (input.containsKey("uart_rx")) step.uart = dualHexBytes(input["uart_rx"], dualUartLimit);
    if (input.containsKey("incoming")) {
        check(input["incoming"].is<JsonArray>(), "real pipe incoming must be an array");
        const auto packets = input["incoming"].as<JsonArrayConst>();
        check(packets.size() <= 16, "real pipe packet count exceeds test budget");
        for (const auto packet : packets) {
            check(packet.is<JsonObjectConst>() && packet.size() == 3 && packet["retained"].is<bool>(),
                  "invalid real pipe packet fields");
            const auto topic = dualText(packet["topic"], 128);
            check(topic == prefix + "config" || topic == prefix + "command", "unexpected real pipe topic");
            step.incoming.push_back({topic, dualText(packet["payload"], 8192), packet["retained"].as<bool>()});
        }
    }
    return step;
}
struct DualOptions {
    std::optional<nvs::Database> disk;
    uint32_t seed = 0;
};
DualOptions realBridgeOptions(int argc, char** argv) {
    DualOptions options;
    bool stateSeen = false, seedSeen = false;
    for (int i = 2; i < argc; i += 2) {
        check(i + 1 < argc, "real pipe option requires a value");
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--state-file") {
            check(!stateSeen && !value.empty(), "invalid or repeated real pipe state-file");
            stateSeen = true;
            std::ifstream file(value, std::ios::binary);
            check(file.is_open(), "cannot open real pipe state-file");
            bool present;
            options.disk = dualParseSnapshot(dualReadBounded(file, dualSnapshotLimit, false, present));
        } else if (key == "--boot-id") {
            check(!seedSeen && !value.empty() && value.size() <= 10, "invalid or repeated real pipe boot-id");
            seedSeen = true;
            uint64_t number = 0;
            for (const char byte : value) {
                check(byte >= '0' && byte <= '9', "real pipe boot-id must be decimal");
                number = number * 10 + unsigned(byte - '0');
            }
            check(number >= 1 && number <= dualSeedMax, "real pipe boot-id outside SDK seed budget");
            options.seed = uint32_t(number);
        } else throw std::runtime_error("unknown real pipe option");
    }
    if (!seedSeen) {
        std::random_device random;
        do { options.seed = uint32_t(random()); } while (!options.seed || options.seed > dualSeedMax);
    }
    return options;
}
void runRealBridge(const DualOptions& options) {
    seed(false);
    fake::io.delayLimit = 3000;
    // Nominal pipe uses the advertised room; regular short-write tests keep 7.
    // Throttling 7 bytes per 15ms UI step exceeds the real 50ms first-frame guard.
    fake_main::uartWriteLimit = 23;
    if (options.disk) nvs::io.disk = *options.disk;
    fake_main::randomCounter = options.seed;
    setup();
    check(productState.ready() && fake::io.tasks.size() == 1 && !simulating(), "real pipe startup failed");
    fake::io.onDelay = [&](unsigned) {
        const bool worker = fake::io.inWorker;
        fake::io.inWorker = false;
        check(!simulating(), "real pipe unexpectedly entered simulation");
        check(fake_main::uartTx.size() <= dualUartLimit && fake::io.published.size() <= 64 &&
              fake::io.subscriptions.size() <= 64, "real pipe output exceeds test budget");
        DynamicJsonDocument report(dualOutputLimit);
        report["now_ms"] = millis(); report["connected"] = network.connected();
        report["simulation"] = false; report["random_seed"] = options.seed;
        report["uart_sdk_write_limit"] = fake_main::uartWriteLimit;
        report["sdk_pending_packets"] = fake::io.incoming.size();
        report["sdk_queued_items"] = fake::queuedItems();
        report["sdk_queue_send_failures"] = fake::io.queueSendFailures;
        report["motion_connected"] = controllerLink.connected(millis());
        report["has_motion_snapshot"] = controllerLink.hasSnapshot();
        report["telemetry_seen"] = controllerLink.lastTelemetry() != nullptr;
        if (controllerLink.lastTelemetry())
            report["telemetry_received_ms"] = controllerLink.lastTelemetryReceivedAtMs();
        report["profile_version"] = productState.state().context.profileVersion;
        report["context_cleared"] = productState.state().context.cleared;
        report["baby_id"] = productState.state().context.babyId;
        report["pending"] = productState.state().pending;
        report["local_sequence"] = std::to_string(productState.state().localSequence);
        report["uart_tx"] = dualHex(fake_main::uartTx.data(), fake_main::uartTx.size());
        auto subscriptions = report.createNestedArray("subscriptions");
        for (const auto& topic : fake::io.subscriptions) {
            check(topic.size() <= 128, "oversized real pipe subscription"); subscriptions.add(topic);
        }
        auto packets = report.createNestedArray("published");
        for (const auto& packet : fake::io.published) {
            check(packet.topic.size() <= 128 && packet.payload.size() <= 8192, "oversized real pipe publication");
            auto item = packets.createNestedObject();
            item["topic"] = packet.topic; item["payload"] = packet.payload; item["retained"] = packet.retained;
        }
        dualWriteSnapshot(report.createNestedArray("snapshot"));
        check(!report.overflowed() && measureJson(report) <= dualOutputLimit, "real pipe report overflow");
        std::cout << "DUAL_BRAIN=" << encode(report) << std::endl;
        fake::io.published.clear(); fake::io.subscriptions.clear(); fake_main::uartTx.clear();
        bool present;
        const auto line = dualReadBounded(std::cin, dualInputLimit, true, present);
        if (!present) throw fake::StopWorker{};
        const auto step = dualParseStep(line);
        check(fake_main::uartRx.size() + step.uart.size() <= dualUartLimit &&
              fake::io.incoming.size() + step.incoming.size() <= 16, "real pipe input backlog exceeds test budget");
        if (step.quit) throw fake::StopWorker{};
        // All parsing and budget checks finish before clock, SDK or UART changes.
        fake::io.now += step.advance;
        if (step.connected) fake::io.connectOk = fake::io.loopOk = *step.connected;
        for (const auto& packet : step.incoming) fake::io.incoming.push_back(packet);
        fake_main::uartRx.insert(fake_main::uartRx.end(), step.uart.begin(), step.uart.end());
        loop();
        fake::io.inWorker = worker;
    };
    fake::runWorker();
    check(!nvs::count(nvs::Op::Erase) && !nvs::count(nvs::Op::Init) && nvs::io.handles.empty(),
          "real pipe unsafe NVS operation/leak");
    for (const auto& call : WiFi.calls) check(call.worker, "real pipe Wi-Fi escaped worker");
    check(!fake::io.preferenceWriteCalls, "real pipe changed seeded credentials");
    fake::cleanupLifetimeResources();
}
void checkRealBridgeInputs() {
    const auto step = dualParseStep("{\"advance_ms\":1000,\"connected\":false,\"uart_rx\":\"00abff\","
        "\"incoming\":[{\"topic\":\"devices/bt-main-test/config\",\"payload\":\"{ \\\"x\\\": 1 }\",\"retained\":true}]}");
    check(step.advance == 1000 && step.connected == false && step.uart == nvs::Bytes({0, 0xab, 0xff}) &&
          step.incoming.size() == 1 && step.incoming[0].payload == "{ \"x\": 1 }", "real pipe altered input bytes");
    const auto disk = dualParseSnapshot("[{\"namespace\":\"fixture\",\"key\":\"blob\",\"type\":0,\"hex\":\"00abff00\"},"
        "{\"namespace\":\"fixture\",\"key\":\"str\",\"type\":1,\"hex\":\"6100\"},"
        "{\"namespace\":\"fixture\",\"key\":\"u32\",\"type\":2,\"hex\":\"12345678\"}]");
    check(disk.at("fixture").at("blob").bytes == nvs::Bytes({0, 0xab, 0xff, 0}) &&
          disk.at("fixture").at("str").type == nvs::Type::String &&
          disk.at("fixture").at("u32").bytes == nvs::Bytes({0x12, 0x34, 0x56, 0x78}), "snapshot changed typed bytes");
    DynamicJsonDocument snapshot(4096);
    dualWriteSnapshot(snapshot.to<JsonArray>(), disk);
    check(dualParseSnapshot(encode(snapshot)) == disk, "snapshot typed-byte round trip failed");
    const auto originalDisk = nvs::io.disk;
    const auto originalNow = fake::io.now;
    for (const auto& value : {"[]", "null", "{} {}", "{quit:true}", "{'quit':true}",
            "{\"quit\":false,\"quit\":true}", "{\"quit\":false,\"\\u0071uit\":true}",
            "{\"advance_ms\":01}", "{\"advance_ms\":+1}", "{\"quit\":1}", "{\"advance_ms\":true}",
            "{\"advance_ms\":-1}", "{\"advance_ms\":1.5}", "{\"advance_ms\":1001}", "{\"connected\":0}",
            "{\"uart_rx\":\"0\"}", "{\"uart_rx\":\"AA\"}", "{\"uart_rx\":null}", "{\"usb\":\"SIM ON\"}",
            "{\"state\":[]}", "{\"incoming\":{}}", "{\"incoming\":[null]}",
            "{\"incoming\":[{\"topic\":\"devices/bt-main-test/status\",\"payload\":\"{}\",\"retained\":false}]}",
            "{\"incoming\":[{\"topic\":\"devices/bt-main-test/config\",\"payload\":42,\"retained\":false}]}",
            "{\"incoming\":[{\"topic\":\"devices/bt-main-test/config\",\"payload\":\"{}\",\"retained\":1}]}",
            "{\"uart_rx\":\"00\\u000000\"}", "{\"connected\":true,\"advance_ms\":100,\"uart_rx\":\"zz\"}"}) {
        bool rejected = false;
        try { dualParseStep(value); } catch (const std::exception&) { rejected = true; }
        check(rejected, "real pipe accepted malformed step");
    }
    for (const auto& value : {"{}", "[] []", "[null]",
            "[{\"namespace\":\"n\",\"key\":\"k\",\"type\":true,\"hex\":\"00\"}]",
            "[{\"namespace\":\"n\",\"key\":\"k\",\"type\":3,\"hex\":\"00\"}]",
            "[{\"namespace\":\"n\",\"key\":\"k\",\"type\":2,\"hex\":\"00\"}]",
            "[{\"namespace\":\"n\",\"key\":\"k\",\"type\":0,\"hex\":\"FF\"}]",
            "[{\"namespace\":\"\",\"key\":\"k\",\"type\":0,\"hex\":\"00\"}]",
            "[{\"namespace\":\"n\",\"key\":\"k\",\"type\":0,\"hex\":\"00\"},"
              "{\"namespace\":\"n\",\"key\":\"k\",\"type\":0,\"hex\":\"01\"}]"}) {
        bool rejected = false;
        try { dualParseSnapshot(value); } catch (const std::exception&) { rejected = true; }
        check(rejected, "real pipe accepted malformed snapshot");
    }
    bool rejected = false;
    try { dualParseStep("{\"uart_rx\":\"" + std::string((dualUartLimit + 1) * 2, '0') + "\"}"); }
    catch (const std::exception&) { rejected = true; }
    check(rejected && nvs::io.disk == originalDisk && fake::io.now == originalNow &&
          fake_main::uartRx.empty() && fake::io.incoming.empty(), "real pipe validation had side effects");
}
}  // namespace
