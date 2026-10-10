#include "BoardPairingStore.h"
#include "FakePairingNvs.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
using babytech::v4::Pairing;
using babytech::v4::Role;
using fake::Bytes;
using fake::Op;
using fake::io;

namespace {
unsigned scenarios = 0;

Pairing sample(Role role = Role::Brain) {
    Pairing result{};
    result.role = role;
    std::strcpy(result.deviceId, "Babytech_01-test");
    std::strcpy(result.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(result.localPhysicalId, "012345abcdef");
    std::strcpy(result.peerPhysicalId, "fedcba987654");
    return result;
}

Bytes encode(const Pairing& pairing = sample()) {
    std::array<uint8_t, kPairingRecordMaxSize> data{};
    const size_t length = encodePairingRecord(pairing, data.data(), data.size());
    assert(length != 0);
    return Bytes(data.begin(), data.begin() + length);
}

void seed(const Bytes& bytes, fake::Type type = fake::Type::Blob) {
    io.disk["productpair"]["record"] = {bytes, type};
}

fake::Database protectedData() {
    auto disk = io.disk;
    const auto found = disk.find("productpair");
    if (found != disk.end()) {
        found->second.erase("record");
        if (found->second.empty()) disk.erase(found);
    }
    return disk;
}

void scenario(const std::string& name, const std::function<void()>& run,
              bool namespaceExists = true) {
    fake::reset();
    for (const char* space : {"wifi-cfg", "productctx", "outbox", "actuatorcfg", "sensorcfg"}) {
        io.disk[space]["record"] = {{0, 0xff, 0x7f, 1}, fake::Type::Blob};
        io.disk[space]["payload"] = {{'o', 'l', 'd', 0}, fake::Type::String};
    }
    if (namespaceExists) io.disk["productpair"]["unrelated"] = {{1, 2, 3}, fake::Type::U32};
    const auto protectedBefore = protectedData();
    std::printf("[%u] %s\n", ++scenarios, name.c_str());
    std::fflush(stdout);
    run();
    fake::verifyFaults();
    assert(io.handles.empty());
    assert(protectedData() == protectedBefore);
    assert(fake::count(Op::Erase) == 0);
    for (const auto& call : io.calls) {
        if (call.op == Op::Mac) continue;
        assert(call.name == "productpair");
        if (!call.key.empty()) assert(call.key == "record");
    }
}

void noWrites() {
    assert(fake::count(Op::OpenRW) == 0);
    assert(fake::count(Op::Set) == 0);
    assert(fake::count(Op::Commit) == 0);
}

void load(PairingLoad expected, Role role = Role::Brain, const Pairing& wanted = sample()) {
    Pairing out = sample(Role::Motion);
    std::strcpy(out.deviceId, "untouched-output");
    std::array<uint8_t, sizeof(Pairing)> before{};
    std::memcpy(before.data(), &out, sizeof(out));
    const auto disk = io.disk;
    const auto sets = fake::count(Op::Set);
    const auto commits = fake::count(Op::Commit);
    const auto rw = fake::count(Op::OpenRW);
    assert(loadBoardPairing(role, out) == expected);
    if (expected == PairingLoad::Ready) assert(encode(out) == encode(wanted));
    else assert(std::memcmp(before.data(), &out, sizeof(out)) == 0);
    assert(io.disk == disk);
    assert(fake::count(Op::Set) == sets && fake::count(Op::Commit) == commits);
    assert(fake::count(Op::OpenRW) == rw && io.handles.empty());
}

void latched(PairingInstaller& installer) {
    assert(installer.faulted());
    const auto calls = io.calls.size();
    const auto disk = io.disk;
    Pairing changed = sample();
    std::strcpy(changed.deviceId, "different");
    for (const Pairing& pairing : {sample(), changed, Pairing{}})
        assert(installer.installFirst(Role::Brain, pairing) == PairingInstall::StorageFault);
    assert(io.calls.size() == calls && io.disk == disk && io.handles.empty());
}

void rebootRetry(bool persisted) {
    fake::verifyFaults();
    assert(io.handles.empty());
    fake::reboot();
    load(persisted ? PairingLoad::Ready : PairingLoad::Missing);
    PairingInstaller fresh;
    assert(!fresh.faulted());
    assert(fresh.installFirst(Role::Brain, sample()) ==
           (persisted ? PairingInstall::AlreadyInstalled : PairingInstall::Installed));
    assert(!fresh.faulted());
    if (persisted) noWrites();
    else assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
    assert(io.disk.at("productpair").at("record").bytes == encode());
}

void loadAndInstallBasics() {
    for (bool namespaceExists : {false, true}) {
        scenario(namespaceExists ? "missing key" : "missing namespace", [&] {
            const auto before = io.disk;
            load(PairingLoad::Missing);
            assert(io.disk == before);
            noWrites();
            PairingInstaller installer;
            assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::Installed);
            assert(!installer.faulted());
            assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            load(PairingLoad::Ready);
        }, namespaceExists);
    }
    for (Role role : {Role::Brain, Role::Motion}) {
        for (bool early : {false, true}) {
            scenario("install success, exact SDK order, retry and reboot", [&] {
                io.durableOnSet = early;
                const Pairing pairing = sample(role);
                PairingInstaller installer;
                assert(installer.installFirst(role, pairing) == PairingInstall::Installed);
                const std::vector<Op> expected{Op::Mac, Op::OpenRO, Op::Query, Op::Close,
                    Op::OpenRW, Op::Query, Op::Set, Op::Commit, Op::Close, Op::OpenRO,
                    Op::Query, Op::Read, Op::Close, Op::Mac};
                assert(io.calls.size() == expected.size());
                for (size_t i = 0; i < expected.size(); ++i) assert(io.calls[i].op == expected[i]);
                const auto disk = io.disk;
                for (unsigned retry = 0; retry < 3; ++retry)
                    assert(installer.installFirst(role, pairing) == PairingInstall::AlreadyInstalled);
                assert(!installer.faulted() && io.disk == disk);
                assert(fake::count(Op::OpenRW) == 1 && fake::count(Op::Set) == 1);
                assert(fake::count(Op::Commit) == 1);
                fake::reboot();
                load(PairingLoad::Ready, role, pairing);
                PairingInstaller fresh;
                assert(fresh.installFirst(role, pairing) == PairingInstall::AlreadyInstalled);
                noWrites();
            });
        }
        scenario("valid persisted retry is entirely read-only", [&] {
            const auto pairing = sample(role);
            seed(encode(pairing));
            const auto disk = io.disk;
            load(PairingLoad::Ready, role, pairing);
            PairingInstaller installer;
            assert(installer.installFirst(role, pairing) == PairingInstall::AlreadyInstalled);
            assert(!installer.faulted() && io.disk == disk);
            noWrites();
        });
        for (size_t size : {size_t(1), size_t(64)}) {
            scenario("device ID boundary and ignored in-memory padding", [&] {
                auto pairing = sample(role);
                std::memset(pairing.deviceId, 'Z', size);
                pairing.deviceId[size] = 0;
                seed(encode(pairing));
                if (size < 64) std::memset(pairing.deviceId + size + 1, 0xff, 64 - size);
                const auto disk = io.disk;
                PairingInstaller installer;
                assert(installer.installFirst(role, pairing) == PairingInstall::AlreadyInstalled);
                assert(!installer.faulted() && io.disk == disk);
                noWrites();
            });
        }
    }
}

Pairing conflicting(unsigned field) {
    auto pairing = sample();
    switch (field) {
        case 0: std::strcpy(pairing.deviceId, "different-device"); break;
        case 1: pairing.epoch[0] = 'f'; break;
        case 2: pairing.peerPhysicalId[0] = 'a'; break;
        case 3: pairing.role = Role::Motion; break;
        case 4: pairing.localPhysicalId[0] = 'a'; break;
        default: assert(false);
    }
    return pairing;
}

void conflictsAndValidation() {
    for (unsigned field = 0; field < 5; ++field) {
        scenario("existing conflict field " + std::to_string(field), [&] {
            seed(encode(conflicting(field)));
            const auto disk = io.disk;
            PairingInstaller installer;
            assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::Conflict);
            assert(!installer.faulted() && io.disk == disk);
            noWrites();
        });
    }
    for (unsigned kind = 0; kind < 5; ++kind) {
        scenario("invalid candidate rejected before all SDK calls " + std::to_string(kind), [&] {
            auto pairing = sample();
            switch (kind) {
                case 0: pairing.role = static_cast<Role>(0); break;
                case 1: std::memset(pairing.deviceId, 'a', sizeof(pairing.deviceId)); break;
                case 2: pairing.epoch[0] = 'G'; break;
                case 3: std::strcpy(pairing.peerPhysicalId, pairing.localPhysicalId); break;
                case 4: std::memset(pairing.localPhysicalId, '0', 12); break;
            }
            PairingInstaller installer;
            assert(installer.installFirst(Role::Brain, pairing) == PairingInstall::Invalid);
            assert(!installer.faulted() && io.calls.empty());
            assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::Installed);
        });
    }
    for (bool wrongRole : {false, true}) {
        scenario(wrongRole ? "requested role mismatch" : "wrong local MAC", [&] {
            seed(encode());
            if (!wrongRole) io.mac[0] ^= 0x80;
            const auto role = wrongRole ? Role::Motion : Role::Brain;
            const auto disk = io.disk;
            load(PairingLoad::IdentityMismatch, role);
            PairingInstaller installer;
            assert(installer.installFirst(role, sample()) == PairingInstall::IdentityMismatch);
            assert(!installer.faulted() && io.disk == disk);
            noWrites();
        });
    }
}

