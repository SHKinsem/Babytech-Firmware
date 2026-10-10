#pragma once

#include <Arduino.h>
#include "CloudLink.h"
#include "ProductSession.h"
#include "ProductEventState.h"

class NvsProductEventStorage : public motion::ProductEventStorage {
public:
    bool read(std::string& value) override;
    bool write(const std::string& value) override;
    bool clear() override;
};

class ProductEventOutbox {
public:
    enum class LegacyState { Empty, Present, ReadError };
    static LegacyState inspectLegacyState();
    void begin(const String& deviceId, motion::ProductSession& product);
    bool queue(const motion::ProductTerminal& terminal, uint32_t now);
    void poll(CloudLink& cloud, motion::ProductSession& product, uint32_t now);
    bool receiveReceipt(JsonVariantConst receipt, motion::ProductSession& product);

private:
    NvsProductEventStorage storage_;
    motion::ProductEventState state_{storage_};
    uint32_t retryAt_ = 0;
};
