// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Core::OotCoop::QuestDefer { struct Config; class Machine; struct Port; struct Observation; }

namespace Core::OotCoop::Quest {
struct Entry {
    u64 offset{};
    std::vector<u32> words;
};
struct Table {
    u64 count{}, data{};
    bool operator==(const Table&) const = default;
};
struct Keys {
    bool enabled{};
    std::shared_ptr<const QuestDefer::Config> defer;
    u32 notready_hash{}, complete_hash{}, ready_hash{};
    u64 settle_frames{300}, apply_timeout{600};
    std::map<std::string, u64, std::less<>> manager, layout;
    std::array<Table, 3> tables{};
    Entry setter;
    struct Observer { Entry entry; u32 state_register{}; u64 state_offset{}; };
    std::vector<Observer> observers;
    std::array<u32, 3> arguments{1, 0, 0};
    std::map<u32, std::vector<u32>> mirror;
    u64 M(std::string_view field) const { return manager.at(std::string{field}); }
    u64 L(std::string_view field) const { return layout.at(std::string{field}); }
};
std::shared_ptr<const Keys> DecodeKeys(const nlohmann::json& value);
struct Target {
    u32 challenge{}, step{}, index{};
    std::string origin;
    bool operator==(const Target&) const = default;
};
std::optional<std::vector<Target>> DecodeSnapshot(const nlohmann::json& value);
struct Port {
    std::function<bool(u64, u64)> range;
    std::function<std::optional<u8>(u64)> r8;
    std::function<std::optional<u16>(u64)> r16;
    std::function<std::optional<u32>(u64)> r32;
    std::function<std::optional<u64>(u64)> r64;
    std::function<bool(const Entry&, const std::array<u64, 8>&)> call;
    std::function<bool()> ready;
    std::function<bool(const Target&)> send;
    std::function<void(std::string_view)> log;
};
struct Manager {
    u64 pointer{};
    std::array<Table, 3> tables{}; // live count/data, used only as an identity between frames
    bool initialized{}, busy{}, restoring{}, gated{};
    bool Same(const Manager& other) const { return pointer == other.pointer && tables == other.tables; }
    bool Ready() const { return initialized && !busy && !restoring; }
};
struct Step {
    u32 hash{}, participants{};
    bool minigame{}, boss{};
};
struct Record {
    u32 challenge{}, index{}, current{}, pending{};
    u16 flags{};
    std::vector<Step> steps;
    std::optional<u32> Index(u32 hash) const;
};
std::optional<Manager> ReadManager(const Port& port, const Keys& keys, u64 base,
                                   bool* mismatch = nullptr);
std::optional<Record> ReadRecord(const Port& port, const Keys& keys, const Manager& manager,
                                u32 challenge);
bool Restoring(const Keys& keys, const Manager& manager, const Record& record);
/// A NotReady request left in the pending hash by save restoration and superseded by the game's
/// automatic progression (no request flag outstanding): not an own request for a bundle apply.
bool StaleNotReady(const Keys& keys, const Record& record);
bool CanApply(const Keys& keys, const Manager& manager, const Record& record, const Target& target);
/// Why a target may not be issued now (empty: it may). Diagnostics for the bounded quest log.
std::string_view ApplyBlock(const Keys& keys, const Manager& manager, const Record& record, const Target& target, const QuestDefer::Machine* defer = nullptr);

// No guest addresses: remote raw targets survive save epochs and are revalidated against each save.
class Book {
public:
    Book(std::shared_ptr<const Keys> keys, std::set<u64> denied = {});
    bool Mirrored(u32 challenge) const;
    std::string Remote(const Record& record, Target target);
    std::string Reconcile(const Record& record);
    std::optional<Target> Pending(u32 challenge) const;
    std::optional<u32> Known(u32 challenge) const;
    void Snapshot(const Record& record);
    bool Echo(const Record& record) const;
    void Issued(const Target& target);
    void Applied(const Target& target);
    bool Failed(u32 challenge); // true: exhausted the three attempts for this session
    void Epoch();
    std::vector<Target> Baseline(const std::vector<Record>& records) const;
private:
    struct Row {
        std::optional<u32> local, forwarded;
        std::optional<Target> team, pending, receipt;
        unsigned attempts{};
        bool exhausted{};
    };
    std::shared_ptr<const Keys> keys;
    std::set<u64> denied;
    std::map<u32, Row> rows;
};
class Controller {
public:
    Controller(std::shared_ptr<const Keys> keys, std::set<u64> denied, u64 base);
    ~Controller();
    QuestDefer::Machine& Defer() { return *defer; }
    void Queue(Target target);
    void Dirty(const Port& port, u64 state, u64 pc);
    bool Frame(Port& port, u64 frame, u64 bind_epoch, bool stable, bool negotiated, bool available, QuestDefer::Port* bundles = nullptr);
    void Returned(Port& port, u64 result);
    bool Calling() const;
    void Log(const Port& port, std::string_view line);
    void Reset();
    void Rebaseline() { baseline_sent.clear(); }
private:
    struct Attribution { bool restoration{}, replay{}; };
    struct Flight { Target target; u32 from{}; u64 frame{}; bool returned{}; };
    std::shared_ptr<const Keys> keys;
    Book book;
    std::unique_ptr<QuestDefer::Machine> defer;
    u64 base{}, tick{}, epoch{}, settle_until{};
    std::optional<Manager> manager;
    std::optional<Flight> flight;
    std::map<u32, Attribution> dirty;
    std::map<u32, Target> incoming;
    std::set<u32> baseline_sent, silent_logged, exhausted_logged;
    bool snapshotted{}, mismatch_logged{}, was_negotiated{};
    u64 lines{}, suppressed{}, reported{}, waiting_logged{};
    std::map<u32, std::string> blocked;
    void NewEpoch();
};
} // namespace Core::OotCoop::Quest