void corruptRecords() {
    const auto rejected = [](const Bytes& bytes, PairingLoad expected,
                             fake::Type type = fake::Type::Blob) {
        seed(bytes, type);
        const auto disk = io.disk;
        load(expected);
        PairingInstaller installer;
        assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
        latched(installer);
        assert(io.disk == disk);
        noWrites();
    };
    for (fake::Type type : {fake::Type::String, fake::Type::U32}) {
        scenario("NVS wrong value type", [&] { rejected(encode(), PairingLoad::IoError, type); });
    }
    for (size_t length : {0u, 1u, 70u, 135u, 256u, 257u, 1024u}) {
        scenario("corrupt length " + std::to_string(length), [&] {
            rejected(Bytes(length, 0xa5), PairingLoad::Corrupt);
        });
    }
    const auto original = encode();
    for (size_t byte = 0; byte < original.size(); ++byte) {
        scenario("CRC/header/payload corruption at byte " + std::to_string(byte), [&] {
            auto bytes = original;
            bytes[byte] ^= 0x80;
            rejected(bytes, PairingLoad::Corrupt);
        });
    }
}

void readFailures() {
    for (Op op : {Op::OpenRO, Op::Query, Op::Read, Op::Mac}) {
        scenario("load SDK error " + std::to_string(int(op)), [&] {
            seed(encode());
            fake::fail(op, 1);
            load(PairingLoad::IoError);
            noWrites();
        });
    }
    for (size_t length : {size_t(0), encode().size() - 1, encode().size() + 1, SIZE_MAX}) {
        scenario("load inconsistent successful read length " + std::to_string(length), [&] {
            seed(encode());
            fake::fail(Op::Read, 1, ESP_OK, false, length);
            load(PairingLoad::IoError);
            noWrites();
        });
    }
    for (size_t length : {size_t(0), size_t(257), SIZE_MAX}) {
        scenario("load impossible queried size " + std::to_string(length), [&] {
            seed(encode());
            fake::fail(Op::Query, 1, ESP_OK, false, length);
            load(PairingLoad::Corrupt);
            assert(fake::count(Op::Read) == 0);
            noWrites();
        });
    }
    for (const auto& point : std::vector<std::pair<Op, unsigned>>{
             {Op::Mac, 1}, {Op::OpenRO, 1}, {Op::Query, 1}, {Op::Read, 1},
             {Op::Mac, 2}, {Op::OpenRW, 1}, {Op::Query, 2}}) {
        scenario("installer prewrite error " + std::to_string(int(point.first)) + "/" +
                 std::to_string(point.second), [&] {
            if (point.first == Op::Read || (point.first == Op::Mac && point.second == 2))
                seed(encode());
            const auto disk = io.disk;
            fake::fail(point.first, point.second);
            PairingInstaller installer;
            assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
            latched(installer);
            assert(io.disk == disk);
            assert(fake::count(Op::Set) == 0 && fake::count(Op::Commit) == 0);
        });
    }
}

