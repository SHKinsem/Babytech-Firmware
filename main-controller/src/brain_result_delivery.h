#pragma once

#include <ProductEventMessages.h>
#include <cstring>

namespace babytech { namespace brain {

// UI-loop relay only. Motion retains the durable outbox and republishes after
// lost MQTT/UART traffic; neither queue admission nor LinkAck deletes a result.
template<class Link, class Network>
class BrainResultDelivery {
public:
    BrainResultDelivery(Link& link, Network& network) : link_(link), network_(network) {}
    bool terminal(const v4::Message& message) {
        const auto* pairing = link_.verifiedPairing();
        return pairing && network_.publishTerminalEvent(*pairing, message);
    }
    bool receipt(const boardlink::CloudReceipt& receipt) {
        const auto* pairing = link_.verifiedPairing();
        if (!pairing || std::strncmp(pairing->deviceId, receipt.deviceId, sizeof(receipt.deviceId))) return false;
        if (pending_) return !std::strncmp(receipt_.eventId, receipt.eventId, sizeof(receipt.eventId));
        receipt_ = receipt;
        pending_ = true;
        return true;
    }
    void poll(uint32_t nowMs, bool maintenance = false) {
        if (pending_ && !maintenance && link_.forwardCloudReceipt(receipt_, nowMs)) pending_ = false;
    }
private:
    Link& link_;
    Network& network_;
    boardlink::CloudReceipt receipt_{};
    bool pending_ = false;
};

} }
