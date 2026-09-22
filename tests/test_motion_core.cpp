// Host unit tests for the Arduino-independent motion core.
// Build (no Arduino required):
//   g++ -std=c++11 -I motion/include tests/test_motion_core.cpp -o test_motion_core
//
// Covers: sign handling, wire scaling (acceleration is whole RPM/s), triangular
// and trapezoid duration, range and non-finite rejection, malformed and
// negative feedback decoding, and acknowledgement classification checked
// against the authoritative firmware codec fixtures.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../motion/include/MotionCore.h"
// Authoritative X firmware sources, used to cross-check the acknowledgement
// bytes and the CAN id framing rules against real fixtures.
#include "../motion/lib/XMotor/src/x42s_can_id.h"
#include "../motion/lib/XMotor/src/x_firmware_can_codec.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        ++g_checks;                                                    \
        if (!(cond)) {                                                 \
            ++g_failures;                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                              \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (strcmp((actual), (expected)) != 0) {                             \
            ++g_failures;                                                    \
            printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__,        \
                   __LINE__, (actual), (expected));                          \
        }                                                                    \
    } while (0)

motion::MoveRequest makeRequest(
    uint8_t id, float angleDeg, float speedRpm, float accelRpmS, float decelRpmS,
    uint16_t currentMa) {
    motion::MoveRequest request;
    request.id = id;
    request.angleDeg = angleDeg;
    request.speedRpm = speedRpm;
    request.accelRpmS = accelRpmS;
    request.decelRpmS = decelRpmS;
    request.currentMa = currentMa;
    return request;
}

motion::MoveRequest validRequest() {
    return makeRequest(1, 90.0f, 60.0f, 60.0f, 60.0f, 800);
}

void expectRejected(const motion::MoveRequest& request, const char* expected) {
    motion::MovePlan plan;
    const char* error = nullptr;
    const bool ok = motion::buildMovePlan(request, plan, &error);
    ++g_checks;
    if (ok) {
        ++g_failures;
        printf("FAIL: request accepted, expected \"%s\"\n", expected);
        return;
    }
    if (error == nullptr || strcmp(error, expected) != 0) {
        ++g_failures;
        printf("FAIL: error \"%s\", expected \"%s\"\n",
               error != nullptr ? error : "(null)", expected);
    }
}

void testSigns() {
    motion::MovePlan plan;
    const char* error = nullptr;

    CHECK(motion::buildMovePlan(
        makeRequest(7, 90.0f, 60.0f, 60.0f, 60.0f, 800), plan, &error));
    CHECK(error == nullptr);
    CHECK(plan.id == 7);
    CHECK(plan.deltaTenths == 900);
    CHECK(plan.magnitudeTenths == 900u);
    CHECK(plan.direction == motion::kDirectionPositive);

    CHECK(motion::buildMovePlan(
        makeRequest(7, -90.0f, 60.0f, 60.0f, 60.0f, 800), plan, &error));
    CHECK(plan.deltaTenths == -900);
    CHECK(plan.magnitudeTenths == 900u);
    CHECK(plan.direction == motion::kDirectionNegative);

    // A direction is available even when the travel rounds to a fraction.
    CHECK(motion::buildMovePlan(
        makeRequest(9, -0.25f, 1.0f, 1.0f, 1.0f, 100), plan, &error));
    CHECK(plan.deltaTenths == -3);  // -0.25 rounds to -0.3
    CHECK(plan.magnitudeTenths == 3u);
    CHECK(plan.direction == motion::kDirectionNegative);
}

