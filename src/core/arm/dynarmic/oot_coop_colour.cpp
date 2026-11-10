// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_colour.h"
#include "core/arm/dynarmic/oot_coop_ghost.h"
#include <charconv>
#include <cstring>
#include <limits>
#include <set>

namespace Core::OotCoop::Colour {
namespace {
using Json = nlohmann::json;
std::optional<u64> Number(const Json& j, u64 max, bool hex = true) {
    if (j.is_number_unsigned()) { const auto n = j.get<u64>(); return n <= max ? std::optional{n} : std::nullopt; }
    if (j.is_number_integer()) { const auto n = j.get<s64>(); return n >= 0 && u64(n) <= max ? std::optional{u64(n)} : std::nullopt; }
    if (!hex || !j.is_string()) return {};
    const auto& s = j.get_ref<const std::string&>();
    if (s.size() < 3 || s.size() > 18 || !(s.starts_with("0x") || s.starts_with("0X"))) return {};
    u64 n{};
    const auto [end, ec] = std::from_chars(s.data() + 2, s.data() + s.size(), n, 16);
    return ec == std::errc{} && end == s.data() + s.size() && n <= max ? std::optional{n} : std::nullopt;
}
bool Fields(const Json& j, std::initializer_list<std::string_view> fields) {
    if (!j.is_object()) return false;
    for (const auto& [key, value] : j.items())
        if (std::find(fields.begin(), fields.end(), key) == fields.end()) return false;
    return true;
}
template <size_t N>
std::optional<std::array<u64, N>> Offsets(const Json& j) {
    if (!j.is_array() || j.size() != N) return {};
    std::array<u64, N> result{};
    for (size_t i = 0; i < N; ++i) {
        const auto n = Number(j[i], 0x10000);
        if (!n) return {};
        result[i] = *n;
    }
    return result;
}
// Reject overflow and null base before asking the checked guest-memory port to read.
std::optional<u64> Address(u64 base, u64 offset) {
    if (!base || base > std::numeric_limits<u64>::max() - offset - 8) return {};
    return base + offset;
}
std::optional<u64> R64(Port& p, u64 base, u64 offset) {
    const auto at = Address(base, offset);
    return at ? p.r64(*at) : std::nullopt;
}
std::optional<u32> R32(Port& p, u64 base, u64 offset) {
    const auto at = Address(base, offset);
    return at ? p.r32(*at) : std::nullopt;
}
std::optional<u16> R16(Port& p, u64 base, u64 offset) {
    const auto at = Address(base, offset);
    if (!at) return {};
    const auto lo = p.r8(*at), hi = p.r8(*at + 1);
    if (!lo || !hi) return {};
    return static_cast<u16>(*lo | (u16{*hi} << 8));
}
bool SameOwner(const View& a, const View& b) {
    return a.origin == b.origin && a.banc == b.banc && a.actor == b.actor &&
        a.record == b.record && a.vtable == b.vtable && a.generation == b.generation &&
        a.child == b.child && a.session == b.session && a.epoch == b.epoch && a.fields == b.fields;
}
}
std::optional<OotGameDataTrace::Entry> DecodeEntry(const Json& j) {
    try {
        if (!Fields(j, {"at", "words"}) || j.size() != 2) return {};
        const auto at = Number(j.at("at"), 0x10000000);
        const auto& words = j.at("words");
        if (!at || !*at || (*at & 3) || !words.is_array() || words.empty() || words.size() > 16) return {};
        OotGameDataTrace::Entry entry{*at, {}};
        for (const auto& word : words) {
            const auto n = Number(word, 0xffffffff);
            if (!n) return {};
            entry.words.push_back(static_cast<u32>(*n));
        }
        return entry;
    } catch (const Json::exception&) { return {}; }
}
std::shared_ptr<const Keys> DecodeKeys(const Json& j) {
    try {
        if (!Fields(j, {"enabled", "apply", "animation", "frames", "players", "hide_armour",
                        "instance", "getter", "bank", "record_stride", "record_name"}) ||
            !j.at("enabled").is_boolean()) return {};
        auto k = std::make_shared<Keys>();
        k->enabled = j.at("enabled").get<bool>();
        if (j.contains("hide_armour")) {
            if (!j.at("hide_armour").is_boolean()) return {};
            k->hide_armour = j.at("hide_armour").get<bool>();
        }
        // Disabled profiles may omit the call configuration, but supplied fields remain strict.
        if (k->enabled || j.contains("apply")) {
            const auto entry = DecodeEntry(j.at("apply"));
            if (!entry || entry->words.size() != 8) return {};
            k->apply = *entry;
        }
        if (k->enabled || j.contains("animation")) {
            if (!j.at("animation").is_string()) return {};
            k->animation = j.at("animation").get<std::string>();
            if (k->animation.empty() || k->animation.size() > 23 ||
                !std::all_of(k->animation.begin(), k->animation.end(), [](unsigned char c) { return c >= 32 && c <= 126; })) return {};
        }
        if (k->enabled || j.contains("frames")) {
            const auto& frames = j.at("frames");
            if (!frames.is_array() || frames.size() < 4 || frames.size() > 16) return {};
            std::set<u64> seen;
            for (const auto& value : frames) {
                const auto n = Number(value, 255, false);
                if (!n || !*n || !seen.insert(*n).second) return {};
                k->frames.push_back(static_cast<u8>(*n));
            }
        }
        if (j.contains("players")) {
            if (!j.at("players").is_object()) return {};
            for (const auto& [name, value] : j.at("players").items()) {
                // dump validates UTF-8 even for programmatically constructed JSON strings.
                if (name.empty() || name.size() > 32 || name.find('\0') != std::string::npos) return {};
                (void)Json(name).dump();
                const auto n = Number(value, 255, false);
                if (!n) return {};
                k->players.emplace(name, static_cast<u8>(*n));
            }
        }
        if (j.contains("instance") || j.contains("getter")) {
            k->instance = Offsets<4>(j.at("instance"));
            const auto getter = DecodeEntry(j.at("getter"));
            if (!k->instance || !getter || getter->words.size() != 8 || ((*k->instance)[2] & 7)) return {};
            k->getter = *getter;
        }
        if (j.contains("bank") || j.contains("record_stride") || j.contains("record_name")) {
            k->bank = Offsets<5>(j.at("bank"));
            const auto stride = Number(j.at("record_stride"), 0x10000);
            const auto name = Number(j.at("record_name"), 0x10000);
            if (!k->instance || !k->bank || !stride || *stride < 8 || !name || *name > *stride - 8) return {};
            k->record_stride = *stride;
            k->record_name = *name;
        }
        return k;
    } catch (const Json::exception&) { return {}; }
}
u8 DesiredFrame(const Keys& keys, size_t slot, std::string_view name) {
    if (!keys.enabled) return 0;
    const auto found = keys.players.find(name);
    return found != keys.players.end() ? found->second : slot < keys.frames.size() ? keys.frames[slot] : 0;
}
std::optional<Equip::Packet> MaskEquipment(std::optional<Equip::Packet> packet, bool child, bool colour_on) {
    if (!packet || child || !colour_on) return packet;
    auto appearance = Equip::Unpack(packet->word, child);
    if (!appearance) return packet;
    appearance->tunic = 0;
    if (const auto word = Equip::Pack(*appearance)) packet->word = *word;
    return packet;
}
// Creation ends at +0x140; the legacy request ends at +0x1B0; the modern command
// ends at +0x1C0. Tint starts at +0x1E0. Matrices occupy +0x10..+0x3F.
static_assert(0x180 + 64 <= CellAt);
static_assert(CellAt + CellBytes <= 0x1E0);
static_assert(CellAt + CellBytes <= GhostDriver<int>::SlotSize);
static_assert(8 + 23 + 1 == CellBytes);
Controller::Controller(std::shared_ptr<const Keys> k, OotGameDataTrace::GhostOffsets g)
    : keys{std::move(k)}, ghost{std::move(g)}, states(GhostSlots) {}
bool Controller::On(size_t slot, std::string_view name) const {
    return keys->hide_armour && DesiredFrame(*keys, slot, name) != 0;
}
void Controller::Line(Port& port, std::string_view text) {
    if (lines++ < 4096) port.log(fmt::format("OoT co-op: colour {}", text));
}
bool Controller::Eligible(const View& v, std::string_view scene, bool ready) const {
    return keys->enabled && ready && !scene.empty() && v.banc == scene && v.live && v.placed &&
        v.alpha_done && !v.parked && !v.abandon && v.actor && v.record && v.vtable;
}
bool Controller::Valid(Port& p, const View& v) const {
    if (v.fields.size() != ghost.actor_fields.size() ||
        R64(p, v.actor, ghost.link[0]) != v.record || R64(p, v.record, ghost.link[2]) != v.actor ||
        R32(p, v.record, ghost.link[3]) != v.generation || R64(p, v.actor, 0) != v.vtable) return false;
    for (size_t i = 0; i < v.fields.size(); ++i)
        if (R64(p, v.actor, ghost.actor_fields[i]) != v.fields[i]) return false;
    if (ghost.lifecycle) {
        const auto life = R32(p, v.actor, ghost.lifecycle);
        if (!life || (*life >= ghost.lifecycle_lo && *life <= ghost.lifecycle_hi)) return false;
    }
    return true;
}
std::optional<bool> Controller::Palette(Port& p, const View& v, u32& banks) const {
    if (!keys->instance || !keys->bank) return true;
    const auto& i = *keys->instance;
    const auto& b = *keys->bank;
    const auto getter = p.verified(keys->getter);
    const u64 holder = R64(p, v.actor, i[0]).value_or(0);
    const u64 model = R64(p, holder, i[1]).value_or(0);
    const u64 vt = R64(p, model, 0).value_or(0);
    if (!getter || R64(p, vt, i[2]) != getter) return {};
    const u64 instance = R64(p, model, i[3]).value_or(0);
    const u64 manager = R64(p, instance, b[0]).value_or(0);
    const auto count = R32(p, manager, b[1]);
    const u64 list = R64(p, manager, b[2]).value_or(0);
    if (!count || *count > 64 || (*count && !list)) return {};
    banks = *count;
    bool found = false;
    for (u32 n = 0; n < *count; ++n) {
        const u64 bank = R64(p, list, u64{n} * 8).value_or(0);
        const auto size = R16(p, bank, b[3]);
        const u64 records = R64(p, bank, b[4]).value_or(0);
        if (!size || *size > 256 || (*size && !records)) return {};
        for (u32 r = 0; r < *size; ++r) {
            const u64 name = R64(p, records, u64{r} * keys->record_stride + keys->record_name).value_or(0);
            if (!name) return {};
            bool equal = true;
            for (size_t c = 0; c <= keys->animation.size(); ++c) {
                const auto at = Address(name, c);
                const auto byte = at ? p.r8(*at) : std::nullopt;
                if (!byte) return {};
                const u8 expected = c < keys->animation.size() ? static_cast<u8>(keys->animation[c]) : 0;
                if (*byte != expected) { equal = false; break; }
            }
            found |= equal;
        }
        if (R64(p, list, u64{n} * 8) != bank || R16(p, bank, b[3]) != size ||
            R64(p, bank, b[4]) != records) return {};
    }
    if (R64(p, v.actor, i[0]) != holder || R64(p, holder, i[1]) != model ||
        R64(p, model, 0) != vt || R64(p, vt, i[2]) != getter ||
        R64(p, model, i[3]) != instance || R64(p, instance, b[0]) != manager ||
        R32(p, manager, b[1]) != count || R64(p, manager, b[2]) != list) return {};
    return found;
}
bool Controller::Frame(Port& p, const std::vector<View>& views, std::string_view scene, bool ready,
                       const Names& names, u64 tick, u64 block) {
    if (Calling() || (called && tick == last_call_tick)) return false;
    for (size_t n = 0; n < std::min(views.size(), states.size()); ++n) {
        const size_t index = (cursor + n) % std::min(views.size(), states.size());
        const auto& v = views[index];
        const auto name = names(v.origin);
        const u8 frame = DesiredFrame(*keys, index, name);
        auto& s = states[index];
        if (!SameOwner(s.owner, v)) s = State{v, frame};
        if (s.frame != frame) { const bool logged = s.palette_logged; s = State{v, frame}; s.palette_logged = logged; }
        if (!frame || s.applied || s.gave_up || !Eligible(v, scene, ready)) continue;
        if (s.attempts && tick - s.last < 30) continue;
        if (s.attempts >= 3) {
            s.gave_up = true;
            Line(p, fmt::format("ghost {} give-up after 3 attempts", v.origin.substr(0, 8)));
            continue;
        }
        ++s.attempts;
        s.last = tick;
        if (!Valid(p, v)) { Line(p, "drop before issue: actor no longer valid"); continue; }
        u32 banks{};
        const auto palette = Palette(p, v, banks);
        if (palette && keys->bank && !s.palette_logged) {
            Line(p, fmt::format("ghost {} palette {} banks={}", v.origin.substr(0, 8), *palette ? "present" : "missing", banks));
            s.palette_logged = true;
        }
        if (!palette || !*palette) {
            Line(p, palette ? "palette missing; not applied" : "palette unreadable; not applied");
            continue;
        }
        const auto entry = p.verified(keys->apply);
        const auto cell = Address(block, index * GhostDriver<int>::SlotSize + CellAt);
        if (!entry || !cell || *cell > std::numeric_limits<u64>::max() - CellBytes) {
            Line(p, "drop before issue: entry or block invalid"); continue;
        }
        std::array<u8, CellBytes> bytes{};
        const u64 string = *cell + 8;
        std::memcpy(bytes.data(), &string, 8);
        std::memcpy(bytes.data() + 8, keys->animation.data(), keys->animation.size());
        if (!p.write(*cell, bytes.data(), bytes.size()) || !Valid(p, v) ||
            !p.call(*entry, {v.actor, *cell}, static_cast<float>(frame))) {
            Line(p, "drop before issue: write, validation or call refused"); continue;
        }
        pending = Pending{index, v, frame};
        called = true;
        last_call_tick = tick;
        cursor = (index + 1) % states.size();
        Line(p, fmt::format("ghost {} frame={} actor={:#x} gen={:#x} ({})", v.origin.substr(0, 8),
            frame, v.actor, v.generation, keys->players.contains(name) ? "player " + name : "slot"));
        return true;
    }
    return false;
}
void Controller::Returned(Port& p, const std::vector<View>& views, std::string_view scene, bool ready,
                          const Names& names) {
    if (!pending) return;
    const auto call = *pending;
    pending.reset();
    if (call.index >= views.size() || !SameOwner(call.owner, views[call.index]) ||
        !Eligible(views[call.index], scene, ready) || !Valid(p, call.owner) ||
        DesiredFrame(*keys, call.index, names(call.owner.origin)) != call.frame) {
        Line(p, "drop on return: actor, scene or desired frame changed");
        return;
    }
    states[call.index].applied = true;
    Line(p, fmt::format("ghost {} frame={} applied", call.owner.origin.substr(0, 8), call.frame));
}
} // namespace Core::OotCoop::Colour
