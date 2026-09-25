#pragma once
#include <stddef.h>
#include <stdint.h>

namespace babytech { namespace v2 {
constexpr size_t kMaxPayload = 128, kHeaderSize = 19, kMaxFrameSize = 149;
constexpr uint32_t kByteTimeoutMs = 100, kLinkTimeoutMs = 1500;
enum class Cmd : uint8_t { Read = 1, Write = 2, Exec = 3, Stop = 4 };
enum class Kind : uint8_t { Request, Response, Event };
enum class Outcome : uint8_t { Ok, Accepted, Done, Rejected, Failed, Cancelled };
enum class Reason : uint16_t {
    None, Busy, InvalidParam, InvalidState, NotReady, FaultActive, Unsupported,
    ConfigMismatch, Timeout, FeedbackStale, RequestConflict, ResultExpired,
    BootMismatch, StopUnconfirmed, InternalError
};
enum class State : uint8_t { NotConfigured, Idle, Running, Stopping, Fault };
constexpr uint8_t kSystem = 1, kStage = 4, kMotor = 5;
constexpr uint16_t kInfo = 1, kStatus = 2, kFirstResult = 0x100;
// Stage 1 is a bounded single-motor relative move, not a configured mechanism.
constexpr uint16_t kMoveStage = 1, kRun = 1, kEnable = 1, kDisable = 2;
constexpr uint32_t kCapabilities = 0x0F; // info/status, RAM stage, move/enable, stop

struct Frame {
    Cmd cmd = Cmd::Read;
    Kind kind = Kind::Request;
    uint64_t session = 0;
    uint32_t sequence = 0;
    uint16_t length = 0;
    uint8_t payload[kMaxPayload]{};
};
class Writer {
public:
    Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
    void put(uint64_t value, size_t bytes);
    void raw(const uint8_t* value, size_t bytes);
    size_t size() const { return size_; }
    bool ok() const { return ok_; }
private:
    uint8_t* data_; size_t capacity_, size_ = 0; bool ok_ = true;
};
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint64_t get(size_t bytes);
    bool done() const { return ok_ && offset_ == size_; }
    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? size_ - offset_ : 0; }
private:
    const uint8_t* data_; size_t size_, offset_ = 0; bool ok_ = true;
};
size_t encode(const Frame& frame, uint8_t* output, size_t capacity);
class Parser {
public:
    bool push(uint8_t byte, Frame& frame);
    void reset() { size_ = 0; }
private:
    uint8_t buffer_[kMaxFrameSize]{}; size_t size_ = 0;
    void discard(size_t count);
};
Frame reply(const Frame& request, Outcome outcome, Reason reason = Reason::None);
bool result(const Frame& frame, Outcome& outcome, Reason& reason);
bool sameRequest(const Frame& a, const Frame& b);
struct Key { uint64_t session = 0; uint32_t sequence = 0; };
struct Status {
    uint64_t boot = 0; uint32_t uptime = 0, revision = 0;
    State state = State::NotConfigured; Reason fault = Reason::None;
    bool motors = false; Key active, last;
    Outcome lastOutcome = Outcome::Ok; Reason lastReason = Reason::None;
    bool lastValid = false; uint16_t stage = 0;
};
void writeStatus(Writer& w, const Status& s);
bool readStatus(Reader& r, Status& s);
// Every stage parameter is a four-byte integer. Angle is signed 0.1 degree;
// speed is 0.1 rpm, accelerations whole rpm/s, current mA.
struct Parameters {
    uint32_t motor = 1; int32_t angle = 100;
    uint32_t speed = 50, accel = 10, decel = 10, current = 800;
    uint32_t get(uint16_t field) const;
    bool set(uint16_t field, uint32_t value);
};
bool validParameters(const Parameters& p);
// Structural limits of the UART stage-to-CAN bridge. This deliberately omits
// motion policy and feedback assumptions but rejects values that would narrow.
bool representableParameters(const Parameters& p);
Frame readField(uint64_t session, uint32_t seq, uint64_t boot,
                uint8_t object, uint16_t instance, uint16_t field);
} }