void testWireScaling() {
    motion::MovePlan plan;
    const char* error = nullptr;

    CHECK(motion::buildMovePlan(
        makeRequest(3, 12.3f, 45.6f, 30.0f, 120.0f, 2500), plan, &error));
    CHECK(plan.deltaTenths == 123);
    CHECK(plan.magnitudeTenths == 123u);
    CHECK(plan.speedTenths == 456);
    CHECK(plan.accelWire == 30);    // whole RPM/s on the wire, no x10
    CHECK(plan.decelWire == 120);
    CHECK(plan.currentMa == 2500);
    CHECK(plan.motionMode == motion::kMotionModeRelativeToCurrent);
    CHECK(plan.sync == false);

    // Angle and speed are rounded to 0.1 before the wire value is built.
    CHECK(motion::buildMovePlan(
        makeRequest(3, 1.04f, 0.14f, 1.0f, 1.0f, 100), plan, &error));
    CHECK(plan.deltaTenths == 10);
    CHECK(plan.speedTenths == 1);
}

void testDurations() {
    // Acceleration is whole RPM/s. 60 RPM == 360 deg/s, 60 RPM/s == 360 deg/s^2
    // so each ramp covers 180 deg in 1 s.
    // Trapezoid: 720 deg at 60 RPM with 60 RPM/s on both ramps -> 3 s.
    CHECK(motion::expectedDurationMs(7200u, 600u, 60u, 60u) == 3000u);
    // Reaching the cruise speed exactly at the end of the move -> 2 s.
    CHECK(motion::expectedDurationMs(3600u, 600u, 60u, 60u) == 2000u);
    // Triangular: 90 deg at 60 RPM with 60 RPM/s ramps -> 1 s.
    CHECK(motion::expectedDurationMs(900u, 600u, 60u, 60u) == 1000u);
    // Triangular with a slower deceleration ramp -> 867 ms.
    CHECK(motion::expectedDurationMs(900u, 600u, 60u, 120u) == 867u);
    // Degenerate inputs cannot describe a move.
    CHECK(motion::expectedDurationMs(0u, 600u, 60u, 60u) == 0u);
    CHECK(motion::expectedDurationMs(3600u, 0u, 60u, 60u) == 0u);
    CHECK(motion::expectedDurationMs(3600u, 600u, 0u, 60u) == 0u);

    motion::MovePlan plan;
    const char* error = nullptr;
    CHECK(motion::buildMovePlan(validRequest(), plan, &error));
    // 90 deg, 60 RPM, 60 RPM/s on both ramps -> triangular, 1000 ms.
    CHECK(plan.expectedDurationMs == 1000u);
}

