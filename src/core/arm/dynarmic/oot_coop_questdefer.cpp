// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_questdefer.h"
#include <algorithm>
#include <charconv>
#include <fmt/format.h>
#include <utility>

namespace Core::OotCoop::QuestDefer {
namespace {
using Json = nlohmann::json;
bool Fields(const Json& j, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {}) {
    if (!j.is_object()) return false;
    for (auto f : required) if (!j.contains(std::string{f})) return false;
    for (const auto& [f, v] : j.items())
        if (std::find(required.begin(), required.end(), f) == required.end() &&
            std::find(optional.begin(), optional.end(), f) == optional.end()) return false;
    return true;
}
std::optional<u32> Number(const Json& j, u32 max = 0xffffffff) {
    u64 n{};
    if (j.is_number_unsigned()) n = j.get<u64>();
    else if (j.is_number_integer()) { if (j.get<s64>() < 0) return {}; n = j.get<u64>(); }
    else if (j.is_string()) {
        const auto s = j.get<std::string>();
        if (!s.starts_with("0x") || s.size() > 18) return {};
        const auto [end, ec] = std::from_chars(s.data() + 2, s.data() + s.size(), n, 16);
        if (ec != std::errc{} || end != s.data() + s.size()) return {};
    } else return {};
    if (!n || n > max) return {};
    return static_cast<u32>(n);
}
bool Text(const Json& j, size_t max) {
    if (!j.is_string()) return false;
    const auto s = j.get<std::string>();
    return !s.empty() && s.size() <= max &&
        std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool Strings(const Json& j, std::vector<std::string>& out, size_t max) {
    if (!j.is_array() || j.empty() || j.size() > max) return false;
    for (const auto& v : j) {
        if (!Text(v, 128)) return false;
        const auto s = v.get<std::string>();
        if (std::find(out.begin(), out.end(), s) != out.end()) return false;
        out.push_back(s);
    }
    return true;
}
}
const Boss* Config::Find(u32 c) const {
    for (const auto& b : bosses) if (b.challenge == c && b.enabled) return &b;
    return nullptr;
}
std::shared_ptr<const Config> Decode(const Json& j) {
    try {
        auto k = std::make_shared<Config>();
        for (auto [name, dest] : {std::pair{"defer_in_activity", &k->defer_in_activity},
            {"death_skips", &k->death_skips}, {"skip_in_place", &k->skip_in_place}}) {
            if (!j.contains(name)) continue;
            if (!j.at(name).is_boolean()) return {};
            *dest = j.at(name).get<bool>();
        }
        if (j.contains("death_apply")) {
            const auto& v = j.at("death_apply");
            if (v != "after_respawn" && v != "during_gameover") return {};
            k->during_gameover = v == "during_gameover";
        }
        if (j.contains("activity")) {
            const auto& a = j.at("activity");
            if (!Fields(a, {"boss_bool", "death_commands"}, {"death_load_frames", "exit_settle_frames"})) return {};
            const auto hash = Number(a.at("boss_bool"));
            if (!hash || !Strings(a.at("death_commands"), k->death_commands, 16)) return {};
            k->boss_bool = *hash;
            for (auto [name, dest] : {std::pair{"death_load_frames", &k->death_load_frames}, {"exit_settle_frames", &k->exit_settle_frames}}) {
                if (!a.contains(name)) continue;
                const auto n = Number(a.at(name), 36000);
                if (!n) return {};
                *dest = *n;
            }
        }
        if (!j.contains("bosses")) return k;
        const auto& bs = j.at("bosses");
        if (!bs.is_array() || bs.size() > 16 || (!bs.empty() && !j.contains("activity"))) return {};
        std::set<u32> unique;
        for (const auto& b : bs) {
            if (!Fields(b, {"challenge", "enabled", "name", "trigger", "apply", "chain", "bools", "region"})) return {};
            Boss boss;
            const auto c = Number(b.at("challenge")), t = Number(b.at("trigger")), a = Number(b.at("apply"));
            if (!c || !t || !a || !unique.insert(*c).second || !b.at("enabled").is_boolean() || !Text(b.at("name"), 32)) return {};
            boss.challenge = *c; boss.trigger = *t; boss.apply = *a;
            boss.enabled = b.at("enabled").get<bool>(); boss.name = b.at("name").get<std::string>();
            if (!Strings(b.at("region"), boss.region, 8)) return {};
            const auto& chain = b.at("chain"); const auto& bools = b.at("bools");
            if (!chain.is_array() || chain.size() > 4 || !bools.is_array() || bools.size() > 8) return {};
            for (const auto& e : chain) {
                if (!Fields(e, {"challenge", "step"})) return {};
                const auto ch = Number(e.at("challenge")), st = Number(e.at("step"));
                if (!ch || !st || *ch == *c) return {};
                boss.chain.emplace_back(*ch, *st);
            }
            for (const auto& e : bools) { const auto h = Number(e); if (!h) return {}; boss.bools.push_back(*h); }
            if (j.contains("mirror")) for (const auto& [hash, value] : j.at("mirror").items())
                if (Number(hash) == c) return {};
            k->bosses.push_back(std::move(boss));
        }
        return k;
    } catch (const Json::exception&) { return {}; }
}
Machine::Machine(std::shared_ptr<const Quest::Keys> k, std::set<u64> d) : keys{std::move(k)}, denied{std::move(d)} {
    if (keys->defer) for (const auto& b : keys->defer->bosses)
        if (b.enabled && !denied.contains(b.challenge)) rows.emplace(b.challenge, Row{});
}
void Machine::Log(const Port& p, std::string s) {
    if (lines++ < 511 && p.log) p.log("OoT co-op: questdefer " + s);
}
void Machine::Queue(const Quest::Target& t) {
    const auto it = rows.find(t.challenge);
    if (it == rows.end()) return;
    auto& r = it->second;
    if (!r.team || t.index > r.team->index) {
        r.team = t;
        if (r.state == State::Idle) r.state = State::Pending;
    }
}
bool Machine::InRegion(const Boss& b) const {
    return !observation.banc || !observation.boss || !observation.event || *observation.boss || *observation.event ||
        std::find(b.region.begin(), b.region.end(), *observation.banc) != b.region.end();
}
bool Machine::Watching() const {
    return std::any_of(rows.begin(), rows.end(), [](const auto& e) {
        return e.second.team && e.second.state != State::Idle && e.second.state != State::Done && e.second.state != State::Failed;
    });
}
void Machine::Observe(const Observation& o, u64 frame, const Port& p) {
    const auto before = observation;
    observation = o; observed_frame = frame;
    if (!keys->defer || !Watching()) { death_candidate.reset(); previous_loading = o.loading; return; }
    const auto& k = *keys->defer;
    const bool death = o.command && std::find(k.death_commands.begin(), k.death_commands.end(), *o.command) != k.death_commands.end();
    // Field diagnostics (bounded): every change of the sampled activity inputs while a bundle is watched.
    if (watch_lines < 400 && p.log && (before.command != o.command || before.loading != o.loading ||
        before.event != o.event || before.boss != o.boss || before.banc != o.banc)) {
        ++watch_lines;
        const auto text = [](const std::optional<std::string>& v) { return v ? *v : std::string{"?"}; };
        const auto flag = [](const std::optional<bool>& v) { return v ? (*v ? "1" : "0") : "?"; };
        p.log(fmt::format("OoT co-op: questdefer watch frame={} banc={} command={} loading={} event={} boss={} stable={}",
                          frame, text(o.banc), text(o.command), flag(o.loading), flag(o.event), flag(o.boss), o.stable));
    }
    // A rising load consumes the previous command; a freshly created player may already report a live command.
    const bool rising = o.loading == true && previous_loading == false;
    // The candidate is refreshed while a death command is held: the Game Over menu waits for the
    // player (field C17 sB: Dead at frame 11847, Try Again pressed 28 s later, load rose at 13550
    // with the command still DeadRagdoll), so the window counts from the last death frame.
    const bool latched = std::any_of(rows.begin(), rows.end(), [](const auto& e) { return e.second.state == State::Latched; });
    if (death && o.loading == false && !death_committed) death_candidate = frame;
    if (death_candidate && frame - *death_candidate > k.death_load_frames) death_candidate.reset();
    if (rising && death_candidate) {
        Log(p, k.death_skips ? "committed death: latched" : "committed death: death_skips off");
        for (auto& [c, r] : rows) if (r.state == State::Deferred && k.death_skips) { r.latched = true; r.state = State::Latched; }
        death_candidate.reset(); respawn_live = false; death_committed = true;
    } else if (o.command && !death && death_candidate) {
        Log(p, "revive: no respawn load"); death_candidate.reset();
    }
    // After the respawn load the player stays in the death command for seconds while the scene
    // restores (sB: DeadRagdoll until Fall/LandWater 7 s after the load): the latched bundle waits
    // for the first live command after the load.
    if (o.command && !death && o.loading == false && !respawn_live && latched) {
        respawn_live = true;
        Log(p, fmt::format("respawned: live command {}", *o.command));
    }
    if (o.command && !death && o.loading == false) death_committed = false; // alive again: a later death is new
    previous_loading = o.loading;
    for (auto& [c, r] : rows) {
        if (r.state != State::Deferred) continue;
        if (InRegion(*k.Find(c))) r.outside = 0;
        else if (o.stable && o.save_loaded && o.loading == false) ++r.outside;
        else r.outside = 0;
    }
}
void Machine::Receipt(const Port& p, const Boss& b, Row& r, std::string_view outcome) {
    const auto steps = 1 + b.chain.size();
    const auto element = r.element == 0 ? fmt::format("step={:08x}/{:08x}", b.challenge, b.apply) :
        r.element < steps ? fmt::format("chain={:08x}/{:08x}", b.chain[r.element - 1].first, b.chain[r.element - 1].second) :
        fmt::format("bool={:08x}", b.bools[r.element - steps]);
    Log(p, fmt::format("challenge={:08x} element={} {} {}", b.challenge, r.element, outcome, element));
}
bool Machine::Frame(Port& p, u64 frame, bool available) {
    if (!keys->defer || observed_frame != frame) return false;
    const auto& k = *keys->defer;
    for (auto& [c, r] : rows) {
        if (!r.team || r.state == State::Done || r.state == State::Failed) continue;
        const auto& b = *k.Find(c);
        const auto local = p.read(c);
        if (!local || local->Index(r.team->step) != r.team->index) continue;
        const auto trigger = local->Index(b.trigger), apply = local->Index(b.apply);
        if (!trigger || !apply || *trigger > *apply || r.team->index < *trigger) { r.state = State::Idle; continue; }
        if (r.state != State::Apply || (r.element == 0 && !r.waiting && r.primary_attempts == 0)) {
            if (local->index >= *apply) { r.state = State::Done; Log(p, "not applied: completed locally"); continue; }
            if (local->index >= *trigger) {
                if (!r.local_toast) { if (p.toast) p.toast("you beat it too"); r.local_toast = true; }
                r.state = State::Pending;
                continue; // The local reward chain owns completion.
            }
        }
        if (r.state != State::Apply) {
            if (r.state == State::Idle) r.state = State::Pending;
            if (r.latched && !k.during_gameover &&
                (!observation.stable || !observation.save_loaded || observation.loading != false || !respawn_live)) {
                r.state = State::Latched; continue;
            }
            if (r.arrival || r.latched || k.skip_in_place || !k.defer_in_activity ||
                (r.state == State::Pending && !InRegion(b)) || r.outside >= k.exit_settle_frames) {
                r.state = State::Apply; r.since = frame;
            } else {
                r.state = State::Deferred;
                if (!r.deferred_toast) {
                    Log(p, fmt::format("challenge={:08x} deferred", c));
                    if (p.toast) p.toast(fmt::format("{} beat {}. Teleport/die to skip", p.peer ? p.peer(r.team->origin) : r.team->origin, b.name));
                    r.deferred_toast = true;
                }
                continue;
            }
        }
        if (r.element == 0 && !r.waiting && InRegion(b) && !r.arrival && !r.latched &&
            k.defer_in_activity && !k.skip_in_place) { r.state = State::Deferred; r.outside = 0; continue; }
        if (!available || !observation.stable || !observation.save_loaded || observation.loading != false || observation.event != false) continue;
        if ((local->pending && local->pending != local->current && !Quest::StaleNotReady(*keys, *local)) ||
            (local->flags & keys->L("record.pending_mask"))) {
            if (!std::exchange(r.pending_logged, true))
                Log(p, fmt::format("challenge={:08x} waiting: own request pending (pending={:08x} current={:08x} flags={:04x})",
                                   c, local->pending, local->current, local->flags));
            continue;
        }
        const size_t steps = 1 + b.chain.size();
        const size_t total = steps + b.bools.size();
        if (r.element == total) {
            r.state = State::Done;
            if (p.toast) p.toast(fmt::format("{} skipped: synced from {}", b.name, p.peer ? p.peer(r.team->origin) : r.team->origin));
            Log(p, fmt::format("challenge={:08x} done", c)); continue;
        }
        const bool is_step = r.element < steps;
        const auto pair = r.element == 0 ? std::pair{c, b.apply} : is_step ? b.chain[r.element - 1] : std::pair<u32, u32>{};
        std::optional<Quest::Record> record;
        std::optional<u32> index;
        std::optional<bool> flag;
        if (is_step) { record = p.read(pair.first); if (record) index = record->Index(pair.second); }
        else flag = p.read_bool(b.bools[r.element - steps]);
        const bool reached = is_step ? record && index && record->index >= *index : flag == true;
        auto advance = [&] { ++r.element; r.attempts = 0; r.waiting = r.returned = false; r.since = frame; };
        if (reached && (!r.waiting || r.returned)) {
            Receipt(p, b, r, r.waiting ? "applied" : "already");
            if (r.element == 0) r.primary_attempts = 0;
            advance(); continue;
        }
        if (r.waiting) {
            if (!r.returned || frame - r.since < keys->apply_timeout) continue;
            Receipt(p, b, r, "failed(verify timeout)"); r.waiting = false;
            if (r.attempts >= 3) {
                if (r.element == 0) { r.state = State::Failed; Log(p, "gave up for session"); }
                else advance();
            }
            continue;
        }
        if (is_step && (!record || !index || record->current == keys->notready_hash)) {
            if (frame - r.since >= keys->apply_timeout && r.element != 0) { Receipt(p, b, r, "failed(NotReady or unreadable)"); advance(); }
            continue;
        }
        const Quest::Target t{pair.first, pair.second, index.value_or(0), r.team->origin};
        if (r.element == 0 && r.primary_attempts >= 3) {
            Receipt(p, b, r, "failed(session attempts exhausted)"); r.state = State::Failed; continue;
        }
        const bool issued = is_step ? p.step(t, r.element == 0) : flag.has_value() && p.grant_bool(b.bools[r.element - steps]);
        if (issued) { if (is_step) receipts[t.challenge] = t; if (r.element == 0) ++r.primary_attempts; r.receipt = is_step ? std::optional{t} : std::nullopt; r.waiting = true; r.returned = false; ++r.attempts; r.since = frame; return true; }
        if (frame - r.since >= keys->apply_timeout && r.element != 0) { Receipt(p, b, r, "failed(guard or setter unavailable)"); advance(); }
    }
    return false;
}
void Machine::Returned() { for (auto& [c, r] : rows) if (r.waiting && !r.returned) r.returned = true; }
bool Machine::Calling() const { for (const auto& [c, r] : rows) if (r.waiting && !r.returned) return true; return false; }
bool Machine::Authorizes(const Quest::Target& t) const {
    const auto it = rows.find(t.challenge);
    const auto* b = keys->defer ? keys->defer->Find(t.challenge) : nullptr;
    return b && it != rows.end() && it->second.state == State::Apply && t.step == b->apply &&
        (!InRegion(*b) || it->second.arrival || it->second.latched || keys->defer->skip_in_place || !keys->defer->defer_in_activity);
}
bool Machine::Echo(const Quest::Record& r) const {
    const auto it = receipts.find(r.challenge);
    return it != receipts.end() && it->second.index == r.index && it->second.step == r.current;
}
bool Machine::PrivateBool(u32 h) const {
    if (keys->defer) for (const auto& b : keys->defer->bosses)
        if (std::find(b.bools.begin(), b.bools.end(), h) != b.bools.end()) return true;
    return false;
}
void Machine::Epoch() {
    receipts.clear();
    death_candidate.reset(); previous_loading.reset(); respawn_live = false; death_committed = false;
    for (auto& [c, r] : rows) {
        if (r.state == State::Failed) continue;
        const auto team = r.team;
        const bool unconsumed = r.state == State::Deferred || r.state == State::Latched ||
            (r.state == State::Apply && r.element == 0 && !r.waiting && r.primary_attempts == 0);
        const bool latched = unconsumed && r.latched, arrival = unconsumed && r.arrival;
        const unsigned attempts = r.primary_attempts;
        const State state = r.state == State::Deferred || r.state == State::Latched ? r.state : r.team ? State::Pending : State::Idle;
        const bool local_toast = unconsumed && r.local_toast, deferred_toast = unconsumed && r.deferred_toast;
        r = {}; r.state = state; r.local_toast = local_toast; r.deferred_toast = deferred_toast; r.team = team; r.latched = latched; r.arrival = arrival; r.primary_attempts = attempts;
    }
}
void Machine::Arrival() { for (auto& [c, r] : rows) if (r.state == State::Deferred || r.state == State::Latched) r.arrival = true; }
std::vector<std::string> Machine::ExemptBancs() const {
    std::vector<std::string> result;
    if (keys->defer) for (const auto& [c, r] : rows) if (r.state == State::Deferred) {
        const auto& region = keys->defer->Find(c)->region; result.insert(result.end(), region.begin(), region.end());
    }
    return result;
}
State Machine::Status(u32 c) const { const auto it = rows.find(c); return it == rows.end() ? State::Idle : it->second.state; }
} // namespace Core::OotCoop::QuestDefer
