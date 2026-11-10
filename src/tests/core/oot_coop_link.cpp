// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_link.h"
#include "core/arm/dynarmic/oot_coop_ledger.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace {
using namespace Core::OotCoop;
struct Scratch {
    std::filesystem::path path = std::filesystem::current_path() /
        ("oot-coop-link-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directory(path); }
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
void SetEndpoint(const char* value) {
#ifdef _WIN32
    _putenv_s("EDEN_NX2_OOT_COOP", value ? value : "");
#else
    if (value) setenv("EDEN_NX2_OOT_COOP", value, 1);
    else unsetenv("EDEN_NX2_OOT_COOP");
#endif
}
struct RestoreEndpoint {
    std::optional<std::string> value;
    RestoreEndpoint() {
        if (const char* old = std::getenv("EDEN_NX2_OOT_COOP")) value = old;
    }
    ~RestoreEndpoint() { SetEndpoint(value ? value->c_str() : nullptr); BeginSession(); }
};
}
TEST_CASE("Disconnect trigger holds until removed and takes stale requests", "[oot_coop]") {
    Scratch scratch;
    Link::DisconnectTrigger trigger(scratch.path);
    REQUIRE_FALSE(trigger.Present());
    REQUIRE(trigger.TakeStale() == Link::DisconnectTrigger::Stale::Absent);
    std::ofstream(scratch.path / "coop-disconnect") << "synthetic request";
    REQUIRE(trigger.Present());
    REQUIRE(trigger.Present()); // polling never consumes a live request
    REQUIRE(trigger.TakeStale() == Link::DisconnectTrigger::Stale::Removed);
    REQUIRE_FALSE(trigger.Present());
    REQUIRE(trigger.TakeStale() == Link::DisconnectTrigger::Stale::Absent);
    std::filesystem::create_directory(scratch.path / "coop-disconnect");
    std::ofstream(scratch.path / "coop-disconnect/child") << "synthetic";
    REQUIRE(trigger.TakeStale() == Link::DisconnectTrigger::Stale::Failed);
    REQUIRE(trigger.Present());
}
TEST_CASE("Disconnect trigger treats dangling links as requests", "[oot_coop]") {
    Scratch scratch;
    std::error_code ec;
    std::filesystem::create_symlink(scratch.path / "missing", scratch.path / "coop-disconnect", ec);
    if (ec) { SKIP("symlink creation unavailable"); }
    Link::DisconnectTrigger trigger(scratch.path);
    REQUIRE(trigger.Present());
    REQUIRE(trigger.TakeStale() == Link::DisconnectTrigger::Stale::Removed);
    REQUIRE_FALSE(trigger.Present());
}
TEST_CASE("Co-op enabled gate refreshes at each session", "[oot_coop]") {
    RestoreEndpoint restore;
    SetEndpoint("0"); BeginSession(); REQUIRE_FALSE(Enabled());
    SetEndpoint("1"); BeginSession(); REQUIRE(Enabled());
    SetEndpoint("127.0.0.1:43390"); BeginSession(); REQUIRE(Enabled());
    SetEndpoint(nullptr); BeginSession(); REQUIRE_FALSE(Enabled());
}
namespace {
// The pre-C17 shape: an out-of-line call into another TU with a function-local static (guard check).
[[gnu::noinline]] bool LegacyEnabled() {
    static const bool legacy = ParseEndpoint(std::getenv("EDEN_NX2_OOT_COOP") ? std::getenv("EDEN_NX2_OOT_COOP") : "").has_value();
    return legacy;
}
template <typename F>
double NsPerCall(F&& f, size_t iterations) {
    volatile bool sink = false;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) sink = f();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    (void)sink;
    return std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(iterations);
}
} // namespace
TEST_CASE("Co-op enabled hot-path benchmark", "[.oot_coop_enabled_bench]") {
    constexpr size_t iterations = 200'000'000;
    for (int round = 0; round < 3; ++round) {
        const double legacy = NsPerCall([] { return LegacyEnabled(); }, iterations);
        const double gate = NsPerCall([] { return Enabled(); }, iterations);
        std::cout << "round " << round << ": legacy static+call " << legacy << " ns/call, C17 atomic gate "
                  << gate << " ns/call\n";
    }
}

TEST_CASE("Disconnect abort preserves admitted and not yet drained grant IDs", "[oot_coop]") {
    Ledger::Book book;
    book.Epoch("synthetic-epoch");
    Change first{Kind::Item, "synthetic-peer", 1, PackItem({1, 1, {}}), 1};
    first.name = "Synthetic_Item";
    first.catchup = true;
    first.catchup_transaction = 1;
    auto pending = first;
    pending.seq = 2;
    book.Begin(1, "synthetic-epoch", 0, 2);
    REQUIRE(book.Admit(first) == Ledger::Decision::Admit);
    book.Abort(1);
    // Both a runtime-queued grant and a not-yet-drained transport grant may finish offline.
    REQUIRE(book.Admit(first) == Ledger::Decision::Queued);
    REQUIRE(book.Admit(pending) == Ledger::Decision::Admit);
    book.Applied(first);
    book.Applied(pending);
    book.End(1); // a stale end marker cannot resurrect aborted receipt tracking
    book.Begin(2, "synthetic-epoch", 0, 2);
    first.catchup_transaction = pending.catchup_transaction = 2;
    REQUIRE(book.Admit(first) == Ledger::Decision::Applied);
    REQUIRE(book.Admit(pending) == Ledger::Decision::Applied);
    book.End(2);
    Ledger::Book restored;
    restored.Epoch("synthetic-epoch");
    restored.Bind(book.Snapshot());
    REQUIRE(restored.Admit(first) == Ledger::Decision::Applied);
    REQUIRE(restored.Admit(pending) == Ledger::Decision::Applied);
}
