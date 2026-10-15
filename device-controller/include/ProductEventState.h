#pragma once

#include <ArduinoJson.h>
#include "ProductSession.h"

namespace motion {

class ProductEventStorage {
public:
    virtual ~ProductEventStorage() = default;
    // An empty successful read means a new/cleared slot, never a read failure.
    virtual bool read(std::string& value) = 0;
    virtual bool write(const std::string& value) = 0;
    virtual bool clear() = 0;
};

// One atomic storage slot transitions journal -> terminal -> empty. No pair of
// keys can resurrect a journal after the corresponding terminal was acknowledged.
class ProductEventState : public ProductStartGuard {
public:
    explicit ProductEventState(ProductEventStorage& storage) : storage_(storage) {}
    bool begin(const std::string& deviceId, const std::string& bootToken);
    bool ready() const override { return healthy_ && !journal_ && payload_.empty(); }
    bool prepare(ProductRun& run, uint32_t now) override;
    bool queue(const ProductTerminal& terminal, uint32_t now);
    bool persistPending();
    bool receiveReceipt(JsonVariantConst receipt);
    bool pending() const { return !journal_ && !payload_.empty(); }
    bool blocked() const { return !healthy_ || pending(); }
    bool persisted() const { return persisted_; }
    const std::string& payload() const { return payload_; }
    const std::string& eventId() const { return eventId_; }

private:
    ProductEventStorage& storage_;
    std::string deviceId_;
    std::string bootToken_;
    std::string commandId_;
    std::string eventId_;
    std::string payload_;
    uint32_t sequence_ = 0;
    bool healthy_ = false;
    bool journal_ = false;
    bool persisted_ = false;
};

}  // namespace motion
