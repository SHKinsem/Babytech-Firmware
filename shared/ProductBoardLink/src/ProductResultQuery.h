#pragma once

#include "MotionStateStore.h"

namespace babytech { namespace boardlink {

struct ResultQuery {
    v4::Source source = v4::Source::LocalTouch;
    uint64_t sequence = 0;
    char deviceId[65] = {};
    char commandId[129] = {};
};

enum class ResultQueryStatus : uint8_t {
    Known = 0, Unknown = 1, Expired = 2, Conflict = 3, StorageFault = 4
};

struct QueriedResult {
    ResultQuery query;
    ResultQueryStatus status = ResultQueryStatus::Unknown;
    bool accepted = false;
    char reason[65] = {};
    MotionOutcome outcome = MotionOutcome::None;
    char requestDigestHex[65] = {};
};

bool validResultQuery(const ResultQuery& query);
bool sameResultQuery(const ResultQuery& left, const ResultQuery& right);
bool encodeResultQuery(const ResultQuery& query, v4::Message& output);
bool decodeResultQuery(const v4::Message& message, ResultQuery& output);
bool encodeQueriedResult(const QueriedResult& result, v4::Message& output);
bool decodeQueriedResult(const v4::Message& message, QueriedResult& output);
// Single-owner snapshot lookup only: never loads, writes, executes or replays.
// Invalid/foreign identities leave output unchanged. Unavailable stores report
// StorageFault; absence of a verified watermark is not an empty history.
bool queryMotionResult(const MotionStateStore& store, const ResultQuery& query,
                       QueriedResult& output);

} }
