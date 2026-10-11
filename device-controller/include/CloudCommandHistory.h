#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace motion {

class CloudCommandHistory {
public:
    static constexpr size_t kCapacity = 16;

    const std::string* find(const std::string& commandId) const {
        for (const auto& entry : entries_) {
            if (entry.commandId == commandId && !entry.commandId.empty()) return &entry.ack;
        }
        return nullptr;
    }

    void remember(std::string commandId, std::string ack) {
        entries_[next_] = {std::move(commandId), std::move(ack)};
        next_ = (next_ + 1) % entries_.size();
    }

private:
    struct Entry {
        std::string commandId;
        std::string ack;
    };
    std::array<Entry, kCapacity> entries_{};
    size_t next_ = 0;
};

}  // namespace motion