void testRangeAndNonFiniteRejection() {
    expectRejected(makeRequest(0, 90.0f, 60.0f, 60.0f, 60.0f, 800), "id_reserved");

    expectRejected(makeRequest(1, 0.0f, 60.0f, 60.0f, 60.0f, 800), "angle_rounds_to_zero");
    expectRejected(makeRequest(1, 0.04f, 60.0f, 60.0f, 60.0f, 800), "angle_rounds_to_zero");
    expectRejected(makeRequest(1, 3600.5f, 60.0f, 60.0f, 60.0f, 800), "angle_out_of_range");
    expectRejected(makeRequest(1, -3600.5f, 60.0f, 60.0f, 60.0f, 800), "angle_out_of_range");
    expectRejected(makeRequest(1, NAN, 60.0f, 60.0f, 60.0f, 800), "angle_not_finite");
    expectRejected(makeRequest(1, INFINITY, 60.0f, 60.0f, 60.0f, 800), "angle_not_finite");
    expectRejected(makeRequest(1, -INFINITY, 60.0f, 60.0f, 60.0f, 800), "angle_not_finite");

    expectRejected(makeRequest(1, 90.0f, 0.04f, 60.0f, 60.0f, 800), "speed_out_of_range");
    expectRejected(makeRequest(1, 90.0f, 120.1f, 60.0f, 60.0f, 800), "speed_out_of_range");
    expectRejected(makeRequest(1, 90.0f, NAN, 60.0f, 60.0f, 800), "speed_not_finite");
    expectRejected(makeRequest(1, 90.0f, INFINITY, 60.0f, 60.0f, 800), "speed_not_finite");

    expectRejected(makeRequest(1, 90.0f, 60.0f, 0.0f, 60.0f, 800), "accel_out_of_range");
    expectRejected(makeRequest(1, 90.0f, 60.0f, 241.0f, 60.0f, 800), "accel_out_of_range");
    expectRejected(makeRequest(1, 90.0f, 60.0f, 2.5f, 60.0f, 800), "accel_not_integer");
    expectRejected(makeRequest(1, 90.0f, 60.0f, NAN, 60.0f, 800), "accel_not_finite");
    expectRejected(makeRequest(1, 90.0f, 60.0f, 60.0f, 0.0f, 800), "decel_out_of_range");
    expectRejected(makeRequest(1, 90.0f, 60.0f, 60.0f, 30.5f, 800), "decel_not_integer");
    expectRejected(makeRequest(1, 90.0f, 60.0f, 60.0f, -INFINITY, 800), "decel_not_finite");

    expectRejected(makeRequest(1, 90.0f, 60.0f, 60.0f, 60.0f, 99), "current_out_of_range");
    expectRejected(makeRequest(1, 90.0f, 60.0f, 60.0f, 60.0f, 5001), "current_out_of_range");

    // 3600 deg at 0.1 RPM needs far more than the 60 s budget.
    expectRejected(makeRequest(1, 3600.0f, 0.1f, 1.0f, 1.0f, 800), "duration_too_long");

    // Nothing is silently clipped: the boundary values are accepted as is.
    motion::MovePlan plan;
    const char* error = nullptr;
    CHECK(motion::buildMovePlan(
        makeRequest(255, 3600.0f, 0.1f, 1.0f, 1.0f, 100), plan, &error) == false);
    CHECK(motion::buildMovePlan(
        makeRequest(255, 12.0f, 120.0f, 240.0f, 240.0f, 5000), plan, &error));
    CHECK(plan.speedTenths == 1200);
    CHECK(plan.accelWire == 240);
    CHECK(plan.currentMa == 5000);
    CHECK(error == nullptr);
}