void recordAppearsBeforeRW() {
    for (unsigned kind = 0; kind < 9; ++kind) {
        scenario("record appears at RW open " + std::to_string(kind), [&] {
            Bytes appeared = kind < 5 ? encode(conflicting(kind)) : encode();
            if (kind == 6) appeared[8] ^= 1;
            if (kind == 8) appeared.clear();
            bool injected = false;
            io.before = [&](const fake::Call& call) {
                if (call.op == Op::OpenRW) {
                    assert(!injected);
                    injected = true;
                    seed(appeared, kind == 7 ? fake::Type::String : fake::Type::Blob);
                }
            };
            PairingInstaller installer;
            const auto expected = kind < 5 ? PairingInstall::Conflict :
                (kind == 5 ? PairingInstall::AlreadyInstalled : PairingInstall::StorageFault);
            assert(installer.installFirst(Role::Brain, sample()) == expected);
            assert(injected && io.disk.at("productpair").at("record").bytes == appeared);
            assert(fake::count(Op::Set) == 0 && fake::count(Op::Commit) == 0);
            if (expected == PairingInstall::StorageFault) latched(installer);
            else assert(!installer.faulted());
            io.before = {};
        });
    }
    scenario("record appears at RW but data read fails", [] {
        io.before = [](const fake::Call& call) { if (call.op == Op::OpenRW) seed(encode()); };
        fake::fail(Op::Read, 1);
        PairingInstaller installer;
        assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
        latched(installer);
        assert(fake::count(Op::Set) == 0 && fake::count(Op::Commit) == 0);
        assert(io.disk.at("productpair").at("record").bytes == encode());
    });
}

