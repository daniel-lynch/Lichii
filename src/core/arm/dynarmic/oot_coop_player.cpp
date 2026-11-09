// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/dynarmic/oot_coop_player.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <utility>

namespace Core::OotCoop::Player {
namespace {
constexpr u64 OffsetLimit = 0x10000000;
std::optional<u64> Number(const nlohmann::json& value, u64 limit) {
    u64 n{};
    if (value.is_number_unsigned()) {
        n = value.get<u64>();
    } else if (value.is_string()) {
        const auto text = value.get<std::string>();
        const bool hex = text.starts_with("0x") || text.starts_with("0X");
        const char* begin = text.data() + (hex ? 2 : 0);
        const char* end = text.data() + text.size();
        const auto [stop, error] = std::from_chars(begin, end, n, hex ? 16 : 10);
        if (error != std::errc{} || stop != end || begin == end) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return n <= limit ? std::optional<u64>{n} : std::nullopt;
}
} // namespace

std::optional<Keys> DecodeKeys(const nlohmann::json& value) {
    try {
        if (!value.is_object()) return std::nullopt;
        Keys k;
        const std::array<std::pair<std::string_view, u64 Keys::*>, 11> offsets{{
            {"user_cell", &Keys::user_cell}, {"user_equipment", &Keys::user_equipment},
            {"owner", &Keys::owner}, {"holder", &Keys::holder}, {"equipment", &Keys::equipment},
            {"actor_record", &Keys::actor_record}, {"actor_id", &Keys::actor_id},
            {"record_actor", &Keys::record_actor}, {"record_id", &Keys::record_id},
            {"actor_name", &Keys::actor_name}, {"lifecycle", &Keys::lifecycle}}};
        size_t known = 0;
        for (const auto& [name, member] : offsets) {
            const auto it = value.find(std::string{name});
            if (it == value.end()) return std::nullopt;
            const auto n = Number(*it, OffsetLimit);
            if (!n) return std::nullopt;
            k.*member = *n;
            ++known;
        }
        if (!k.user_cell) return std::nullopt;
        for (const auto& [name, member] : {std::pair{"lifecycle_lo", &Keys::lifecycle_lo},
                                           std::pair{"lifecycle_hi", &Keys::lifecycle_hi}}) {
            const auto n = Number(value.at(name), 0xFFFF);
            if (!n) return std::nullopt;
            k.*member = static_cast<u32>(*n);
            ++known;
        }
        if (k.lifecycle_lo > k.lifecycle_hi) return std::nullopt;
        const auto& names = value.at("names");
        if (!names.is_array() || names.empty() || names.size() > 4) return std::nullopt;
        for (const auto& name : names) {
            if (!name.is_string()) return std::nullopt;
            auto text = name.get<std::string>();
            if (text.empty() || text.size() > 63 ||
                !std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c <= 126; }))
                return std::nullopt;
            k.names.push_back(std::move(text));
        }
        if (value.size() != known + 1) return std::nullopt; // unknown fields
        return k;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace Core::OotCoop::Player