void testFeedbackDecoding() {
    motion::FeedbackSample sample;

    // Position, positive: 0x36 00 00 00 01 2C 6B -> 300 tenths = 30.0 deg.
    {
        const uint8_t frame[] = {0x36, 0x00, 0x00, 0x00, 0x01, 0x2C, 0x6B};
        CHECK(motion::decodeFeedback(frame, sizeof(frame), sample));
        CHECK(sample.field == motion::FeedbackField::Position);
        CHECK(sample.value == 300);
    }
    // Position, negative (sign byte 1) -> -30.0 deg.
    {
        const uint8_t frame[] = {0x36, 0x01, 0x00, 0x00, 0x01, 0x2C, 0x6B};
        CHECK(motion::decodeFeedback(frame, sizeof(frame), sample));
        CHECK(sample.field == motion::FeedbackField::Position);
        CHECK(sample.value == -300);
    }
    // Position, negative zero magnitude stays zero.
    {
        const uint8_t frame[] = {0x36, 0x01, 0x00, 0x00, 0x00, 0x00, 0x6B};
        CHECK(motion::decodeFeedback(frame, sizeof(frame), sample));
        CHECK(sample.value == 0);
    }
    // Position, malformed variants.
    {
        const uint8_t badChecksum[] = {0x36, 0x00, 0x00, 0x00, 0x01, 0x2C, 0x00};
        CHECK(!motion::decodeFeedback(badChecksum, sizeof(badChecksum), sample));
        const uint8_t badSign[] = {0x36, 0x02, 0x00, 0x00, 0x01, 0x2C, 0x6B};
        CHECK(!motion::decodeFeedback(badSign, sizeof(badSign), sample));
        const uint8_t badLength[] = {0x36, 0x00, 0x00, 0x00, 0x01, 0x6B};
        CHECK(!motion::decodeFeedback(badLength, sizeof(badLength), sample));
        const uint8_t tooLarge[] = {0x36, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x6B};
        CHECK(!motion::decodeFeedback(tooLarge, sizeof(tooLarge), sample));
        CHECK(!motion::decodeFeedback(nullptr, 7, sample));
        CHECK(!motion::decodeFeedback(badChecksum, 0, sample));
    }

    // Velocity, negative: 0x35 01 00 1E 6B -> -30 tenths = -3.0 RPM.
    {
        const uint8_t frame[] = {0x35, 0x01, 0x00, 0x1E, 0x6B};
        CHECK(motion::decodeFeedback(frame, sizeof(frame), sample));
        CHECK(sample.field == motion::FeedbackField::Velocity);
        CHECK(sample.value == -30);
    }
    // Velocity, positive.
    {
        const uint8_t frame[] = {0x35, 0x00, 0x00, 0x1E, 0x6B};
        CHECK(motion::decodeFeedback(frame, sizeof(frame), sample));
        CHECK(sample.value == 30);
    }
    // Velocity, malformed sign byte and checksum.
    {
        const uint8_t badSign[] = {0x35, 0x02, 0x00, 0x1E, 0x6B};
        CHECK(!motion::decodeFeedback(badSign, sizeof(badSign), sample));
        const uint8_t badChecksum[] = {0x35, 0x00, 0x00, 0x1E, 0x6A};
        CHECK(!motion::decodeFeedback(badChecksum, sizeof(badChecksum), sample));
        const uint8_t badLength[] = {0x35, 0x00, 0x00, 0x1E};
        CHECK(!motion::decodeFeedback(badLength, sizeof(badLength), sample));
    }

    // Current: 0x27 03 E8 6B -> 1000 mA. Current is unsigned.
    {
        const uint8_t frame[] = {0x27, 0x03, 0xE8, 0x6B};
        CHECK(motion::decodeFeedback(frame, sizeof(frame), sample));
        CHECK(sample.field == motion::FeedbackField::Current);
        CHECK(sample.value == 1000);
        const uint8_t malformed[] = {0x27, 0x03, 0xE8};
        CHECK(!motion::decodeFeedback(malformed, sizeof(malformed), sample));
    }

    // Flags.
    {
        const uint8_t frame[] = {0x3A, 0x05, 0x6B};
        CHECK(motion::decodeFeedback(frame, sizeof(frame), sample));
        CHECK(sample.field == motion::FeedbackField::Flags);
        CHECK(sample.value == 5);
        const uint8_t malformed[] = {0x3A, 0x05, 0x00};
        CHECK(!motion::decodeFeedback(malformed, sizeof(malformed), sample));
    }

    // Unknown function code.
    {
        const uint8_t frame[] = {0x99, 0x00, 0x6B};
        CHECK(!motion::decodeFeedback(frame, sizeof(frame), sample));
    }
}

void testAckClassification() {
    // Exact bytes from the authoritative firmware codec.
    CHECK(motion::classifyAck(0x02) == motion::AckStatus::Received);
    CHECK(motion::classifyAck(0x9F) == motion::AckStatus::Completed);
    CHECK(motion::classifyAck(0xE2) == motion::AckStatus::ParameterError);
    CHECK(motion::classifyAck(0xEE) == motion::AckStatus::FormatError);
    CHECK(motion::classifyAck(0x80) == motion::AckStatus::UnknownError);
    CHECK(motion::classifyAck(0xFF) == motion::AckStatus::UnknownError);

    // The previously invented 0x00..0x03 mapping must never classify as an ack.
    CHECK(motion::classifyAck(0x00) == motion::AckStatus::UnknownError);
    CHECK(motion::classifyAck(0x01) == motion::AckStatus::UnknownError);
    CHECK(motion::classifyAck(0x03) == motion::AckStatus::UnknownError);

    CHECK_STR(motion::ackStatusToString(motion::AckStatus::Received), "received");
    CHECK_STR(motion::ackStatusToString(motion::AckStatus::Completed), "completed");
    CHECK_STR(motion::ackStatusToString(motion::AckStatus::ParameterError), "param_error");
    CHECK_STR(motion::ackStatusToString(motion::AckStatus::FormatError), "format_error");
    CHECK_STR(motion::ackStatusToString(motion::AckStatus::UnknownError), "unknown_error");
}

