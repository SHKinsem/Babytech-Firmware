#include <cassert>
#include <cstdint>

#include "RetryDeadline.h"

int main() {
    constexpr uint32_t lateUptime = 0x80000010u;
    assert(motion::retryDue(lateUptime, lateUptime));
    assert(!motion::retryDue(lateUptime, lateUptime + 5000));
    assert(motion::retryDue(lateUptime + 5000, lateUptime + 5000));

    constexpr uint32_t beforeWrap = 0xfffffff0u;
    constexpr uint32_t afterWrap = beforeWrap + 5000;
    assert(!motion::retryDue(beforeWrap, afterWrap));
    assert(!motion::retryDue(afterWrap - 1, afterWrap));
    assert(motion::retryDue(afterWrap, afterWrap));
    assert(motion::retryDue(afterWrap + 1, afterWrap));
}
