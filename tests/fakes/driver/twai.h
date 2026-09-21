#pragma once
// Intentionally empty stand-in for the ESP-IDF TWAI driver header.
//
// The real X42sProtocol.h includes "driver/twai.h", but none of the TWAI types
// appear in its class declaration (only in the .cpp), so an empty header is
// enough for the host build. The driver behaviour itself is faked by the
// X42sProtocol implementation in tests/fakes/fake_x42s.cpp.