void writeFailures() {
    for (Op op : {Op::Set, Op::Commit}) {
        for (esp_err_t error : {ESP_FAIL, ESP_ERR_NVS_NOT_ENOUGH_SPACE, ESP_ERR_NVS_REMOVE_FAILED}) {
            for (bool persisted : {false, true}) {
                scenario("write failure " + std::to_string(int(op)) + "/" +
                         std::to_string(error) + (persisted ? " persisted" : " not persisted"), [&] {
                    fake::fail(op, 1, error, persisted);
                    PairingInstaller installer;
                    assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
                    latched(installer);
                    assert(fake::count(Op::Set) == 1);
                    assert(fake::count(Op::Commit) == (op == Op::Commit ? 1u : 0u));
                    assert(fake::count(Op::OpenRO) == 1);
                    assert(io.disk.at("productpair").count("record") == (persisted ? 1u : 0u));
                    if (persisted) assert(io.disk.at("productpair").at("record").bytes == encode());
                    rebootRetry(persisted);
                });
            }
        }
    }
    scenario("successful early-durable set followed by failing commit", [] {
        io.durableOnSet = true;
        fake::fail(Op::Commit, 1);
        PairingInstaller installer;
        assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
        latched(installer);
        rebootRetry(true);
    });
}

void readbackFailures() {
    for (const auto& point : std::vector<std::pair<Op, unsigned>>{
             {Op::OpenRO, 2}, {Op::Query, 3}, {Op::Read, 1}, {Op::Mac, 2}}) {
        for (esp_err_t error : {ESP_FAIL, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_TYPE_MISMATCH,
                                ESP_ERR_NVS_INVALID_LENGTH}) {
            scenario("readback API failure " + std::to_string(int(point.first)) + "/" +
                     std::to_string(error), [&] {
                fake::fail(point.first, point.second, error);
                PairingInstaller installer;
                assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
                latched(installer);
                assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
                assert(io.disk.at("productpair").at("record").bytes == encode());
                rebootRetry(true);
            });
        }
    }
    for (Op op : {Op::Query, Op::Read}) {
        for (size_t length : {size_t(0), encode().size() - 1, encode().size() + 1, size_t(257), SIZE_MAX}) {
            scenario("readback inconsistent length " + std::to_string(int(op)) + "/" +
                     std::to_string(length), [&] {
                fake::fail(op, op == Op::Query ? 3 : 1, ESP_OK, false, length);
                PairingInstaller installer;
                assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
                latched(installer);
                rebootRetry(true);
            });
        }
    }
    for (unsigned kind = 0; kind < 10; ++kind) {
        scenario("readback changed data or hardware " + std::to_string(kind), [&] {
            bool injected = false;
            fake::Database atReadback;
            io.before = [&](const fake::Call& call) {
                if (call.op != Op::OpenRO || call.occurrence != 2) return;
                injected = true;
                if (kind < 5) seed(encode(conflicting(kind)));
                else if (kind == 5) { auto bytes = encode(); bytes[8] ^= 1; seed(bytes); }
                else if (kind == 6) io.disk["productpair"].erase("record");
                else if (kind == 7) seed(encode(), fake::Type::U32);
                else if (kind == 8) seed(Bytes(257, 0));
                else io.mac[0] ^= 0x80;
                atReadback = io.disk;
            };
            PairingInstaller installer;
            assert(installer.installFirst(Role::Brain, sample()) == PairingInstall::StorageFault);
            assert(injected && io.disk == atReadback);
            latched(installer);
            assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            io.before = {};
            if (kind == 9) { io.mac[0] ^= 0x80; rebootRetry(true); }
        });
    }
}
}  // namespace

int main() {
    loadAndInstallBasics();
    conflictsAndValidation();
    corruptRecords();
    readFailures();
    recordAppearsBeforeRW();
    writeFailures();
    readbackFailures();
    std::printf("Pairing storage: %u scenarios passed; production decisions, SDK faults, "
                "sticky failures, reboot retry, no erase and namespace preservation\n", scenarios);
}
