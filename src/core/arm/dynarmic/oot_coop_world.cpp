// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_world.h"
#include <charconv>
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace Core::OotCoop::World {
using Json = nlohmann::json;
std::optional<u64> Number(const Json& value, u64 maximum) {
    u64 result{};
    if (value.is_number_unsigned()) result = value.get<u64>();
    else if (value.is_number_integer() && value.get<s64>() >= 0) result = value.get<u64>();
    else if (value.is_string()) {
        auto text = value.get<std::string>();
        if (text.size() < 3 || text.size() > 18 || (!text.starts_with("0x") && !text.starts_with("0X"))) return {};
        const auto [end, error] = std::from_chars(text.data() + 2, text.data() + text.size(), result, 16);
        if (error != std::errc{} || end != text.data() + text.size()) return {};
    } else return {};
    return result <= maximum ? std::optional{result} : std::nullopt;
}

bool DecodeProfileSection(std::string_view field, const Json& value, OotGameDataTrace::Profile& profile) {
    using OotGameDataTrace::Point;
    const auto offset = [](const Json& number) -> std::optional<u64> {
        const auto at = Number(number, 0x10000000); // module-size bound, not a guest address
        return at && *at && !(*at & 3) ? at : std::nullopt;
    };
    const auto entry = [&](const Json& object) -> std::optional<OotGameDataTrace::Entry> {
        if (!object.is_object() || object.size() != 2 || !object.contains("at") ||
            !object.contains("words") || !object.at("words").is_array() ||
            object.at("words").empty() || object.at("words").size() > 16) return {};
        const auto at = offset(object.at("at"));
        if (!at) return {};
        OotGameDataTrace::Entry result{*at, {}};
        for (const auto& word : object.at("words")) {
            const auto number = Number(word, UINT32_MAX);
            if (!number) return {};
            result.words.push_back(static_cast<u32>(*number));
        }
        return result;
    };
    if (field == "pickup") {
        // World pickups: {"callers": [return offsets], "names": [exact item names]}.
        if (!value.is_object() || !value.contains("callers") ||
            !value.contains("names") || !value.at("callers").is_array() ||
            !value.at("names").is_array() || value.at("callers").empty() ||
            value.at("callers").size() > 8 || value.at("names").empty() ||
            value.at("names").size() > 32) return false;
        for (const auto& [name, part] : value.items()) {
            if (name == "callers" || name == "names") continue;
            if (name == "component_entry" || name == "death_wrapper") {
                const auto decoded = entry(part);
                if (!decoded) return {};
                if (name == "component_entry") {
                    if (decoded->words.front() != OotGameDataTrace::Pacibsp) return {};
                    profile.pickup_entry = *decoded;
                    profile.sites.push_back({Point::Pickup, decoded->offset, decoded->words.front()});
                } else profile.death_wrapper = *decoded;
            } else if (name == "return_sites") {
                if (!part.is_array() || part.size() != 2) return {};
                for (const auto& row : part) {
                    const auto decoded = entry(row);
                    if (!decoded || (decoded->words.front() & 0x9F00001F) != 0x90000008) return {};
                    profile.pickup_returns.push_back(*decoded);
                    profile.sites.push_back({Point::PickupReturn, decoded->offset, decoded->words.front()});
                }
            } else if (name == "actor_off" || name == "metadata_off" || name == "key_off") {
                const auto number = Number(part, 0xFFFF);
                if (!number || !*number || (*number & 7)) return {};
                (name == "actor_off" ? profile.world.actor_off : name == "metadata_off" ?
                    profile.world.metadata_off : profile.world.key_off) = *number;
            } else return {};
        }
        for (const auto& item : value.at("callers")) {
            const auto at = offset(item);
            if (!at) return {};
            profile.pickup_callers.push_back(*at);
        }
        for (const auto& item : value.at("names")) {
            if (!item.is_string() || !ItemName(item.get<std::string>())) return {};
            profile.pickup_names.push_back(item.get<std::string>());
        }
    } else if (field == "world") {
        if (!World::DecodeConfig(value, profile.world)) return {};
    } else if (field == "wallet") {
        if (!value.is_object()) return {};
        for (const auto& [name, part] : value.items()) {
            if (name == "shop_lr") {
                const auto at = offset(part);
                if (!at) return {};
                profile.world.shop_lr = *at;
            } else if (name == "pending_off") {
                const auto at = Number(part, 0xFFFF);
                if (!at || !*at || (*at & 3)) return {};
                profile.world.pending_off = *at;
            } else if (name == "capacity_hash") {
                const auto key = Number(part, UINT32_MAX);
                if (!key || !*key) return {};
                profile.world.capacity_hash = static_cast<u32>(*key);
            } else return {};
        }
    } else if (field == "token") {
        if (!value.is_object()) return {};
        for (const auto& [name, part] : value.items()) {
            if (name == "enabled") {
                if (!part.is_boolean()) return {};
                profile.world.tokens = part.get<bool>();
            } else if (name == "counter_hash" || name == "ownership_hash") {
                const auto key = Number(part, UINT32_MAX);
                if (!key || !*key) return {};
                (name == "counter_hash" ? profile.world.counter_hash : profile.world.ownership_hash) = static_cast<u32>(*key);
            } else if (name == "pairs") {
                if (!part.is_array() || part.size() > 1024) return {};
                std::set<u64> keys;
                for (const auto& row : part) {
                    if (!row.is_array() || row.size() != 2) return {};
                    const auto h = Number(row[0], UINT64_MAX), partner = Number(row[1], UINT64_MAX);
                    if (!h || !partner || !*h || !*partner || !keys.insert(*h).second ||
                        !keys.insert(*partner).second) return {};
                    profile.world.pairs.emplace(*h, *partner);
                }
            } else return {};
        }
    } else return false;
    return true;
}

bool ValidateProfile(const OotGameDataTrace::Profile& profile) {
    if (profile.world.capacity_hash && !profile.world.pending_off) return false;
    if (!profile.world.families.empty() || profile.pickup_entry) {
        if (!profile.pickup_entry || !profile.world.actor_off || !profile.world.metadata_off || !profile.world.key_off ||
            profile.pickup_returns.size() != 2) return {};
        for (const auto& ret : profile.pickup_returns)
            if (std::find(profile.pickup_callers.begin(), profile.pickup_callers.end(), ret.offset) ==
                profile.pickup_callers.end()) return {};
    }
    for (const auto& [h, partner] : profile.world.pairs) {
        if (!profile.world.families.contains(h) || !profile.world.families.contains(partner) ||
            profile.world.families.at(h) != World::Family::TokenH ||
            profile.world.families.at(partner) != World::Family::TokenPartner) return {};
    }
    return true;
}

bool Valid(Origin origin) {
    switch (origin.kind) {
    case OriginKind::None: return !origin.key && !origin.partner;
    case OriginKind::Chest: return origin.key && origin.key <= UINT32_MAX && !origin.partner;
    case OriginKind::Bit: return origin.key && !origin.partner;
    case OriginKind::Token: return origin.key && origin.partner && origin.key != origin.partner;
    }
    return false;
}
bool OriginField(std::string_view field) {
    return field == "origin_kind" || field == "origin_key" || field == "origin_key64" ||
           field == "origin_partner";
}
void EncodeOrigin(Json& packet, Origin origin) {
    if (!origin) return;
    if (origin.kind == OriginKind::Chest) {
        packet["origin_kind"] = "chest";
        packet["origin_key"] = origin.key;
    } else {
        packet["origin_kind"] = origin.kind == OriginKind::Token ? "token" : "bit";
        packet["origin_key64"] = origin.key;
        if (origin.kind == OriginKind::Token) packet["origin_partner"] = origin.partner;
    }
}
std::optional<Origin> DecodeOrigin(const Json& packet) {
    const auto integer = [&](const char* name, u64 max) -> std::optional<u64> {
        const auto it = packet.find(name);
        if (it == packet.end() || !it->is_number_integer()) return {};
        return Number(*it, max);
    };
    if (!packet.is_object()) return {};
    std::string kind = packet.contains("origin_key") ? "chest" : "none";
    if (packet.contains("origin_kind")) {
        if (!packet.at("origin_kind").is_string()) return {};
        kind = packet.at("origin_kind").get<std::string>();
    }
    Origin result;
    if (kind == "none") {
        if (packet.contains("origin_key") || packet.contains("origin_key64") || packet.contains("origin_partner")) return {};
    } else if (kind == "chest") {
        const auto key = integer("origin_key", UINT32_MAX);
        if (!key || !*key || packet.contains("origin_key64") || packet.contains("origin_partner")) return {};
        result = Origin::Chest(static_cast<u32>(*key));
    } else if (kind == "bit" || kind == "token") {
        const auto key = integer("origin_key64", UINT64_MAX);
        if (!key || !*key || packet.contains("origin_key")) return {};
        result = {kind == "bit" ? OriginKind::Bit : OriginKind::Token, *key, 0};
        if (kind == "token") {
            const auto partner = integer("origin_partner", UINT64_MAX);
            if (!partner) return {};
            result.partner = *partner;
        } else if (packet.contains("origin_partner")) return {};
    } else return {};
    return Valid(result) ? std::optional{result} : std::nullopt;
}

bool DecodeConfig(const Json& value, Config& config) {
    if (!value.is_object()) return false;
    for (const auto& [field, item] : value.items()) {
        if (field == "hearts") {
            if (!item.is_boolean()) return false;
            config.hearts = item.get<bool>();
        } else if (field == "cleanup") {
            if (item != "flag_only" && item != "death") return false;
            config.cleanup = item == "death" ? Cleanup::Death : Cleanup::FlagOnly;
        } else if (field == "wallet_reconcile") {
            if (item != "off" && item != "target_current") return false;
            config.reconcile = item == "off" ? Reconcile::Off : Reconcile::TargetCurrent;
        } else if (field == "families") {
            if (!item.is_array() || item.size() > Claims::Limit) return false;
            for (const auto& row : item) {
                if (!row.is_object() || row.size() != 2 || !row.contains("key") || !row.contains("family")) return false;
                const auto key = Number(row.at("key"), UINT64_MAX);
                const auto& family = row.at("family");
                const std::map<std::string, Family> names{{"rupee", Family::Rupee}, {"heart", Family::Heart},
                    {"piece", Family::Piece}, {"container", Family::Container}, {"token-H", Family::TokenH},
                    {"token-partner", Family::TokenPartner}};
                if (!key || !*key || !family.is_string()) return false;
                const auto found = names.find(family.get<std::string>());
                if (found == names.end() || !config.families.emplace(*key, found->second).second) return false;
            }
        } else return false;
    }
    return true;
}
bool Config::AdmitsFlag(u64 key) const {
    const auto it = families.find(key);
    return it != families.end() && (it->second == Family::Rupee || it->second == Family::TokenH ||
        it->second == Family::TokenPartner || hearts);
}
std::optional<Origin> Config::Source(u64 key) const {
    if (!AdmitsFlag(key)) return {};
    const auto family = families.at(key);
    if (family == Family::TokenH || family == Family::TokenPartner) {
        if (!tokens) return {};
        for (const auto& [h, partner] : pairs)
            if (key == h || key == partner) return Origin{OriginKind::Token, h, partner};
        return {};
    }
    return Origin{OriginKind::Bit, key, 0};
}
bool Config::Admits(Origin origin) const {
    if (!Valid(origin)) return false;
    if (origin.kind == OriginKind::None || origin.kind == OriginKind::Chest) return true;
    return Source(origin.key) == origin;
}
std::vector<u64> Flags(Origin origin) {
    if (origin.kind == OriginKind::Bit) return {origin.key};
    if (origin.kind == OriginKind::Token) return {origin.key, origin.partner};
    return {};
}
bool Claims::Reserve(Origin origin, std::string sender) {
    if (!Valid(origin)) return false;
    if (!origin) return true;
    if (Busy(origin) || pending.size() + applied.size() >= Limit) return false;
    pending.emplace(origin, std::move(sender));
    return true;
}
bool Claims::Complete(Origin origin, std::string_view sender) {
    if (!Valid(origin)) return false;
    if (!origin) return true;
    const auto it = pending.find(origin);
    if (it == pending.end() || it->second != sender) return false;
    applied.insert(origin);
    pending.erase(it);
    return true;
}
void Claims::Cancel(Origin origin, std::string_view sender) {
    const auto it = pending.find(origin);
    if (it != pending.end() && it->second == sender) pending.erase(it);
}
std::optional<Origin> Claims::GaveFlag(u64 key) const {
    if (!key) return {};
    for (const auto& origin : applied)
        if ((origin.kind == OriginKind::Bit || origin.kind == OriginKind::Token) &&
            (origin.key == key || origin.partner == key)) return origin;
    return {};
}
void Pickups::Put(Capture capture) {
    if (!capture.thread) return;
    captures.erase(capture.thread);
    if (capture.origin && captures.size() < 64) captures.emplace(capture.thread, capture);
}
std::optional<Origin> Pickups::Take(u64 thread, u64 epoch, u64 tick, u64 sp) {
    const auto it = captures.find(thread);
    if (it == captures.end()) return {};
    const auto capture = it->second;
    captures.erase(it);
    if (capture.epoch != epoch || tick < capture.tick || tick - capture.tick > 1800) return {};
    if (sp >= capture.sp || capture.sp - sp > StackSpan) return {};
    return capture.origin;
}
std::optional<Wallet> DecodeWallet(const Json& value) {
    if (!value.is_object() || value.size() != 2 || !value.contains("balance") || !value.contains("capacity")) return {};
    if (!value.at("balance").is_number_integer() || !value.at("capacity").is_number_integer()) return {};
    const auto balance = Number(value.at("balance"), INT32_MAX), capacity = Number(value.at("capacity"), INT32_MAX);
    if (!balance || !capacity || *balance > *capacity) return {};
    return Wallet{static_cast<s32>(*balance), static_cast<s32>(*capacity)};
}
Json EncodeWallet(Wallet wallet) { return {{"balance", wallet.balance}, {"capacity", wallet.capacity}}; }
std::optional<s64> ReconcileDelta(Wallet ours, Wallet peer, Reconcile mode) {
    if (mode != Reconcile::TargetCurrent || ours.capacity != peer.capacity || ours.balance < 0 ||
        ours.balance > ours.capacity || peer.balance < 0 || peer.balance > peer.capacity) return {};
    const s64 delta = s64{peer.balance} - ours.balance;
    if (delta < -999 || delta > 999) return {};
    return delta;
}
} // namespace Core::OotCoop::World
