// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_equip.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace Core::OotCoop::Equip {
namespace {
using Json = nlohmann::json;
constexpr std::array<unsigned, 9> Shifts{0, 3, 6, 8, 10, 12, 14, 18, 20};
constexpr std::array<u8, 9> Widths{7, 7, 3, 3, 3, 3, 15, 3, 1};
constexpr std::array<u8, 8> Unknown{7, 7, 3, 3, 3, 3, 15, 3};
std::array<u8, 9> Values(const Appearance& a) {
    return {a.sword, a.shield, a.sword_state, a.shield_state, a.tunic, a.boots, a.mask, a.age, a.guard};
}
Appearance From(const std::array<u8, 9>& a) {
    return {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]};
}
bool Allowed(size_t i, u8 v) {
    if (v > Widths[i]) return false;
    if (i < 2) return v <= 3 || v == 7;
    if (i == 6) return v <= 10 || v == 15;
    if (i == 7) return v != 2;
    return true;
}
u8 Id(const Appearance& a, Slot s) {
    switch (s) {
    case Slot::Sword: return a.sword;
    case Slot::Shield: return a.shield;
    case Slot::Tunic: return a.tunic;
    case Slot::Boots: return a.boots;
    case Slot::Head: return a.mask;
    default: return 0xff;
    }
}
bool Known(Slot s, u8 id) {
    return id != (s == Slot::Sword || s == Slot::Shield ? 7 : s == Slot::Head ? 15 : 3);
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
bool SafeName(std::string_view n) {
    return !n.empty() && n.size() <= 63 && std::all_of(n.begin(), n.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
std::optional<ActorName> Text(const Port& p, u64 at) {
    if (!at) return {};
    ActorName text;
    for (u64 i = 0; i < 64; ++i) {
        const auto b = p.r8(at + i);
        if (!b) return {};
        if (!*b) return SafeName(text) ? std::optional{text} : std::nullopt;
        text.bytes[i] = static_cast<char>(*b);
        ++text.length;
    }
    return {};
}
constexpr std::array<std::string_view, 6> SlotNames{"sword", "shield", "tunic", "boots", "head", "sheath"};
std::optional<Slot> SlotByName(std::string_view name) {
    for (size_t i = 0; i < SlotNames.size(); ++i) if (name == SlotNames[i]) return static_cast<Slot>(i);
    return {};
}
// Private profile field names only. Values, including native slot domains, are never compiled in.
constexpr std::string_view Fields[] = {
    "holder", "equipment", "model", "user", "owner", "param", "pouch", "sheath_user", "actor_name",
    "link_record", "link_id", "record_actor", "record_id", "actor_record", "actor_id", "lifecycle",
    "lifecycle_lo", "lifecycle_hi", "request_bits", "bookkeeping", "bookkeeping_stride", "error_flags",
    "error_mask", "manager_cell", "ring_indices", "ring_capacity", "ring_start", "ring_count",
    "pair_count", "pairs", "pair_stride", "pair_child", "guard", "guard_mask",
    "user_cell", "user_equipment"};
}

std::optional<u32> Pack(const Appearance& value) {
    const auto v = Values(value);
    u32 word = 1U << 28;
    for (size_t i = 0; i < v.size(); ++i) {
        if (!Allowed(i, v[i])) return {};
        word |= u32{v[i]} << Shifts[i];
    }
    return word;
}
std::optional<Appearance> Unpack(u64 word, std::optional<bool> child) {
    if (word > 0xffffffffULL || word >> 28 != 1 || (word & 0x0fe00000)) return {};
    std::array<u8, 9> v{};
    for (size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<u8>((word >> Shifts[i]) & Widths[i]);
        if (!Allowed(i, v[i])) return {};
    }
    if (child && v[7] != 3 && v[7] != (*child ? 0 : 1)) return {};
    return From(v);
}
bool Settled(u8 id, u8 state) { return id == 0 ? state == 0 : id <= 3 && (state == 1 || state == 2); }
bool Newer(u16 value, u16 previous) { const auto d = static_cast<u16>(value - previous); return d && d < 0x8000; }
u16 Sequence::Sample(const Appearance& value) {
    auto old = Values(known);
    const auto next = Values(value);
    bool changed = false;
    for (size_t i = 0; i < Unknown.size(); ++i) {
        if (next[i] == Unknown[i]) continue;
        if ((i == 0 || i == 2) && !Settled(value.sword, value.sword_state)) continue;
        if ((i == 1 || i == 3) && !Settled(value.shield, value.shield_state)) continue;
        if (old[i] != next[i]) { old[i] = next[i]; changed = true; }
    }
    known = From(old);
    if (changed) ++sequence;
    return sequence;
}
std::optional<Packet> DecodePacket(const Json& value, bool child) {
    try {
        const auto& w = value.at("eq32"); const auto& s = value.at("eqseq");
        if (!w.is_number_integer() || !s.is_number_integer()) return {};
        const auto word = Number(w, 0xffffffff), seq = Number(s, 0xffff);
        if (!word || !seq || !Unpack(*word, child)) return {};
        return Packet{static_cast<u32>(*word), static_cast<u16>(*seq)};
    } catch (const Json::exception&) { return {}; }
}
std::shared_ptr<const Keys> DecodeKeys(const Json& j) {
    try {
        if (!j.is_object() || j.size() != 6) return {};
        auto k = std::make_shared<Keys>();
        for (auto [name, target] : {std::pair{"enabled", &k->enabled}, {"recreate_none", &k->recreate_none}}) {
            if (!j.at(name).is_boolean()) return {};
            *target = j.at(name).get<bool>();
        }
        const auto& fields = j.at("fields");
        if (!fields.is_object() || fields.size() != std::size(Fields)) return {};
        for (auto name : Fields) {
            const auto n = Number(fields.at(std::string{name}), 0x10000000);
            if (!n) return {};
            k->fields.emplace(name, *n);
        }
        if (!k->Get("holder") || !k->Get("equipment") || !k->Get("manager_cell") ||
            !k->Get("pair_stride") || k->Get("pair_stride") > 256 ||
            k->Get("lifecycle_hi") < k->Get("lifecycle_lo")) return {};
        const auto& slots = j.at("slots");
        if (!slots.is_object() || slots.size() != SlotCount) return {};
        for (size_t i = 0; i < SlotCount; ++i) {
            const auto& s = slots.at(std::string{SlotNames[i]});
            if (!s.is_object() || s.size() != 5) return {};
            auto& out = k->slots[i];
            const auto l = Number(s.at("link"), 0x10000), h = Number(s.at("handle"), 0x10000);
            const auto c = Number(s.at("create"), 31), d = Number(s.at("dynamic"), 31);
            const auto m = Number(s.at("request_mask"), 0xffffffff);
            if (!l || !h || !c || !d || !m || !*m) return {};
            out = {*l, *h, static_cast<u32>(*c), static_cast<u32>(*d), static_cast<u32>(*m)};
        }
        const auto& entries = j.at("entries");
        if (!entries.is_object() || entries.size() != 5) return {};
        for (auto [name, entry] : {std::pair{"dynamic", &k->dynamic}, {"static", &k->statik},
             {"draw", &k->draw}, {"sheathe", &k->sheathe}, {"state", &k->state}}) {
            const auto& e = entries.at(name);
            const auto at = Number(e.at("at"), 0x10000000);
            const auto& words = e.at("words");
            if (e.size() != 2 || !at || !*at || (*at & 3) || !words.is_array() || words.empty() || words.size() > 16) return {};
            entry->offset = *at;
            for (const auto& w : words) { const auto n = Number(w, 0xffffffff); if (!n) return {}; entry->words.push_back(static_cast<u32>(*n)); }
        }
        const auto& arguments = j.at("arguments");
        if (!arguments.is_object() || arguments.size() != 2) return {};
        for (auto [name, target] : {std::pair{"dynamic", &k->dynamic_arguments}, {"static", &k->static_arguments}}) {
            const auto& a = arguments.at(name);
            if (!a.is_array() || a.size() != target->size()) return {};
            for (size_t i = 0; i < a.size(); ++i) {
                const auto n = Number(a[i], 0xffffffff); if (!n) return {};
                (*target)[i] = *n;
            }
        }
        return k;
    } catch (const Json::exception&) { return {}; }
}
const Name* Names::Find(Slot s, u8 id, u8 age) const {
    for (const auto& n : rows) if (n.slot == s && n.id == id && (n.age == age || n.age == 3)) return &n;
    return nullptr;
}
u8 Names::Identify(Slot s, std::string_view name, u8 age) const {
    for (const auto& n : rows) if (n.slot == s && n.source == name && (n.age == age || n.age == 3)) return n.id;
    return s == Slot::Sword || s == Slot::Shield ? 7 : s == Slot::Head ? 15 : 3;
}
std::optional<Names> DecodeNames(const Json& j) {
    try {
        Names n;
        if (j.at("schema") != 1 || !j.at("rows").is_array() || j.at("rows").size() > 64) return {};
        const auto& players = j.at("players");
        if (!players.is_array() || players.empty() || players.size() > 2) return {};
        for (const auto& name : players) { n.players.push_back(name.get<std::string>()); if (!SafeName(n.players.back())) return {}; }
        std::set<std::tuple<Slot, u8, u8>> seen;
        for (const auto& row : j.at("rows")) {
            const auto s = SlotByName(row.at("slot").get<std::string>());
            const auto id = Number(row.at("id"), 15), age = Number(row.at("age"), 3);
            if (!s || !id || !age || *age == 2 || !Known(*s, static_cast<u8>(*id)) ||
                *id > (*s == Slot::Head ? 10 : (*s == Slot::Tunic || *s == Slot::Boots ? 2 : 3))) return {};
            Name r{*s, static_cast<u8>(*id), static_cast<u8>(*age), row.at("source").get<std::string>(),
                   row.at("target").get<std::string>(), row.value("sheath", std::string{})};
            if (!SafeName(r.source) || !SafeName(r.target) || (!r.sheath.empty() && !SafeName(r.sheath)) ||
                !seen.emplace(r.slot, r.id, r.age).second) return {};
            n.rows.push_back(std::move(r));
        }
        return n;
    } catch (const Json::exception&) { return {}; }
}
std::optional<Names> LoadNames(std::string_view build) {
    const char* profile = std::getenv("EDEN_NX2_OOT_COOP_KEYS");
    if (!profile || !*profile) return {};
    std::ifstream file{std::filesystem::path{profile}.parent_path() / "coop-equip.json"};
    if (!file) return {};
    auto j = Json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.contains("build_id") || j["build_id"] != build) return {};
    return DecodeNames(j);
}

std::optional<Child> ReadLink(const Port& p, const Keys& k, u64 link, const Child* cached) {
    if (!link) return {};
    const auto id = p.r32(link + k.Get("link_id"));
    const auto r = p.r64(link + k.Get("link_record"));
    if (!id || *id == 0xffffffff || !r || !*r || p.r32(*r + k.Get("record_id")) != id) return {};
    const auto a = p.r64(*r + k.Get("record_actor"));
    if (!a || !*a) return {};
    const auto life = p.r32(*a + k.Get("lifecycle"));
    const auto np = p.r64(*a + k.Get("actor_name"));
    if (!life || (*life >= k.Get("lifecycle_lo") && *life <= k.Get("lifecycle_hi")) || !np) return {};
    const auto name = cached && cached->record == *r && cached->actor == *a &&
        cached->generation == *id && cached->name_pointer == *np ? std::optional{cached->name} : Text(p, *np);
    if (!name || p.r32(link + k.Get("link_id")) != id || p.r64(link + k.Get("link_record")) != r ||
        p.r32(*r + k.Get("record_id")) != id || p.r64(*r + k.Get("record_actor")) != a ||
        p.r32(*a + k.Get("lifecycle")) != life || p.r64(*a + k.Get("actor_name")) != np) return {};
    return Child{link, *r, *a, *np, *id, *name};
}
bool Related(const Port& p, const Keys& k, u64 base, u64 owner, u64 child) {
    const auto m = p.r64(base + k.Get("manager_cell"));
    const auto indices = p.r64(owner + k.Get("ring_indices"));
    const auto capacity = p.r32(owner + k.Get("ring_capacity"));
    const auto start = p.r32(owner + k.Get("ring_start"));
    const auto count = p.r32(owner + k.Get("ring_count"));
    if (!m || !*m || !indices || !*indices || !capacity || !*capacity || *capacity > 256 ||
        !start || *start >= *capacity || !count || *count > *capacity) return false;
    const auto pairs = p.r64(*m + k.Get("pairs")); const auto total = p.r32(*m + k.Get("pair_count"));
    if (!pairs || !*pairs || !total || *total > 1000000) return false;
    bool found = false;
    std::array<std::array<u64, 4>, 256> receipts{};
    for (u32 i = 0; i < *count; ++i) {
        const auto index = p.r32(*indices + 4 * u64{(*start + i) % *capacity});
        if (!index || *index >= *total) return false;
        const u64 at = *pairs + u64{*index} * k.Get("pair_stride");
        const auto first = p.r64(at), second = p.r64(at + k.Get("pair_child"));
        if (!first || !second) return false;
        receipts[i] = {*indices + 4 * u64{(*start + i) % *capacity}, *index, *first, *second};
        if (*first == owner && *second == child) found = true;
    }
    for (size_t i = 0; i < *count; ++i) {
        const auto& r = receipts[i];
        const u64 at = *pairs + r[1] * k.Get("pair_stride");
        if (p.r32(r[0]) != r[1] || p.r64(at) != r[2] || p.r64(at + k.Get("pair_child")) != r[3]) return false;
    }
    return found && p.r64(base + k.Get("manager_cell")) == m && p.r64(owner + k.Get("ring_indices")) == indices &&
           p.r32(owner + k.Get("ring_capacity")) == capacity && p.r32(owner + k.Get("ring_start")) == start &&
           p.r32(owner + k.Get("ring_count")) == count && p.r64(*m + k.Get("pairs")) == pairs &&
           p.r32(*m + k.Get("pair_count")) == total;
}

bool Reconciler::Desired(Packet packet, u8 age) {
    const auto a = Unpack(packet.word, age == 0);
    // Poses arrive in order on one TCP stream and every one carries the full snapshot, so the
    // newest packet is the truth; eqseq is not compared here (a peer that restarts its client
    // restarts its sequence, and a stale-rejection would freeze its ghost's equipment).
    if (!a || a->age == 3) return false;
    auto values = Values(desired); const auto incoming = Values(*a);
    for (size_t i = 0; i < Unknown.size(); ++i) {
        if (incoming[i] == Unknown[i]) continue;
        if ((i == 0 || i == 2) && !Settled(a->sword, a->sword_state)) continue;
        if ((i == 1 || i == 3) && !Settled(a->shield, a->shield_state)) continue;
        values[i] = incoming[i];
    }
    values[8] = a->guard;
    desired = From(values); sequence = packet.sequence;
    return true;
}
bool Reconciler::WantsNone() const { return desired.sword == 0 || desired.shield == 0; }
Work Reconciler::Step(const std::array<Observation, SlotCount>& o, const Names& names, u8 age,
                     bool eligible, bool recreate_none) {
    if (quarantined) return {Action::Quarantine};
    if (!eligible || !sequence) return {};
    for (size_t i = 0; i < 5; ++i) {
        const Slot s = static_cast<Slot>(i); auto& r = requests[i]; const auto& v = o[i];
        if (r.pending) {
            if (++r.updates >= 300) { quarantined = true; return {Action::Quarantine, s}; }
            const Name* n = names.Find(s, r.id, age);
            bool correct = n && v.child && v.child->name == n->target;
            if (r.action == Action::Create) correct = correct && v.child->generation != r.old_generation;
            if (s == Slot::Tunic && r.id == 0) correct = v.readable && !v.child;
            if (s == Slot::Sword && n) correct = correct && o[5].idle && o[5].child && o[5].child->name == n->sheath;
            if (!correct || !v.idle) return {};
            if (i < 2 && v.actual == 3) return {Action::Query, s};
            if (i < 2 && v.actual != (r.action == Action::Draw ? 1 : 2)) return {Action::Query, s};
            r.pending = false;
        }
        const u8 id = Id(desired, s);
        if (!Known(s, id)) continue;
        // Boots are deferred: the native lower-body request writes global game data (research
        // section 4) and no guard is installed. Child clothing and adult masks are unsupported
        // combinations, rather than guessed conversions.
        if (s == Slot::Boots || (age == 0 && s == Slot::Tunic) || (age == 1 && s == Slot::Head && id != 0)) continue;
        if (i < 2 && id == 0) {
            if (v.child && recreate_none) return {Action::Recreate, s};
            continue;
        }
        const Name* name = names.Find(s, id, age);
        if (!name) continue;
        if (!v.readable || !v.idle) {
            if (++r.updates >= 300) { quarantined = true; return {Action::Quarantine, s}; }
            return {};
        }
        const bool base_tunic = s == Slot::Tunic && id == 0 && !v.child;
        if (!base_tunic && (!v.child || v.child->name != name->target)) return {Action::Create, s, id};
        if (s == Slot::Sword && (!o[5].child || o[5].child->name != name->sheath || !o[5].idle)) {
            if (++r.updates >= 300) { quarantined = true; return {Action::Quarantine, s}; }
            return {};
        }
        if (i >= 2) r.updates = 0;
    }
    for (size_t i = 0; i < 2; ++i) {
        const auto s = static_cast<Slot>(i); const auto id = Id(desired, s);
        const auto state = i == 0 ? desired.sword_state : desired.shield_state;
        if (!id || !Settled(id, state) || !o[i].child || !o[i].idle) continue;
        if (o[i].actual == 3) {
            if (++requests[i].updates >= 300) { quarantined = true; return {Action::Quarantine, s}; }
            return {Action::Query, s};
        }
        requests[i].updates = 0;
        if (o[i].actual != state) return {state == 1 ? Action::Draw : Action::Sheathe, s, id};
    }
    return {};
}
void Reconciler::Issued(Work w, const Observation& old) {
    if (w.action == Action::Query || w.action == Action::None) return;
    requests[static_cast<size_t>(w.slot)] = {true, w.id, old.child ? old.child->generation : 0xffffffffU, 0, w.action};
}
std::array<u64, 8> Arguments(const Keys& k, Work w, u64 e, u64 cell) {
    const auto& s = k.slots[static_cast<size_t>(w.slot)];
    if (w.action != Action::Create) return {e, s.dynamic};
    std::array<u64, 8> args{e, s.create, cell};
    const auto& tail = w.slot == Slot::Sword || w.slot == Slot::Shield ? k.dynamic_arguments : k.static_arguments;
    std::copy(tail.begin(), tail.end(), args.begin() + 3);
    return args;
}


std::optional<u64> NameTable::Cell(Port& p, std::string_view name) {
    for (const auto& [text, cell] : cells) if (text == name) return cell;
    if (!SafeName(name)) return {};
    const u64 bytes = (8 + name.size() + 1 + 7) & ~u64{7};
    if (used + bytes > size) {
        if (!std::exchange(full_logged, true)) p.log("OoT co-op: equipment name table full; request refused");
        return {};
    }
    const u64 cell = at + used;
    std::array<u8, 8 + 64> entry{};
    const u64 text = cell + 8;
    std::memcpy(entry.data(), &text, 8);
    std::memcpy(entry.data() + 8, name.data(), name.size());
    if (!p.write(cell, entry.data(), 8 + name.size() + 1)) return {};
    used += bytes;
    cells.emplace_back(std::string{name}, cell);
    return cell;
}
std::optional<u64> NameTable::Park(Port& p) {
    if (park) return park;
    // Identity rotation, translated far below any scene (the ghost driver's own park height).
    const std::array<float, 12> matrix{1, 0, 0, 0, 0, 1, 0, -10000, 0, 0, 1, 0};
    if (used + sizeof(matrix) > size || !p.write(at + used, matrix.data(), sizeof(matrix))) return {};
    park = at + used;
    used += sizeof(matrix);
    return park;
}

Controller::Controller(std::shared_ptr<const Keys> keys_, Names names_, OotGameDataTrace::GhostOffsets ghost_,
                       u64 base_, u64 table_, u64 table_bytes)
    : keys{std::move(keys_)}, names{std::move(names_)}, ghost{std::move(ghost_)}, table{table_, table_bytes},
      base{base_} {
    trees.reserve(MaxTrees);
}
bool Controller::Calling() const { return pending.call != Call::None; }
Controller::Tree* Controller::find(u64 id) {
    for (auto& t : trees) if (t.id == id) return &t;
    return nullptr;
}
std::optional<u64> Controller::equipment(const Port& p, u64 actor, bool private_owner) const {
    const auto& k = *keys;
    const auto h = p.r64(actor + k.Get("holder"));
    const auto e = h && *h ? p.r64(*h + k.Get("equipment")) : std::nullopt;
    if (!e || !*e || p.r64(*e + k.Get("owner")) != actor) return {};
    if (private_owner) {
        const auto param = p.r64(*e + k.Get("param"));
        if (!param || !*param || p.r8(*param + k.Get("pouch")) != 0 || p.r8(*param + k.Get("sheath_user")) != 1) return {};
    }
    return e;
}
bool Controller::valid(const Port& p, const Token& t) const {
    const auto& k = *keys;
    if (!t.actor || !t.record || !t.equipment || t.age > 1 ||
        p.r64(t.actor + k.Get("actor_record")) != t.record ||
        p.r32(t.actor + k.Get("actor_id")) != t.generation ||
        p.r64(t.record + k.Get("record_actor")) != t.actor ||
        p.r32(t.record + k.Get("record_id")) != t.generation) return false;
    const auto life = p.r32(t.actor + k.Get("lifecycle"));
    const auto np = p.r64(t.actor + k.Get("actor_name"));
    const auto name = np ? Text(p, *np) : std::nullopt;
    if (!life || (*life >= k.Get("lifecycle_lo") && *life <= k.Get("lifecycle_hi")) || !name ||
        *name != (t.age == 0 ? ghost.actor_child : ghost.actor_adult)) return false;
    return equipment(p, t.actor, true) == t.equipment &&
        p.r32(t.record + k.Get("record_id")) == t.generation &&
        p.r64(t.record + k.Get("record_actor")) == t.actor &&
        p.r32(t.actor + k.Get("lifecycle")) == life;
}
Observation Controller::observe(const Port& p, const Token& t, Slot slot, const Child* cached) const {
    const auto& k = *keys; const auto& s = k.slots[static_cast<size_t>(slot)];
    Observation o;
    const u64 at = t.equipment + k.Get("bookkeeping") + s.create * k.Get("bookkeeping_stride");
    const auto phase = p.r32(at), detail = p.r32(at + sizeof(u32));
    const auto bits = p.r32(t.equipment + k.Get("request_bits")), errors = p.r32(t.equipment + k.Get("error_flags"));
    const auto handle = p.r64(t.equipment + s.handle);
    const auto id = p.r32(t.equipment + s.link + k.Get("link_id"));
    o.child = ReadLink(p, k, t.equipment + s.link, cached);
    if (o.child && (!Related(p, k, base, t.actor, o.child->actor) ||
        ReadLink(p, k, o.child->link, &*o.child) != o.child)) o.child.reset();
    o.readable = phase && detail && bits && errors && handle && id && (o.child || *id == 0xffffffff);
    o.phase = phase.value_or(0); o.detail = detail.value_or(0); o.bits = bits.value_or(0);
    o.errors = errors.value_or(0); o.handle = handle.value_or(0);
    // An empty component-owned handle and cleared request bit are a conservative idle receipt.
    // The two phase words remain telemetry: no undocumented phase is labelled ready.
    o.idle = o.readable && !o.handle && !(o.bits & s.request_mask) && !(o.errors & k.Get("error_mask"));
    return o;
}
bool Controller::start(Port& p, const OotGameDataTrace::Entry& entry, std::array<u64, 8> args,
                       Pending call, std::optional<float> alpha) {
    const auto address = p.verified(entry);
    if (Calling() || !address || !p.call(*address, args, alpha)) return false;
    pending = std::move(call);
    return true;
}
void Controller::dump(Port& p, const Tree& tree, std::string_view reason) const {
    const auto& t = tree.token;
    p.log(fmt::format("OoT co-op: equipment {} peer={} scene={} epoch={} actor={:#x} generation={} E={:#x} age={} desired={:#x}",
        reason, t.peer.substr(0, 8), t.scene, t.epoch, t.actor, t.generation, t.equipment, t.age,
        Pack(tree.reconcile.Desired()).value_or(0)));
    for (size_t i = 0; i < SlotCount; ++i) {
        const auto& o = tree.observed[i];
        p.log(fmt::format("OoT co-op: equipment slot={} handle={:#x} bookkeeping={:#x}/{:#x} bits={:#x} errors={:#x} child={:#x} id={:#x} name={} actual={} readable={} idle={}",
            SlotNames[i], o.handle, o.phase, o.detail, o.bits, o.errors, o.child ? o.child->actor : 0,
            o.child ? o.child->generation : 0xffffffff, o.child ? std::string_view{o.child->name} : std::string_view{"-"}, o.actual, o.readable, o.idle));
    }
}
void Controller::retire(Port& p, Tree& tree, bool recreate, std::string_view reason) {
    if (tree.retired) return;
    tree.retired = true;
    tree.recreate = recreate;
    tree.retired_tick = tick;
    dump(p, tree, reason);
}
void Controller::remember(Tree& tree, const Child& child) {
    if (std::any_of(tree.children.begin(), tree.children.end(), [&](const Child& c) {
            return c.actor == child.actor && c.generation == child.generation; }))
        return;
    if (tree.children.size() >= MaxChildren) { tree.reconcile.Quarantine(); return; }
    tree.children.push_back(child);
}
bool Controller::Blocked(u64 actor, u32 generation) const {
    return std::any_of(trees.begin(), trees.end(), [&](const Tree& t) {
        return t.token.actor == actor && t.token.generation == generation && t.retired &&
               (t.recreate || t.reconcile.Quarantined());
    });
}
bool Controller::Recreate(u64 actor, u32 generation) const {
    return std::any_of(trees.begin(), trees.end(), [&](const Tree& t) {
        return t.token.actor == actor && t.token.generation == generation && t.retired && t.recreate;
    });
}
void Controller::Invalidate() {
    ++epoch;
    for (auto& t : trees) if (!t.retired) { t.retired = true; t.retired_tick = tick; }
    player.reset(); local_packet.reset(); local_stage = 0; sequence.Reset(); absent_samples = {};
}
std::optional<Packet> Controller::Local(bool child) const {
    if (!local_packet || tick - local_tick > 4 * SampleEvery || !Unpack(local_packet->word, child)) return {};
    return local_packet;
}

bool Controller::Frame(Port& p, const std::vector<Root>& roots, std::string_view scene_, bool eligible, bool send,
                       std::optional<bool> child) {
    if (Calling() || !keys->enabled) return false;
    ++tick;
    local_child = child;
    if (scene != scene_) { Invalidate(); scene = scene_; }
    if (!eligible || !send) { player.reset(); local_packet.reset(); local_stage = 0; }
    const auto root_of = [&](const Tree& tree) -> const Root* {
        for (const auto& root : roots) {
            if (root.token.actor == tree.token.actor && root.token.record == tree.token.record &&
                root.token.generation == tree.token.generation && root.token.peer == tree.token.peer &&
                root.token.scene == tree.token.scene && root.token.age == tree.token.age) return &root;
        }
        return nullptr;
    };
    // An owner whose root left the ghost driver (dropped, abandoned, re-aged, reassigned) retires
    // with its whole tree. A root that is only ineligible for now (peer loading, elsewhere, stale)
    // keeps its tree suspended: no requests, no timeout progress, nothing parked or recreated.
    for (auto& tree : trees) if (!tree.retired && !root_of(tree)) retire(p, tree, false, "retired (owner left the ghost driver)");
    // A retired tree is forgotten once its owner is gone (the game deleted it with its children)
    // or its children had the park window; a quarantined owner still listed stays, so it stays blocked.
    std::erase_if(trees, [&](const Tree& t) {
        if (!t.retired || (Calling() && pending.tree == t.id)) return false;
        if (!valid(p, t.token)) return true;
        return tick - t.retired_tick > RetireWindow && !root_of(t);
    });
    for (const auto& root : roots) {
        if (!root.eligible || !root.desired) continue;
        auto it = std::find_if(trees.begin(), trees.end(), [&](const Tree& t) {
            return t.token.actor == root.token.actor && t.token.generation == root.token.generation;
        });
        if (it == trees.end()) {
            if (trees.size() >= MaxTrees) {
                if (!std::exchange(budget_logged, true)) p.log("OoT co-op: equipment owner budget full; ghost shown without equipment");
                continue;
            }
            auto token = root.token;
            token.epoch = epoch;
            token.equipment = equipment(p, token.actor, true).value_or(0);
            if (!valid(p, token)) continue;
            Tree tree; tree.id = next_tree++; tree.token = token;
            trees.push_back(std::move(tree)); it = std::prev(trees.end());
            dump(p, *it, "owner ready");
        }
        if (!it->retired) it->reconcile.Desired(*root.desired, root.token.age);
    }
    std::vector<std::pair<u64, Work>> work;
    for (auto& t : trees) {
        const Root* root = root_of(t);
        if (!eligible || t.retired || !root || !root->eligible || !valid(p, t.token)) continue;
        for (size_t i = 0; i < SlotCount; ++i) {
            auto fresh = observe(p, t.token, static_cast<Slot>(i), t.observed[i].child ? &*t.observed[i].child : nullptr);
            if (fresh.child && t.queried[i] == fresh.child && tick - t.query_ticks[i] < 30) fresh.actual = t.observed[i].actual;
            if (fresh.child) remember(t, *fresh.child);
            t.observed[i] = std::move(fresh);
        }
        if (t.reconcile.WantsNone() && !keys->recreate_none && !std::exchange(t.none_logged, true))
            dump(p, t, "none deferred (kept as is; removal is a separate probe)");
        if (t.reconcile.Desired().boots != 0 && t.reconcile.Desired().boots != 3 && !std::exchange(t.boots_logged, true))
            dump(p, t, "boots deferred (no guarded lower-body request)");
        const auto w = t.reconcile.Step(t.observed, names, t.token.age, true, keys->recreate_none);
        if (w.action == Action::Quarantine || w.action == Action::Recreate) {
            retire(p, t, w.action == Action::Recreate, w.action == Action::Recreate ? "none retire/recreate" : "timeout/quarantined");
            continue;
        }
        if (w.action != Action::None) work.emplace_back(t.id, w);
    }
    if (!eligible) return false;
    // Parking a retired owner's children comes first: they must not linger where the owner was.
    if (tick % 2 == 1 && park(p)) return true;
    if (send && tick % 2 == 0 && sender(p)) return true;
    for (size_t n = 0; n < work.size(); ++n) {
        const auto& [id, w] = work[(active_cursor + n) % work.size()];
        Tree* t = find(id);
        if (!t || t->retired || !valid(p, t->token)) continue;
        const size_t si = static_cast<size_t>(w.slot);
        const auto& o = t->observed[si];
        u64 cell = 0;
        if (w.action == Action::Create) {
            const auto* name = names.Find(w.slot, w.id, t->token.age);
            const auto at = name ? table.Cell(p, name->target) : std::nullopt;
            if (!at) { t->reconcile.Quarantine(); retire(p, *t, false, "name cell refused; quarantined"); continue; }
            cell = *at;
        }
        const auto& entry = w.action == Action::Create ? (si < 2 ? keys->dynamic : keys->statik) :
                            w.action == Action::Query ? keys->state : w.action == Action::Draw ? keys->draw : keys->sheathe;
        if (start(p, entry, Arguments(*keys, w, t->token.equipment, cell), {Call::Work, id, si, t->token, w, o.child})) {
            active_cursor = active_cursor + n + 1;
            t->reconcile.Issued(w, o);
            if (w.action != Action::Query) {
                t->queried[si].reset(); t->observed[si].actual = 3;
                if (w.action == Action::Create && w.slot == Slot::Sword) {
                    t->queried[1].reset(); t->observed[1].actual = 3;
                }
                dump(p, *t, w.action == Action::Create ? "request issued" : "state setter issued");
            }
            return true;
        }
        t->reconcile.Quarantine(); retire(p, *t, false, "entry/call refused; quarantined");
    }
    return false;
}

bool Controller::park(Port& p) {
    for (size_t n = 0; n < trees.size(); ++n) {
        auto& t = trees[(retire_cursor + n) % trees.size()];
        if (!t.retired || tick - t.retired_tick > RetireWindow || !valid(p, t.token)) continue;
        // A late native completion on the retired owner joins the list and is parked too.
        for (size_t i = 0; i < SlotCount; ++i)
            if (const auto o = observe(p, t.token, static_cast<Slot>(i)); o.child) remember(t, *o.child);
        while (t.parked < t.children.size()) {
            const auto child = t.children[t.parked++];
            const auto& k = *keys;
            const auto life = p.r32(child.actor + k.Get("lifecycle"));
            if (p.r32(child.record + k.Get("record_id")) != child.generation ||
                p.r64(child.record + k.Get("record_actor")) != child.actor || !life ||
                (*life >= k.Get("lifecycle_lo") && *life <= k.Get("lifecycle_hi")) ||
                !Related(p, k, base, t.token.actor, child.actor)) continue;
            const auto matrix = table.Park(p);
            if (!matrix) return false;
            retire_cursor = (retire_cursor + n + 1) % trees.size();
            p.log(fmt::format("OoT co-op: equipment parking child {:#x} id={:#x} name={} of retired owner {:#x}",
                child.actor, child.generation, std::string_view{child.name}, t.token.actor));
            if (start(p, ghost.set_mtx, {child.actor, *matrix, 0}, {Call::Park, t.id, 0, t.token, {}, child})) return true;
        }
    }
    return false;
}

// The resident player is found by pure reads: the pouch manager's registered EquipmentUser and its
// owner backlink, validated like any actor (record backlink, generation, lifecycle, holder -> E).
std::optional<Child> Controller::resident(const Port& p) const {
    const auto& k = *keys;
    const auto q = p.r64(base + k.Get("user_cell"));
    const auto e = q && *q ? p.r64(*q + k.Get("user_equipment")) : std::nullopt;
    const auto actor = e && *e ? p.r64(*e + k.Get("owner")) : std::nullopt;
    if (!actor || !*actor || equipment(p, *actor, false) != e) return {};
    const auto record = p.r64(*actor + k.Get("actor_record"));
    const auto generation = p.r32(*actor + k.Get("actor_id"));
    const auto life = p.r32(*actor + k.Get("lifecycle"));
    const auto np = p.r64(*actor + k.Get("actor_name"));
    if (!record || !*record || !generation || p.r64(*record + k.Get("record_actor")) != actor ||
        p.r32(*record + k.Get("record_id")) != generation || !life ||
        (*life >= k.Get("lifecycle_lo") && *life <= k.Get("lifecycle_hi")) || !np) return {};
    const auto name = Text(p, *np);
    if (!name || std::none_of(names.players.begin(), names.players.end(), [&](const std::string& n) { return *name == n; }))
        return {};
    return Child{0, *record, *actor, *np, *generation, *name};
}
bool Controller::sender(Port& p) {
    const auto& k = *keys;
    if (local_stage && (!player || resident(p) != player || equipment(p, player->actor, false) != player_equipment)) {
        local_stage = 0; player.reset(); local_packet.reset(); absent_samples = {};
    }
    if (!local_stage) {
        if (tick - local_tick < SampleEvery) return false;
        local_tick = tick;
        const auto found = resident(p);
        const auto e = found ? equipment(p, found->actor, false) : std::nullopt;
        if (!found || !e) { player.reset(); local_packet.reset(); return false; }
        if (player != found || player_equipment != *e) {
            absent_samples = {}; local_children = {};
            p.log(fmt::format("OoT co-op: equipment local player actor={:#x} id={:#x} name={} E={:#x}",
                found->actor, found->generation, std::string_view{found->name}, *e));
        }
        player = found;
        player_equipment = *e;
        snapshot(p);
        return false;
    }
    if (local_stage <= 2) {
        const size_t si = local_stage - 1;
        if (!local_children[si]) { ++local_stage; return sender(p); }
        return start(p, k.state, {player_equipment, k.slots[si].dynamic},
                     {Call::LocalState, 0, si, {}, {}, local_children[si]});
    }
    bool consistent = player && resident(p) == player;
    for (size_t i = 0; i < SlotCount; ++i) {
        if (ReadLink(p, k, player_equipment + k.slots[i].link) != local_children[i]) consistent = false;
    }
    consistent = consistent && equipment(p, player->actor, false) == player_equipment;
    if (consistent) local_packet = Packet{*Pack(local), sequence.Sample(local)};
    else local_packet.reset();
    local_stage = 0;
    return false;
}
void Controller::snapshot(Port& p) {
    const auto& k = *keys;
    // The actor name does not encode age on this build; the age is the local pose's (GameData) one.
    local = {}; local.age = local_child ? (*local_child ? 0 : 1) : 3;
    Token token; token.actor = player->actor; token.equipment = player_equipment;
    std::array<u8*, 5> ids{&local.sword, &local.shield, &local.tunic, &local.boots, &local.mask};
    for (size_t i = 0; i < SlotCount; ++i) {
        auto o = observe(p, token, static_cast<Slot>(i), local_children[i] ? &*local_children[i] : nullptr);
        local_children[i] = o.child;
        if (i >= ids.size()) continue;
        if (o.child) {
            *ids[i] = names.Identify(static_cast<Slot>(i), o.child->name, local.age);
            absent_samples[i] = 0;
        } else if (o.idle && ++absent_samples[i] >= 3) {
            // A missing link is pending/unknown during async creation; it reads as none/base
            // only after three consecutive idle samples with no request in flight.
            *ids[i] = 0;
            if (i == 0) local.sword_state = 0;
            if (i == 1) local.shield_state = 0;
        } else if (!o.idle) {
            absent_samples[i] = 0;
        }
    }
    const auto h = p.r64(player->actor + k.Get("holder"));
    const auto u = h && *h ? p.r64(*h + k.Get("user")) : std::nullopt;
    const auto guard = u && *u ? p.r8(*u + k.Get("guard")) : std::nullopt;
    local.guard = guard && (*guard & k.Get("guard_mask")) ? 1 : 0;
    local_stage = 1;
}
void Controller::CheckContext(const std::vector<Root>& roots, std::string_view current_scene, bool eligible) {
    if (!Calling()) return;
    bool accept = eligible && current_scene == scene;
    if (pending.call == Call::Work) {
        accept = accept && std::any_of(roots.begin(), roots.end(), [&](const Root& r) {
            return r.eligible && r.token.peer == pending.token.peer && r.token.scene == pending.token.scene &&
                r.token.actor == pending.token.actor && r.token.record == pending.token.record &&
                r.token.generation == pending.token.generation && r.token.age == pending.token.age;
        });
    }
    pending.accept = pending.accept && accept;
}

void Controller::Returned(Port& p, u64 result) {
    const auto done = std::exchange(pending, {});
    const auto& k = *keys;
    if (done.call == Call::Park) return;
    if (!done.accept) {
        if (done.call == Call::LocalState) { local_packet.reset(); local_stage = 0; }
        return; // a work result is never accepted out of context; the next observation re-reads E
    }
    if (done.call == Call::LocalState) {
        if (!player || resident(p) != player ||
            equipment(p, player->actor, false) != player_equipment || !done.child ||
            ReadLink(p, k, done.child->link) != done.child ||
            !Related(p, k, base, player->actor, done.child->actor)) {
            local_stage = 0; local_packet.reset(); return;
        }
        (done.slot == 0 ? local.sword_state : local.shield_state) = result <= 2 ? static_cast<u8>(result) : 3;
        ++local_stage;
        return;
    }
    Tree* tree = find(done.tree);
    if (!tree || tree->token != done.token || tree->retired || done.call != Call::Work || !valid(p, done.token)) return;
    if (done.work.action == Action::Query) {
        const auto child = ReadLink(p, k, tree->token.equipment + k.slots[done.slot].link);
        if (child != done.child || !child || !Related(p, k, base, tree->token.actor, child->actor) ||
            ReadLink(p, k, child->link, &*child) != child) return;
        const u8 actual = result <= 2 ? static_cast<u8>(result) : 3;
        if (tree->queried[done.slot] != child || tree->observed[done.slot].actual != actual)
            dump(p, *tree, fmt::format("actual state receipt slot={} state={}", SlotNames[done.slot], actual));
        tree->observed[done.slot].actual = actual;
        tree->queried[done.slot] = child;
        tree->query_ticks[done.slot] = tick;
    } else if (done.work.action == Action::Create && !(result & 1)) {
        tree->reconcile.Quarantine(); retire(p, *tree, false, "request refused; quarantined");
    }
}
} // namespace Core::OotCoop::Equip
