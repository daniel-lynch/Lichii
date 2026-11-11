// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_quest.h"
#include "core/arm/dynarmic/oot_coop_questdefer.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <fmt/format.h>

namespace Core::OotCoop::Quest {
namespace {
using Json = nlohmann::json;
constexpr u64 ModuleLimit = 0x10000000;
bool Fields(const Json& j, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {}) {
    if (!j.is_object()) return false;
    for (const auto name : required) if (!j.contains(std::string{name})) return false;
    for (const auto& [name, unused] : j.items())
        if (std::find(required.begin(), required.end(), name) == required.end() &&
            std::find(optional.begin(), optional.end(), name) == optional.end()) return false;
    return true;
}
std::optional<u64> Number(const Json& j, u64 max) {
    if (j.is_number_unsigned()) { const auto n = j.get<u64>(); return n <= max ? std::optional{n} : std::nullopt; }
    if (j.is_number_integer()) { const auto n = j.get<s64>(); return n >= 0 && u64(n) <= max ? std::optional{u64(n)} : std::nullopt; }
    if (!j.is_string()) return {};
    const auto s = j.get<std::string>();
    if (!s.starts_with("0x") || s.size() > 18) return {};
    u64 n{};
    const auto [end, ec] = std::from_chars(s.data() + 2, s.data() + s.size(), n, 16);
    if (ec != std::errc{} || end != s.data() + s.size() || n > max) return {};
    return n;
}
bool Numbers(const Json& j, std::map<std::string, u64, std::less<>>& out,
             std::string_view prefix, std::initializer_list<std::string_view> fields) {
    if (!Fields(j, fields)) return false;
    for (auto name : fields) {
        const auto n = Number(j.at(std::string{name}), 0x10000);
        if (!n) return false;
        out.emplace(std::string{prefix} + std::string{name}, *n);
    }
    return true;
}
std::optional<Entry> ReadEntry(const Json& j) {
    if (!Fields(j, {"at", "words"})) return {};
    const auto at = Number(j.at("at"), ModuleLimit);
    const auto& words = j.at("words");
    if (!at || !*at || (*at & 3) || !words.is_array() || words.empty() || words.size() > 16) return {};
    Entry e{*at, {}};
    for (const auto& word : words) {
        const auto n = Number(word, 0xffffffff);
        if (!n) return {};
        e.words.push_back(static_cast<u32>(*n));
    }
    return e;
}
bool Span(const Port& p, u64 at, u64 size) {
    return at && size && at <= std::numeric_limits<u64>::max() - size && p.range(at, size);
}
}
std::shared_ptr<const Keys> DecodeKeys(const Json& j) {
    try {
        if (!Fields(j, {"manager", "entries", "commit_observation", "layout", "setter_arguments",
                        "mirror", "notready_hash", "complete_hash", "ready_hash"},
                       {"enabled", "settle_frames", "apply_timeout", "defer_in_activity", "death_skips",
                        "skip_in_place", "death_apply", "activity", "bosses"})) return {};
        auto k = std::make_shared<Keys>();
        k->defer = QuestDefer::Decode(j);
        if (!k->defer) return {};
        if (j.contains("enabled")) {
            if (!j.at("enabled").is_boolean()) return {};
            k->enabled = j.at("enabled").get<bool>();
        }
        for (auto [name, dest] : {std::pair{"settle_frames", &k->settle_frames}, {"apply_timeout", &k->apply_timeout}}) {
            if (!j.contains(name)) continue;
            const auto n = Number(j.at(name), 36000);
            if (!n || !*n) return {};
            *dest = *n;
        }
        for (auto [name, dest] : {std::pair{"notready_hash", &k->notready_hash},
                                  {"complete_hash", &k->complete_hash}, {"ready_hash", &k->ready_hash}}) {
            const auto n = Number(j.at(name), 0xffffffff);
            if (!n || !*n) return {};
            *dest = static_cast<u32>(*n);
        }
        if (k->notready_hash == k->ready_hash || k->complete_hash == k->ready_hash ||
            k->notready_hash == k->complete_hash) return {};
        const auto& m = j.at("manager");
        if (!Fields(m, {"cell", "dereferences", "got_cell", "got_dereferences", "tables", "record_stride",
                        "initialized", "rollback_seen", "restoring", "suppressed", "busy", "dependency_gate"})) return {};
        for (const auto& [name, value] : m.items()) {
            if (name == "tables") continue;
            const auto n = Number(value, name == "cell" || name == "got_cell" ? ModuleLimit : 0x10000);
            if (!n) return {};
            k->manager.emplace(name, *n);
        }
        if (!k->M("cell") || (k->M("cell") & 7) || !k->M("got_cell") || (k->M("got_cell") & 7) ||
            k->M("dereferences") != 1 || k->M("got_dereferences") != 2 ||
            !k->M("record_stride") || k->M("record_stride") > 4096) return {};
        const auto& tables = m.at("tables");
        if (!tables.is_array() || tables.size() != k->tables.size()) return {};
        std::set<u64> locations;
        for (size_t i = 0; i < tables.size(); ++i) {
            if (!Fields(tables[i], {"count", "data"})) return {};
            const auto count = Number(tables[i].at("count"), 4096), data = Number(tables[i].at("data"), 4096);
            if (!count || !data || (*count & 3) || (*data & 7) ||
                !locations.insert(*count).second || !locations.insert(*data).second) return {};
            k->tables[i] = {*count, *data};
        }
        const auto& layout = j.at("layout");
        if (!Fields(layout, {"record", "state", "step"}) ||
            !Numbers(layout.at("record"), k->layout, "record.", {"challenge", "active_condition", "current_hash",
                "flags_u16", "state_embedded", "restoration_mask", "pending_mask"}) ||
            !Numbers(layout.at("state"), k->layout, "state.", {"record_backlink", "initial_dependency", "current_index",
                "step_count", "step_array", "enum_key", "pending_hash"}) ||
            !Numbers(layout.at("step"), k->layout, "step.", {"stride", "hash", "force_progress_condition", "is_minigame",
                "is_boss_battle", "stop_egg_hatch", "safety_setting_hash", "participant_count", "participants"})) return {};
        const auto fits = [](u64 offset, u64 bytes, u64 size) { return offset <= size && bytes <= size - offset; };
        const u64 stride = k->M("record_stride"), state = k->L("record.state_embedded"), step = k->L("step.stride");
        if (!state || state >= stride || !step || step > 4096 || !k->L("record.restoration_mask") ||
            k->L("record.restoration_mask") > 0xffff || !k->L("record.pending_mask") || k->L("record.pending_mask") > 0xffff) return {};
        for (auto [field, value] : k->layout) {
            if (field.ends_with("mask") || field == "step.stride") continue;
            const u64 width = field == "record.flags_u16" ? 2 :
                field == "record.state_embedded" ? 1 :
                field == "state.record_backlink" || field == "state.step_array" || field == "step.participants" ? 8 :
                field == "step.is_minigame" || field == "step.is_boss_battle" || field == "step.stop_egg_hatch" ? 1 : 4;
            if (!fits(value, width, field.starts_with("record.") ? stride : field.starts_with("state.") ? stride - state : step) ||
                (value % width)) return {};
        }
        if (!Fields(j.at("entries"), {"setter"})) return {};
        const auto setter = ReadEntry(j.at("entries").at("setter"));
        if (!setter) return {};
        k->setter = *setter;
        const auto& args = j.at("setter_arguments");
        if (!Fields(args, {"w3", "w4", "w5"})) return {};
        for (size_t i = 0; i < 3; ++i) {
            const auto n = Number(args.at("w" + std::to_string(i + 3)), 1);
            if (!n || *n != k->arguments[i]) return {}; // silent, no subentries, NEVER force
        }
        const auto& observers = j.at("commit_observation");
        if (!observers.is_array() || observers.empty() || observers.size() > 4) return {};
        std::set<u64> sites;
        for (const auto& o : observers) {
            if (!Fields(o, {"at", "words", "resume", "state_register", "record_from_state"}) || o.at("resume") != "pacibsp") return {};
            const auto e = ReadEntry(Json{{"at", o.at("at")}, {"words", o.at("words")}});
            // Architectural PACIBSP hint, not a build-specific instruction sequence.
            if (!e || e->words[0] != 0xd503237fU || !sites.insert(e->offset).second) return {};
            const auto reg = Number(o.at("state_register"), 30);
            if (!reg || !o.at("record_from_state").is_string()) return {};
            const auto relative = o.at("record_from_state").get<std::string>();
            if (!relative.starts_with("-0x")) return {};
            const auto amount = Number(relative.substr(1), 4096);
            if (!amount || *amount != state) return {};
            k->observers.push_back({*e, static_cast<u32>(*reg), *amount});
        }
        const auto& mirror = j.at("mirror");
        if (!mirror.is_object() || mirror.empty() || mirror.size() > 256) return {};
        for (const auto& [hash, list] : mirror.items()) {
            const auto challenge = Number(hash, 0xffffffff);
            if (!challenge || !*challenge || !list.is_array() || list.empty() || list.size() > 256) return {};
            std::vector<u32> safe;
            std::set<u32> unique;
            for (const auto& value : list) {
                const auto n = Number(value, 0xffffffff);
                if (!n || !*n || *n == k->notready_hash || !unique.insert(static_cast<u32>(*n)).second) return {};
                safe.push_back(static_cast<u32>(*n));
            }
            if (safe.front() != k->ready_hash || !k->mirror.emplace(static_cast<u32>(*challenge), std::move(safe)).second) return {};
        }
        return k;
    } catch (const Json::exception&) { return {}; }
}
std::optional<std::vector<Target>> DecodeSnapshot(const Json& value) {
    if (!value.is_array() || value.size() > 256) return {};
    std::vector<Target> result;
    std::set<u32> unique;
    for (const auto& p : value) {
        if (!Fields(p, {"type", "challenge", "step", "index"}) || p.at("type") != "quest_step") return {};
        for (auto field : {"challenge", "step", "index"}) if (!p.at(field).is_number_integer()) return {};
        const auto c = Number(p.at("challenge"), 0xffffffff), s = Number(p.at("step"), 0xffffffff), i = Number(p.at("index"), 255);
        if (!c || !*c || !s || !*s || !i || !unique.insert(static_cast<u32>(*c)).second) return {};
        result.push_back({static_cast<u32>(*c), static_cast<u32>(*s), static_cast<u32>(*i), {}});
    }
    return result;
}
std::optional<u32> Record::Index(u32 hash) const {
    for (size_t i = 0; i < steps.size(); ++i) if (steps[i].hash == hash) return static_cast<u32>(i);
    return {};
}
std::optional<Manager> ReadManager(const Port& p, const Keys& k, u64 base, bool* mismatch) {
    if (mismatch) *mismatch = false;
    if (!base || base > std::numeric_limits<u64>::max() - ModuleLimit) return {};
    const auto direct = p.r64(base + k.M("cell")), cell = p.r64(base + k.M("got_cell"));
    const auto indirect = cell && *cell ? p.r64(*cell) : std::nullopt;
    if (!direct || !*direct || !indirect) return {};
    if (*direct != *indirect) { if (mismatch) *mismatch = true; return {}; }
    u64 extent = 1;
    for (const auto& [field, offset] : k.manager)
        if (field != "cell" && field != "got_cell" && field != "record_stride") extent = std::max(extent, offset + 1);
    for (const auto& t : k.tables) extent = std::max({extent, t.count + 4, t.data + 8});
    if (!Span(p, *direct, extent)) return {};
    const auto initialized = p.r8(*direct + k.M("initialized")), busy = p.r8(*direct + k.M("busy")),
               restoring = p.r8(*direct + k.M("restoring")), suppressed = p.r8(*direct + k.M("suppressed")),
               dependency = p.r8(*direct + k.M("dependency_gate"));
    if (!initialized || !busy || !restoring || !suppressed || !dependency) return {};
    // Only the suppress byte's bit 0 gates the update's request path; the dependency byte gates
    // NotReady processing alone (the update's own checks), so it is read but not a receiver gate.
    Manager m{*direct, {}, *initialized != 0, *busy != 0, (*restoring & 1) != 0, (*suppressed & 1) != 0};
    for (size_t i = 0; i < k.tables.size(); ++i) {
        const auto count = p.r32(*direct + k.tables[i].count);
        const auto data = p.r64(*direct + k.tables[i].data);
        if (!count || *count > 256 || !data || (*count && !Span(p, *data, *count * k.M("record_stride")))) return {};
        m.tables[i] = {*count, *data};
    }
    return m;
}
std::optional<Record> ReadRecord(const Port& p, const Keys& k, const Manager& m, u32 challenge) {
    u64 record = 0;
    for (const auto& table : m.tables) {
        if (table.count > 256 || (table.count && !Span(p, table.data, table.count * k.M("record_stride")))) return {};
        for (u64 i = 0; i < table.count; ++i) {
            const u64 at = table.data + i * k.M("record_stride");
            const auto hash = p.r32(at + k.L("record.challenge"));
            if (!hash) return {};
            if (*hash == challenge) { if (record) return {}; record = at; }
        }
    }
    if (!record) return {};
    const u64 state = record + k.L("record.state_embedded");
    if (p.r64(state + k.L("state.record_backlink")) != record) return {};
    const auto index = p.r32(state + k.L("state.current_index")), count = p.r32(state + k.L("state.step_count")),
               current = p.r32(record + k.L("record.current_hash")), pending = p.r32(state + k.L("state.pending_hash"));
    const auto flags = p.r16(record + k.L("record.flags_u16"));
    const auto steps = p.r64(state + k.L("state.step_array"));
    if (!index || !count || !current || !pending || !flags || !steps || !*count || *count > 256 || *index >= *count ||
        !Span(p, *steps, *count * k.L("step.stride"))) return {};
    Record r{challenge, *index, *current, *pending, *flags, {}};
    std::set<u32> unique;
    for (u32 i = 0; i < *count; ++i) {
        const u64 at = *steps + u64{i} * k.L("step.stride");
        const auto hash = p.r32(at + k.L("step.hash")), participants = p.r32(at + k.L("step.participant_count"));
        const auto minigame = p.r8(at + k.L("step.is_minigame")), boss = p.r8(at + k.L("step.is_boss_battle"));
        if (!hash || !*hash || !unique.insert(*hash).second || !participants || !minigame || !boss) return {};
        r.steps.push_back({*hash, *participants, *minigame != 0, *boss != 0});
    }
    if (r.steps[r.index].hash != r.current || p.r32(state + k.L("state.current_index")) != r.index ||
        p.r32(record + k.L("record.current_hash")) != r.current || p.r64(state + k.L("state.step_array")) != steps ||
        p.r32(state + k.L("state.step_count")) != count || p.r32(record + k.L("record.challenge")) != challenge ||
        p.r64(state + k.L("state.record_backlink")) != record ||
        p.r32(state + k.L("state.pending_hash")) != r.pending ||
        p.r16(record + k.L("record.flags_u16")) != r.flags) return {};
    return r;
}
bool Restoring(const Keys& k, const Manager& m, const Record& r) {
    return m.restoring || (r.flags & k.L("record.restoration_mask"));
}
bool StaleNotReady(const Keys& k, const Record& r) {
    // Field C17 F-A: the New Game restoration burst applies a NotReady request through the pending
    // path, then the game's automatic next-step progression moves the index on to Ready without
    // rewriting state.pending_hash, so the record keeps pending=NotReady != current, no request flag.
    return r.pending == k.notready_hash && r.current != k.notready_hash && !(r.flags & k.L("record.pending_mask"));
}
std::string_view ApplyBlock(const Keys& k, const Manager& m, const Record& r, const Target& t, const QuestDefer::Machine* defer) {
    if (!m.Ready()) return "manager not ready (uninitialized, busy or restoring)";
    if (m.gated) return "manager suppressed";
    if (Restoring(k, m, r)) return "record restoring";
    if (r.challenge != t.challenge || r.index >= r.steps.size() || r.steps[r.index].hash != r.current ||
        t.index >= r.steps.size() || r.steps[t.index].hash != t.step) return "target or record inconsistent";
    if (r.index >= t.index) return "already at or past target";
    if (r.current == k.notready_hash) return "NotReady";
    if (r.current == k.complete_hash) return "Complete";
    const bool bundle = defer && defer->Authorizes(t);
    // Field C17.2 F-A: on a vanilla New Game a bundle's chain step meets the same stale restoration pending.
    // It is exempt from that pending only; the boss/minigame lift stays primary-only.
    const bool chain = !bundle && defer && defer->AuthorizesChain(t);
    if ((r.pending && r.pending != r.current && !((bundle || chain) && StaleNotReady(k, r))) ||
        (r.flags & k.L("record.pending_mask"))) return "own request pending";
    if (r.steps[r.index].participants || r.steps[t.index].participants) return "participants";
    if (!bundle &&
        (r.steps[t.index].minigame || r.steps[t.index].boss || r.steps[r.index].minigame || r.steps[r.index].boss))
        return "minigame or boss step";
    return {};
}
bool CanApply(const Keys& k, const Manager& m, const Record& r, const Target& t) {
    return ApplyBlock(k, m, r, t).empty();
}
Book::Book(std::shared_ptr<const Keys> keys_, std::set<u64> denied_) : keys{std::move(keys_)}, denied{std::move(denied_)} {}
bool Book::Mirrored(u32 c) const { return (keys->mirror.contains(c) || (keys->defer && keys->defer->Find(c))) && !denied.contains(c); }
std::string Book::Remote(const Record& r, Target t) {
    if (!Mirrored(t.challenge) || r.challenge != t.challenge) return "not mirrored or denied";
    const auto index = r.Index(t.step);
    if (!index || *index != t.index) return "unknown step or index mismatch";
    auto& row = rows[t.challenge];
    if (row.team && row.team->index >= t.index) return "equal or backwards team target";
    row.team = std::move(t);
    return Reconcile(r);
}
std::string Book::Reconcile(const Record& r) {
    if (!Mirrored(r.challenge)) return "not mirrored or denied";
    auto& row = rows[r.challenge];
    row.pending.reset();
    if (row.exhausted) return "gave up for session";
    if (!row.team) return "no remote target";
    if (r.Index(row.team->step) != row.team->index) return "unknown step or index mismatch";
    if (keys->defer && keys->defer->Find(r.challenge)) return "bundle only";
    if (r.current == keys->complete_hash) return "Complete: dropped";
    std::optional<Target> floor;
    std::optional<u32> previous;
    for (u32 hash : keys->mirror.at(r.challenge)) {
        const auto index = r.Index(hash);
        if (!index || (previous && *index <= *previous)) return "safe list order mismatch";
        previous = index;
        if (*index <= row.team->index) floor = Target{r.challenge, hash, *index, row.team->origin};
    }
    if (!floor || floor->index <= r.index) return "already at or past safe floor";
    row.pending = floor;
    return r.current == keys->notready_hash ? "NotReady: waiting" : "queued";
}
std::optional<Target> Book::Pending(u32 c) const { const auto i = rows.find(c); return i == rows.end() ? std::nullopt : i->second.pending; }
std::optional<u32> Book::Known(u32 c) const { const auto i = rows.find(c); return i == rows.end() ? std::nullopt : i->second.local; }
void Book::Snapshot(const Record& r) { auto& row = rows[r.challenge]; row.local = r.index; row.forwarded = r.index; }
bool Book::Echo(const Record& r) const {
    const auto i = rows.find(r.challenge);
    return i != rows.end() && i->second.receipt && i->second.receipt->index == r.index && i->second.receipt->step == r.current;
}
void Book::Issued(const Target& t) { rows[t.challenge].receipt = t; ++rows[t.challenge].attempts; }
void Book::Applied(const Target& t) { auto& row = rows[t.challenge]; row.receipt = t; row.pending.reset(); row.attempts = 0; }
bool Book::Failed(u32 c) { auto& row = rows[c]; row.exhausted = row.attempts >= 3; if (row.exhausted) row.pending.reset(); return row.exhausted; }
void Book::Epoch() {
    for (auto& [c, row] : rows) {
        row.local.reset(); row.forwarded.reset(); row.pending.reset(); row.receipt.reset();
        if (row.attempts >= 3) row.exhausted = true;
    }
}
std::vector<Target> Book::Baseline(const std::vector<Record>& records) const {
    std::vector<Target> result;
    for (const auto& r : records) {
        if (!Mirrored(r.challenge)) continue;
        const auto ready = r.Index(keys->ready_hash);
        if (ready && r.index > *ready) result.push_back({r.challenge, r.current, r.index, {}});
    }
    return result;
}
Controller::Controller(std::shared_ptr<const Keys> keys_, std::set<u64> denied, u64 base_)
    : keys{std::move(keys_)}, book{keys, denied}, defer{std::make_unique<QuestDefer::Machine>(keys, std::move(denied))}, base{base_} {}
Controller::~Controller() = default;
bool Controller::Calling() const { return (flight && !flight->returned) || defer->Calling(); }
void Controller::Log(const Port& p, std::string_view line) {
    if (lines < 511) { ++lines; p.log("OoT co-op: quest " + std::string{line}); }
    else ++suppressed;
}
void Controller::Queue(Target t) {
    if (!book.Mirrored(t.challenge)) return;
    const auto old = incoming.find(t.challenge);
    if (old == incoming.end() || old->second.index < t.index) incoming[t.challenge] = std::move(t);
}
void Controller::NewEpoch() {
    book.Epoch(); defer->Epoch(); dirty.clear(); flight.reset(); baseline_sent.clear(); silent_logged.clear(); blocked.clear();
    snapshotted = false; settle_until = tick + keys->settle_frames;
}
void Controller::Reset() { manager.reset(); NewEpoch(); }
void Controller::Dirty(const Port& p, u64 state, u64 pc) {
    const auto site = std::find_if(keys->observers.begin(), keys->observers.end(),
                                  [&](const auto& o) { return base + o.entry.offset == pc; });
    if (site == keys->observers.end() || state < site->state_offset) return;
    const u64 record = state - site->state_offset;
    if (!Span(p, record, keys->M("record_stride")) || p.r64(state + keys->L("state.record_backlink")) != record) return;
    const auto hash = p.r32(record + keys->L("record.challenge"));
    const auto flags = p.r16(record + keys->L("record.flags_u16"));
    const auto m = ReadManager(p, *keys, base);
    if (!hash || !book.Mirrored(*hash)) return;
    auto& a = dirty[*hash];
    a.restoration |= !flags || !m || !m->initialized || m->restoring || (*flags & keys->L("record.restoration_mask"));
    a.replay |= flight && flight->target.challenge == *hash;
}
bool Controller::Frame(Port& p, u64 frame, u64 bind_epoch, bool stable, bool negotiated, bool available, QuestDefer::Port* bundles) {
    tick = frame;
    if (tick % 3600 == 0 && suppressed != reported) {
        p.log(fmt::format("OoT co-op: quest suppressed lines={} frame={}", suppressed, tick)); reported = suppressed;
    }
    if (Calling()) return false;
    if (was_negotiated != negotiated) { baseline_sent.clear(); was_negotiated = negotiated; }
    if (!stable) { Reset(); return false; }
    bool mismatch = false;
    const auto now = ReadManager(p, *keys, base, &mismatch);
    if (mismatch && !mismatch_logged) { Log(p, "manager chain mismatch: not ready"); mismatch_logged = true; }
    if (!now || !now->initialized) { Reset(); return false; }
    if (!manager || !manager->Same(*now) || epoch != bind_epoch) {
        NewEpoch(); manager = now; epoch = bind_epoch;
        Log(p, "new epoch: settling, remote targets retained");
    }
    manager = now;
    bool settling = tick < settle_until;
    // Poll all mirrored records every 60 frames, plus dirty, pending, verification and initial snapshots.
    const bool poll = tick % 60 == 0 || !snapshotted;
    std::vector<Record> records;
    std::set<u32> challenges;
    for (const auto& [c, safe] : keys->mirror) challenges.insert(c);
    if (keys->defer) for (const auto& b : keys->defer->bosses) if (book.Mirrored(b.challenge)) challenges.insert(b.challenge);
    for (const auto c : challenges) {
        if (!book.Mirrored(c) || (!poll && !dirty.contains(c) && !incoming.contains(c) && !book.Pending(c) &&
            !(flight && flight->target.challenge == c) && (!negotiated || baseline_sent.contains(c)))) continue;
        const auto r = ReadRecord(p, *keys, *now, c);
        if (r) records.push_back(*r);
    }
    const auto checked = ReadManager(p, *keys, base);
    if (!checked || !checked->Same(*now)) { Reset(); return false; }
    if (checked->busy) {
        if (flight && flight->returned && tick - flight->frame >= keys->apply_timeout) {
            Log(p, fmt::format("not applied challenge={:08x} step={:08x} (manager busy at timeout{})",
                flight->target.challenge, flight->target.step,
                book.Failed(flight->target.challenge) ? "; gave up for session" : "; retry when ready"));
            flight.reset();
        }
        return false;
    }
    const bool restoration = checked->restoring ||
        std::any_of(records.begin(), records.end(), [&](const auto& r) { return Restoring(*keys, *checked, r); }) ||
        std::any_of(dirty.begin(), dirty.end(), [](const auto& entry) { return entry.second.restoration; });
    if (restoration) {
        if (snapshotted) { NewEpoch(); Log(p, "restoration: settling again, not forwarded"); }
        settle_until = tick + keys->settle_frames;
        settling = true;
    }
    size_t rises = 0;
    for (const auto& r : records) {
        const auto known = book.Known(r.challenge);
        if (known && r.index > *known && !book.Echo(r) && !defer->Echo(r)) ++rises;
    }
    const bool bulk = snapshotted && rises > 1;
    if (bulk && !settling) { NewEpoch(); Log(p, "bulk change: settling again, not forwarded"); }
    const bool quiet = settling || bulk || !checked->Ready();
    for (const auto& r : records) {
        const auto known = book.Known(r.challenge);
        const auto a = dirty.contains(r.challenge) ? dirty.at(r.challenge) : Attribution{};
        const bool restore = a.restoration || Restoring(*keys, *checked, r);
        const bool echo = a.replay || book.Echo(r) || defer->Echo(r) || (flight && flight->target.challenge == r.challenge);
        const bool moved = known && *known != r.index;
        if (moved) {
            const bool forward = snapshotted && !quiet && !restore && !echo && negotiated && r.index > *known;
            const bool sent = forward && p.send({r.challenge, r.current, r.index, {}});
            if (echo) Log(p, fmt::format("echo suppressed challenge={:08x} step={:08x} index={}", r.challenge, r.current, r.index));
            if ((!quiet && !restore) || silent_logged.insert(r.challenge).second)
                Log(p, fmt::format("observed challenge={:08x} from=#{} to=#{} step={:08x} {} ({})", r.challenge,
                    *known, r.index, r.current, sent ? "forwarded" : "not forwarded",
                    echo ? "our replay" : restore ? "restoration" : quiet || !snapshotted ? "settle/baseline" :
                    r.index < *known ? "backwards" : !negotiated ? "capability off" : !sent ? "send queue full" : "committed"));
            if (forward && !sent) continue;
        }
        book.Snapshot(r);
        dirty.erase(r.challenge);
        if (flight && flight->returned && flight->target.challenge == r.challenge) {
            if (r.index == flight->target.index && r.current == flight->target.step && !restore) {
                Log(p, fmt::format("applied challenge={:08x} step={:08x} index={}", r.challenge, r.current, r.index));
                book.Applied(flight->target); flight.reset();
            } else if (r.index > flight->target.index) {
                Log(p, fmt::format("not applied challenge={:08x} (advanced past target; no retry)", r.challenge));
                book.Applied(flight->target); flight.reset();
            }
        }
        if (const auto in = incoming.find(r.challenge); in != incoming.end()) {
            const auto reason = book.Remote(r, in->second);
            if (r.Index(in->second.step) == in->second.index) defer->Queue(in->second);
            Log(p, fmt::format("remote challenge={:08x} step={:08x} index={} ({})", r.challenge,
                              in->second.step, in->second.index, reason));
            incoming.erase(in);
        }
        const auto decision = book.Reconcile(r);
        if (decision == "gave up for session" && exhausted_logged.insert(r.challenge).second)
            Log(p, fmt::format("not applied challenge={:08x} (gave up for session)", r.challenge));
    }
    if (flight && flight->returned && tick - flight->frame >= keys->apply_timeout) {
        const auto c = flight->target.challenge;
        Log(p, fmt::format("not applied challenge={:08x} step={:08x} (commit timeout{})", c,
                          flight->target.step, book.Failed(c) ? "; gave up for session" : "; retry"));
        flight.reset();
    }
    if (quiet) return false;
    if (!snapshotted) {
        size_t expected = 0;
        for (const auto c : challenges) if (book.Mirrored(c)) ++expected;
        if (records.size() != expected || std::any_of(records.begin(), records.end(),
            [&](const auto& r) { return Restoring(*keys, *checked, r); })) return false;
        snapshotted = true;
    }
    if (negotiated) {
        for (const auto& r : records) {
            const auto ready = r.Index(keys->ready_hash);
            if (ready && r.index <= *ready && !Restoring(*keys, *checked, r) && baseline_sent.insert(r.challenge).second)
                Log(p, fmt::format("baseline challenge={:08x} step={:08x} index={} (at/below Ready; not published)",
                                  r.challenge, r.current, r.index));
        }
        for (const auto& t : book.Baseline(records)) {
            const auto r = std::find_if(records.begin(), records.end(), [&](const auto& v) { return v.challenge == t.challenge; });
            if (baseline_sent.contains(t.challenge) || Restoring(*keys, *checked, *r) || defer->Echo(*r)) continue;
            if (p.send(t)) {
                baseline_sent.insert(t.challenge);
                Log(p, fmt::format("baseline challenge={:08x} step={:08x} index={}", t.challenge, t.step, t.index));
            }
        }
    }
    if (bundles) {
        bundles->read = [&](u32 c) -> std::optional<Record> {
            const auto m = ReadManager(p, *keys, base);
            if (!m || !m->Same(*checked) || !m->Ready() || m->gated) return {};
            const auto r = ReadRecord(p, *keys, *m, c);
            const auto last = ReadManager(p, *keys, base);
            if (!r || !last || !last->Same(*m) || !last->Ready() || Restoring(*keys, *last, *r)) return {};
            return r;
        };
        bundles->step = [&](const Target& t, bool) -> std::string {
            // ApplyBlock decides primary vs chain from the machine (Authorizes / AuthorizesChain).
            const auto m = ReadManager(p, *keys, base);
            if (!m || !m->Same(*checked)) return "manager changed or unreadable";
            const auto r = ReadRecord(p, *keys, *m, t.challenge);
            const auto last = ReadManager(p, *keys, base);
            if (!r || !last || !last->Same(*m)) return "record unreadable or manager moved";
            if (!p.ready()) return "applier not ready";
            if (const auto why = ApplyBlock(*keys, *last, *r, t, defer.get()); !why.empty()) return std::string{why};
            if (!p.call(keys->setter, {m->pointer, t.challenge, t.step, keys->arguments[0], keys->arguments[1], 0, 0, 0}))
                return "setter call refused";
            return {};
        };
        if (defer->Frame(*bundles, tick, negotiated && available && !flight && p.ready())) return true;
    }
    if (!negotiated || !available || flight || !p.ready()) {
        if (negotiated && !flight && tick - waiting_logged >= 600 &&
            std::any_of(records.begin(), records.end(), [&](const auto& r) { return book.Pending(r.challenge).has_value(); })) {
            waiting_logged = tick;
            Log(p, !available ? "target waiting (another co-op call in flight)" : "target waiting (event running or not callable)");
        }
        return false;
    }
    for (const auto& r : records) {
        const auto t = book.Pending(r.challenge);
        if (!t) continue;
        // Resolve everything anew at the issue point, never call with a cached record pointer.
        const auto m = ReadManager(p, *keys, base);
        if (!m || !m->Same(*checked)) { Reset(); return false; }
        const auto live = ReadRecord(p, *keys, *m, r.challenge);
        const auto last = ReadManager(p, *keys, base);
        if (!live || !last || !last->Same(*m) || !p.ready()) continue;
        if (const auto why = ApplyBlock(*keys, *last, *live, *t); !why.empty()) {
            auto& logged = blocked[r.challenge];
            if (logged != why) {
                logged = std::string{why};
                Log(p, fmt::format("target waiting challenge={:08x} step={:08x} index={} at={} ({})", t->challenge,
                                  t->step, t->index, live->index, why));
            }
            continue;
        }
        blocked.erase(r.challenge);
        if (p.call(keys->setter, {m->pointer, t->challenge, t->step, keys->arguments[0], keys->arguments[1], 0, 0, 0})) {
            book.Issued(*t); flight = Flight{*t, live->index, tick, false}; return true;
        }
    }
    return false;
}
void Controller::Returned(Port& p, u64 result) {
    if (defer->Calling()) { defer->Returned(); return; }
    if (!flight || flight->returned) return;
    flight->returned = true; flight->frame = tick;
    Log(p, fmt::format("apply challenge={:08x} step={:08x} index={} from={} origin={} w0={}",
        flight->target.challenge, flight->target.step, flight->target.index, flight->from,
        flight->target.origin.empty() ? "snapshot" : flight->target.origin, result & 0xffffffff));
}
} // namespace Core::OotCoop::Quest
