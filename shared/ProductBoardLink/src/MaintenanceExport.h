#pragma once

#include "BoardPairingRecord.h"
#include "MotionStateRecord.h"
#include "BoardExportSource.h"

namespace babytech { namespace boardlink {

enum class ExportCommand { NotExport, Invalid, Valid };
ExportCommand parseMaintenanceExport(const char* line, char (&device)[65], char (&challenge)[33]);

// Read-only diagnostic capture, NOT an import authorization. Raw validated
// state preserves watermarks and full context; no credentials are read/exported.
// Place this object in static storage. Capture scratch is checked heap storage.
class MaintenanceExport : public BoardExportSource {
public:
    static constexpr uint32_t kLifetimeMs = 5000;
    bool begin(v4::Role role, const char* device, const char* challenge,
               uint64_t boot, uint32_t nowMs) override;
    bool active() const { return active_; }
    bool expired(uint32_t nowMs) const { return uint32_t(nowMs - beganAt_) >= kLifetimeMs; }
    size_t remaining() const override { return active_ ? total_ - position_ : 0; }
    size_t peek(uint8_t* output, size_t capacity) const override;
    void consume(size_t length) override;
    void cancel() override;

private:
    uint8_t pair_[kPairingRecordMaxSize]{};
    uint8_t state_[kMotionStateMaxSize]{};
    uint8_t legacy_[kContextIdentityMaxSize]{};
    char header_[640]{};
    size_t pairSize_ = 0, stateSize_ = 0, legacySize_ = 0, headerSize_ = 0;
    size_t position_ = 0, total_ = 0;
    uint32_t beganAt_ = 0;
    bool active_ = false;
    uint8_t at(size_t position) const;
};

static_assert(sizeof(MaintenanceExport) <= 6144, "maintenance export static buffer budget");

} }
