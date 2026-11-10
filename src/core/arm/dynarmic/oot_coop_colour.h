// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/arm/dynarmic/oot_coop_equip.h"

namespace Core::OotCoop::Colour {
using Port = Equip::Port;
// The profile entry shape ({at, words}); colour requires exactly eight verification words.
std::optional<OotGameDataTrace::Entry> DecodeEntry(const nlohmann::json& value);
struct Keys {
    bool enabled{}, hide_armour{true};
    OotGameDataTrace::Entry apply, getter;
    std::string animation;
    std::vector<u8> frames;
    std::map<std::string, u8, std::less<>> players;
    std::optional<std::array<u64, 4>> instance;
    std::optional<std::array<u64, 5>> bank;
    u64 record_stride{}, record_name{};
};
std::shared_ptr<const Keys> DecodeKeys(const nlohmann::json& value);
u8 DesiredFrame(const Keys& keys, size_t slot, std::string_view name);
std::optional<Equip::Packet> MaskEquipment(std::optional<Equip::Packet> packet, bool child,
                                         bool colour_on);
inline constexpr u64 CellAt = 0x1C0;
inline constexpr u64 CellBytes = 0x20;
struct View {
    std::string origin, banc;
    u64 actor{}, record{}, vtable{}, session{}, epoch{};
    u32 generation{};
    std::vector<u64> fields;
    bool child{}, live{}, placed{}, parked{}, abandon{}, alpha_done{};
    bool operator==(const View&) const = default;
};
using Names = std::function<std::string(std::string_view)>;
class Controller {
public:
    Controller(std::shared_ptr<const Keys> keys, OotGameDataTrace::GhostOffsets ghost);
    bool Calling() const { return pending.has_value(); }
    bool On(size_t slot, std::string_view name) const;
    bool Frame(Port& port, const std::vector<View>& views, std::string_view scene, bool ready,
               const Names& names, u64 tick, u64 block);
    void Returned(Port& port, const std::vector<View>& views, std::string_view scene, bool ready,
                  const Names& names);
private:
    struct State {
        View owner;
        u8 frame{};
        unsigned attempts{};
        u64 last{};
        bool applied{}, gave_up{}, palette_logged{};
    };
    struct Pending { size_t index{}; View owner; u8 frame{}; };
    std::shared_ptr<const Keys> keys;
    OotGameDataTrace::GhostOffsets ghost;
    std::vector<State> states;
    std::optional<Pending> pending;
    size_t cursor{};
    u64 lines{}, last_call_tick{};
    bool called{};
    bool Valid(Port& port, const View& view) const;
    bool Eligible(const View& view, std::string_view scene, bool ready) const;
    std::optional<bool> Palette(Port& port, const View& view, u32& banks) const;
    void Line(Port& port, std::string_view text);
};
} // namespace Core::OotCoop::Colour
