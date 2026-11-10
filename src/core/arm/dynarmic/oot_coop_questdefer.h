// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/arm/dynarmic/oot_coop_quest.h"

namespace Core::OotCoop::QuestDefer {
struct Boss {
    u32 challenge{}, trigger{}, apply{};
    bool enabled{};
    std::string name;
    std::vector<std::pair<u32, u32>> chain;
    std::vector<u32> bools;
    std::vector<std::string> region;
};
struct Config {
    bool defer_in_activity{true}, death_skips{true}, skip_in_place{};
    bool during_gameover{};
    u32 boss_bool{};
    u64 death_load_frames{900}, exit_settle_frames{60};
    std::vector<std::string> death_commands;
    std::vector<Boss> bosses;
    const Boss* Find(u32 challenge) const;
};
std::shared_ptr<const Config> Decode(const nlohmann::json& quest);
enum class State { Idle, Pending, Deferred, Latched, Apply, Done, Failed };
struct Observation {
    std::optional<std::string> banc, command;
    std::optional<bool> loading, event, boss;
    bool stable{}, save_loaded{};
};
struct Port {
    std::function<std::optional<Quest::Record>(u32)> read;
    std::function<bool(const Quest::Target&, bool)> step;
    std::function<std::optional<bool>(u32)> read_bool;
    std::function<bool(u32)> grant_bool;
    std::function<void(std::string_view)> log, toast;
    std::function<std::string(std::string_view)> peer;
};
class Machine {
public:
    Machine(std::shared_ptr<const Quest::Keys> keys, std::set<u64> denied);
    void Queue(const Quest::Target& target);
    void Observe(const Observation& observation, u64 frame, const Port& port);
    bool Frame(Port& port, u64 frame, bool available);
    void Returned();
    void Gates(bool stable, bool loaded) { observation.stable = stable && observation.banc.has_value(); observation.save_loaded = loaded; }
    void Epoch();
    void Arrival();
    bool Watching() const;
    bool Calling() const;
    bool Authorizes(const Quest::Target& target) const;
    bool Echo(const Quest::Record& record) const;
    bool PrivateBool(u32 hash) const;
    std::vector<std::string> ExemptBancs() const;
    State Status(u32 challenge) const;
private:
    struct Row {
        State state{State::Idle};
        std::optional<Quest::Target> team, receipt;
        size_t element{};
        unsigned attempts{}, primary_attempts{};
        u64 since{}, outside{};
        bool waiting{}, returned{}, local_toast{}, deferred_toast{}, latched{}, arrival{}, pending_logged{};
    };
    std::shared_ptr<const Quest::Keys> keys;
    std::set<u64> denied;
    std::map<u32, Row> rows;
    std::map<u32, Quest::Target> receipts;
    Observation observation;
    std::optional<u64> death_candidate;
    std::optional<bool> previous_loading;
    bool respawn_live{}, death_committed{};
    u64 lines{}, watch_lines{};
    std::optional<u64> observed_frame;
    bool InRegion(const Boss& boss) const;
    void Log(const Port& port, std::string text);
    void Receipt(const Port& port, const Boss& boss, Row& row, std::string_view outcome);
};
} // namespace Core::OotCoop::QuestDefer