// Fixture: the ack frames the authoritative codec accepts must be exactly the
// frames our classifier treats as acknowledgements, and the high bits of the
// status byte must stay rejected.
void testAckFixtureMatchesCodec() {
    const uint8_t statusBytes[] = {0x02, 0x9F, 0xE2, 0xEE};
    const motion::AckStatus expected[] = {
        motion::AckStatus::Received,
        motion::AckStatus::Completed,
        motion::AckStatus::ParameterError,
        motion::AckStatus::FormatError,
    };
    for (size_t i = 0; i < sizeof(statusBytes); ++i) {
        // A real reply is [function][status][0x6B]; the codec validates length
        // and the trailing checksum.
        const uint8_t frame[] = {0xF3, statusBytes[i], 0x6B};
        XCommandResponseStatus decoded = XCommandResponseStatus::None;
        CHECK(decodeXCommandResponse(frame, sizeof(frame), decoded));
        CHECK(decoded != XCommandResponseStatus::None);
        CHECK(motion::classifyAck(statusBytes[i]) == expected[i]);
    }

    // Rejected by the codec: wrong checksum, wrong length, unknown status.
    {
        const uint8_t badChecksum[] = {0xF3, 0x02, 0x00};
        XCommandResponseStatus decoded = XCommandResponseStatus::None;
        CHECK(!decodeXCommandResponse(badChecksum, sizeof(badChecksum), decoded));
    }
    {
        const uint8_t shortFrame[] = {0xF3, 0x02};
        XCommandResponseStatus decoded = XCommandResponseStatus::None;
        CHECK(!decodeXCommandResponse(shortFrame, sizeof(shortFrame), decoded));
    }
    {
        const uint8_t unknownStatus[] = {0xF3, 0x00, 0x6B};
        XCommandResponseStatus decoded = XCommandResponseStatus::None;
        CHECK(!decodeXCommandResponse(unknownStatus, sizeof(unknownStatus), decoded));
        CHECK(motion::classifyAck(0x00) == motion::AckStatus::UnknownError);
    }
}

// Acceleration must be sent as the request's whole RPM/s, never scaled by 10.
void testAccelWireIsWholeRpmPerSecond() {
    motion::MovePlan plan;
    const char* error = nullptr;
    CHECK(motion::buildMovePlan(
        makeRequest(4, 5.0f, 30.0f, 7.0f, 13.0f, 500), plan, &error));
    CHECK(plan.accelWire == 7);   // not 70
    CHECK(plan.decelWire == 13);  // not 130
    CHECK(error == nullptr);
}

// The CAN id helper used to filter incoming frames must accept only single
// packet extended data frames with no high bits set.
void testFrameFiltering() {
    CHECK(x42sCanIsSinglePacketDataFrame(x42sCanFrameId(5, 0), true, false));
    // Multi packet packet index != 0.
    CHECK(!x42sCanIsSinglePacketDataFrame(x42sCanFrameId(5, 1), true, false));
    // Remote frame.
    CHECK(!x42sCanIsSinglePacketDataFrame(x42sCanFrameId(5, 0), true, true));
    // Standard (non-extended) frame.
    CHECK(!x42sCanIsSinglePacketDataFrame(x42sCanFrameId(5, 0), false, false));
    // Extended id with bits above 16 set.
    CHECK(!x42sCanIsSinglePacketDataFrame(
        x42sCanFrameId(5, 0) | 0x10000u, true, false));
}

}  // namespace

int main() {
    testSigns();
    testWireScaling();
    testDurations();
    testRangeAndNonFiniteRejection();
    testFeedbackDecoding();
    testAckClassification();
    testAckFixtureMatchesCodec();
    testAccelWireIsWholeRpmPerSecond();
    testFrameFiltering();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
