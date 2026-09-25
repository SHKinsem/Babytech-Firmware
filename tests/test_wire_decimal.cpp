#include "WireDecimal.h"

#include <cstring>
#include <iostream>

namespace {
int failures = 0;
int checks = 0;

void check(const char* raw, uint32_t maximum, bool signedValue,
           bool accepted, uint32_t expected = 0, bool reverse = false) {
    uint32_t actual = 123;
    bool negative = false;
    const bool ok = motion::parseWireTenths(raw, std::strlen(raw), maximum,
                                              signedValue, actual, negative);
    ++checks;
    if (ok != accepted || (ok && (actual != expected || negative != reverse))) {
        ++failures;
        std::cerr << "wire decimal mismatch: " << raw << '\n';
    }
}
}  // namespace

int main() {
    check("0", UINT32_MAX, true, true, 0);
    check("-0.0", UINT32_MAX, true, true, 0, true);
    check(".5", UINT32_MAX, true, true, 5);
    check("+12.30", UINT32_MAX, true, true, 123);
    check("-429496729.5", UINT32_MAX, true, true, UINT32_MAX, true);
    check("429496729.6", UINT32_MAX, true, false);
    check("6553.5", UINT16_MAX, false, true, UINT16_MAX);
    check("6553.6", UINT16_MAX, false, false);
    check("0.01", UINT16_MAX, false, false);
    check("-1", UINT16_MAX, false, false);
    check("1e2", UINT32_MAX, true, false);
    check("1..0", UINT32_MAX, true, false);
    check("+", UINT32_MAX, true, false);
    check(".", UINT32_MAX, true, false);
    uint32_t actual = 0;
    bool negative = false;
    ++checks;
    if (motion::parseWireTenths(nullptr, 1, UINT32_MAX, true, actual, negative)) ++failures;
    std::cout << "wire-decimal: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
