// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

#include <boost/asio.hpp>

#include "common/logging.h"
#include "common/thread.h"
#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_ghost.h"
#include "core/arm/dynarmic/oot_coop_runtime.h"
#include "core/arm/dynarmic/oot_coop_ux.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "dynarmic/interface/A64/a64.h"

namespace Core::OotCoop {
namespace {
using Json = nlohmann::json;
using boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::mutex sessions_mutex;
std::shared_ptr<Session> current_session;

std::string Env(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool Unsigned(const Json& value, u64 maximum) {
    if (value.is_number_unsigned()) {
        return value.get<u64>() <= maximum;
    }
    return value.is_number_integer() && value.get<s64>() >= 0 &&
           static_cast<u64>(value.get<s64>()) <= maximum;
}

bool Signed32(const Json& value) {
    if (value.is_number_unsigned()) {
        return value.get<u64>() <= static_cast<u64>(std::numeric_limits<s32>::max());
    }
    return value.is_number_integer() && value.get<s64>() >= std::numeric_limits<s32>::min() &&
           value.get<s64>() <= std::numeric_limits<s32>::max();
}

bool Identity(const Json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
           });
}

Kind StorageKind(Kind kind) {
    return kind == Kind::PouchAdd ? Kind::PouchTotal : kind;
}

std::string_view Type(Kind kind) {
    switch (kind) {
    case Kind::Bool: return "bool_set";
    case Kind::Int: return "int_set";
    case Kind::Enum: return "enum_set";
    case Kind::Quest: return "quest_step";
    case Kind::Bit: return "bit_set";
    case Kind::PouchAdd: return "pouch_add";
    case Kind::PouchTotal: return "pouch_total";
    case Kind::Rupee: return "rupee_add";
    case Kind::Item: return "item_give";
    case Kind::Key: return "key_add";
    case Kind::Pose: return "pose";
    }
    return {};
}
} // namespace

bool ItemName(std::string_view name) {
    return !name.empty() && name.size() <= ItemNameLimit &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_';
           });
}

bool ValidItem(const Change& change) {
    const auto args = UnpackItem(change.key);
    return World::Valid(change.source) && change.kind == Kind::Item && ItemName(change.name) && change.value >= 1 &&
           change.value <= ItemCountLimit && args.category < 16 && args.get < 16 &&
           PackItem(args) == change.key;
}

Json EncodeChange(const Change& change) {
    Json packet{{"type", Type(change.kind)}};
    if (!change.origin.empty()) {
        packet["origin"] = change.origin;
        packet["seq"] = change.seq;
    }
    if (change.kind == Kind::Item) {
        const auto args = UnpackItem(change.key);
        packet["name"] = change.name;
        packet["category"] = args.category;
        packet["count"] = change.value;
        packet["get"] = args.get;
        packet["flags"] = Json::array({args.flags[0], args.flags[1], args.flags[2], args.flags[3]});
        World::EncodeOrigin(packet, change.source);
        return packet;
    }
    if (change.kind == Kind::Bit) World::EncodeOrigin(packet, change.source);
    if (change.kind == Kind::Quest) {
        packet["challenge"] = change.key;
        packet["step"] = change.step;
        packet["index"] = change.value;
    } else if (change.kind == Kind::Bit) {
        packet["actor"] = change.key;
        packet["value"] = change.value != 0;
    } else {
        packet["hash"] = change.key;
        if (change.kind == Kind::Rupee || change.kind == Kind::Key) {
            packet["delta"] = change.value;
        } else if (change.kind == Kind::Bool) {
            packet["value"] = change.value != 0;
        } else {
            packet[change.kind == Kind::PouchAdd || change.kind == Kind::PouchTotal ? "count"
                                                                                 : "value"] =
                change.value;
        }
    }
    return packet;
}

std::optional<Change> DecodeChange(const Json& packet, bool snapshot) {
    try {
        if (!packet.is_object() || !packet.at("type").is_string()) {
            return std::nullopt;
        }
        Change change;
        const auto type = packet.at("type").get<std::string>();
        bool found = false;
        for (const Kind kind : {Kind::Bool, Kind::Int, Kind::Enum, Kind::Quest, Kind::Bit,
                                Kind::PouchAdd, Kind::PouchTotal, Kind::Rupee, Kind::Item, Kind::Key}) {
            if (type == Type(kind)) {
                change.kind = kind;
                found = true;
                break;
            }
        }
        if (!found || (!snapshot && change.kind == Kind::PouchTotal)) {
            return std::nullopt;
        }
        // A delta is an event with an origin and sequence; it never appears as snapshot state.
        if (snapshot && EventKind(change.kind)) {
            return std::nullopt;
        }
        if (!snapshot || packet.contains("origin") || packet.contains("seq")) {
            if (!Identity(packet.at("origin")) || !Unsigned(packet.at("seq"), MaxSequence) ||
                packet.at("seq").get<u64>() == 0) {
                return std::nullopt;
            }
            change.origin = packet.at("origin").get<std::string>();
            change.seq = packet.at("seq").get<u64>();
        }
        const auto source = World::DecodeOrigin(packet);
        if (!source || (*source && change.kind != Kind::Item && change.kind != Kind::Bit) ||
            (change.kind == Kind::Bit && source->kind == World::OriginKind::Chest)) return {};
        change.source = *source;
        if (change.kind == Kind::Item) {
            for (const auto& [field, value] : packet.items()) {
                if (field == "type" || field == "origin" || field == "seq" || field == "name" ||
                    field == "category" || field == "count" || field == "get" || field == "flags")
                    continue;
                if (World::OriginField(field)) continue;
                if (field == "rev" && Unsigned(value, MaxSequence) && value != 0) continue;
                return std::nullopt;
            }
            const auto& flags = packet.at("flags");
            if (!packet.at("name").is_string() || !Unsigned(packet.at("category"), 15) ||
                !Unsigned(packet.at("get"), 15) || !Unsigned(packet.at("count"), ItemCountLimit) ||
                !flags.is_array() || flags.size() != 4) return std::nullopt;
            ItemArgs args{packet.at("category").get<u32>(), packet.at("get").get<u32>(), {}};
            for (size_t i = 0; i < 4; ++i) {
                if (!Unsigned(flags[i], 255)) return std::nullopt;
                args.flags[i] = flags[i].get<u8>();
            }
            change.name = packet.at("name").get<std::string>();
            change.key = PackItem(args);
            change.value = packet.at("count").get<s64>();
            return ValidItem(change) ? std::optional{change} : std::nullopt;
        }
        const char* key = change.kind == Kind::Quest ? "challenge"
                          : change.kind == Kind::Bit ? "actor" : "hash";
        if (!Unsigned(packet.at(key), change.kind == Kind::Bit
                                          ? std::numeric_limits<u64>::max()
                                          : std::numeric_limits<u32>::max())) {
            return std::nullopt;
        }
        change.key = packet.at(key).get<u64>();
        if (change.kind == Kind::Bit && change.source && change.key != change.source.key &&
            (change.source.kind != World::OriginKind::Token || change.key != change.source.partner)) return {};
        const char* value_field = change.kind == Kind::Rupee || change.kind == Kind::Key ? "delta" : "value";
        if (WireKind(change.kind)) {
            for (const auto& [field, value] : packet.items()) {
                if (field == "type" || field == key || field == value_field) continue;
                if (change.kind == Kind::Bit && World::OriginField(field)) continue;
                if (!snapshot && (field == "origin" || field == "seq")) continue;
                if (!snapshot && field == "rev" && Unsigned(value, MaxSequence) && value != 0) continue;
                return std::nullopt;
            }
        }
        if (change.kind == Kind::Rupee) {
            const auto& delta = packet.at("delta");
            if (change.key == 0 || !Signed32(delta) || !RupeeDelta(delta.get<s64>())) {
                return std::nullopt;
            }
            change.value = delta.get<s64>();
        } else if (change.kind == Kind::Key) {
            const auto& delta = packet.at("delta");
            if (change.key == 0 || !Signed32(delta) || !KeyGain(delta.get<s64>())) return std::nullopt;
            change.value = delta.get<s64>();
        } else if (change.kind == Kind::Bool || change.kind == Kind::Bit) {
            if (!packet.at("value").is_boolean()) {
                return std::nullopt;
            }
            change.value = packet.at("value").get<bool>() ? 1 : 0;
        } else if (change.kind == Kind::Quest) {
            if (!Unsigned(packet.at("index"), 255) ||
                !Unsigned(packet.at("step"), std::numeric_limits<u32>::max())) {
                return std::nullopt;
            }
            change.value = packet.at("index").get<s64>();
            change.step = packet.at("step").get<u32>();
        } else {
            const bool pouch = change.kind == Kind::PouchAdd || change.kind == Kind::PouchTotal;
            const auto& value = packet.at(pouch ? "count" : "value");
            if (change.kind == Kind::PouchTotal ? !Unsigned(value, MaxSequence) : !Signed32(value)) {
                return std::nullopt;
            }
            change.value = value.get<s64>();
            if (pouch && change.value <= 0) {
                return std::nullopt;
            }
        }
        return change;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::optional<Json> DecodeLine(std::string_view line) {
    if (line.empty() || line.size() > MaxFrame || line.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    // Bound nesting before parsing (JSON strings can contain brackets).
    size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char c : line) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            if (++depth > 16) return std::nullopt;
        } else if (c == ']' || c == '}') {
            if (depth == 0) return std::nullopt;
            --depth;
        }
    }
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key &&
                 !keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
        return true;
    };
    auto packet = Json::parse(line, callback, false);
    if (duplicate || !packet.is_object()) {
        return std::nullopt;
    }
    return packet;
}

bool WireKind(Kind kind) {
    return kind == Kind::Bool || kind == Kind::Bit || kind == Kind::Rupee || kind == Kind::Item ||
           kind == Kind::Key;
}

std::optional<std::set<Kind>> DecodeAllowClasses(std::string_view text) {
    std::set<Kind> result;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto name = text.substr(0, comma);
        if (name == "bool") result.insert(Kind::Bool);
        else if (name == "bit") result.insert(Kind::Bit);
        else if (name == "rupee") result.insert(Kind::Rupee);
        else if (name == "item") result.insert(Kind::Item);
        else if (name == "pose") result.insert(Kind::Pose);
        else if (name == "dungeon") result.insert(Kind::Key);
        else return std::nullopt;
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (text.empty()) return std::nullopt;
    }
    return result;
}

std::optional<u32> FrameLength(std::string_view header) {
    if (header.size() != 4) return std::nullopt;
    u32 length = 0;
    for (const unsigned char byte : header) length = (length << 8) | byte;
    return length > 0 && length <= MaxFrame ? std::optional<u32>{length} : std::nullopt;
}

std::string EncodeFrame(const Json& packet) {
    auto payload = packet.dump();
    if (!packet.is_object() || payload.empty() || payload.size() > MaxFrame) {
        throw std::length_error{"invalid co-op frame"};
    }
    const auto length = static_cast<u32>(payload.size());
    std::string frame(4, '\0');
    for (size_t i = 0; i < 4; ++i) frame[i] = static_cast<char>(length >> (24 - i * 8));
    return frame + payload;
}

std::optional<Endpoint> ParseEndpoint(std::string_view text) {
    if (text == "1") return Endpoint{"127.0.0.1", 43390};
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    auto host = text.substr(0, colon);
    const auto port_text = text.substr(colon + 1);
    if (host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    } else if (host.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    unsigned port = 0;
    const auto [end, error] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (host.empty() || host.find_first_of(" \r\n\t/") != std::string_view::npos ||
        port_text.empty() || error != std::errc{} || end != port_text.data() + port_text.size() ||
        port == 0 || port > 65535) {
        return std::nullopt;
    }
    return Endpoint{std::string{host}, static_cast<u16>(port)};
}

bool Gate(std::string_view endpoint, u64 program_id) {
    return ParseEndpoint(endpoint).has_value() && program_id == OotGameDataTrace::ProgramId;
}

namespace {
std::optional<u64> Number(const Json& value, u64 maximum) {
    if (Unsigned(value, maximum)) return value.get<u64>();
    if (!value.is_string()) return std::nullopt;
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() < 3 || text.size() > 18 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return std::nullopt;
    u64 number = 0;
    const auto [end, error] = std::from_chars(text.data() + 2, text.data() + text.size(), number, 16);
    if (error != std::errc{} || end != text.data() + text.size() || number > maximum)
        return std::nullopt;
    return number;
}

constexpr u64 MaxModuleOffset = 0x10000000; // 256 MiB: larger than any main module
} // namespace

std::optional<OotGameDataTrace::Profile> DecodeProfile(const Json& document,
                                                       std::string_view build_id) {
    using OotGameDataTrace::Point;
    try {
        if (build_id.size() != 40 || !document.is_object() || !document.contains("offsets"))
            return std::nullopt;
        const auto& all = document.at("offsets");
        const auto found = all.find(std::string{build_id});
        if (!all.is_object() || found == all.end() || !found->is_object()) return std::nullopt;
        const Json& source = *found;
        OotGameDataTrace::Profile profile;
        profile.build_id = std::string{build_id};
        const auto offset = [](const Json& value) -> std::optional<u64> {
            const auto number = Number(value, MaxModuleOffset);
            return number && *number && !(*number & 3) ? number : std::nullopt;
        };
        const auto entry = [&](const Json& value) -> std::optional<OotGameDataTrace::Entry> {
            if (!value.is_object() || value.size() != 2 || !value.at("words").is_array() ||
                value.at("words").empty() || value.at("words").size() > 16) return std::nullopt;
            OotGameDataTrace::Entry result;
            const auto at = offset(value.at("at"));
            if (!at) return std::nullopt;
            result.offset = *at;
            for (const auto& word : value.at("words")) {
                const auto number = Number(word, 0xFFFFFFFF);
                if (!number) return std::nullopt;
                result.words.push_back(static_cast<u32>(*number));
            }
            return result;
        };
        constexpr std::array<std::pair<std::string_view, Point>, 10> points{{
            {"item_give", Point::ItemGive}, {"rupee_add", Point::RupeeAdd},
            {"step_set", Point::StepSet}, {"step_apply", Point::StepApply},
            {"played_event", Point::PlayedEvent}, {"frame", Point::Frame},
            {"bit_read", Point::BitRead}, {"bit_set", Point::BitSet},
            {"struct_bool_set", Point::StructBoolSet}, {"actor_flag_set", Point::ActorFlagSet}}};
        for (const auto& [field, value] : source.items()) {
            if (field == "manager_cell" || field == "pouch_cell") {
                const auto cell = Number(value, MaxModuleOffset);
                if (!cell || !*cell || (*cell & 7)) return std::nullopt;
                (field == "manager_cell" ? profile.manager_cell : profile.pouch_cell) = *cell;
            } else if (field == "wallet_hash") {
                const auto hash = Number(value, 0xFFFFFFFF);
                if (!hash || !*hash) return std::nullopt;
                profile.wallet_hash = static_cast<u32>(*hash);
            } else if (field == "sites") {
                if (!value.is_object()) return std::nullopt;
                for (const auto& [name, list] : value.items()) {
                    const auto it = std::find_if(points.begin(), points.end(),
                                                 [&](const auto& row) { return row.first == name; });
                    if (it == points.end() || !list.is_array() || list.size() > 4) return std::nullopt;
                    for (const auto& item : list) {
                        const auto at = offset(item);
                        if (!at) return std::nullopt;
                        profile.sites.push_back(
                            {it->second, *at, OotGameDataTrace::ReplacedInstruction(it->second)});
                    }
                }
            } else if (field == "entries") {
                if (!value.is_object()) return std::nullopt;
                for (const auto& [name, item] : value.items()) {
                    auto* target = name == "bool_setter"   ? &profile.bool_setter
                                   : name == "bit_setter"  ? &profile.bit_setter
                                   : name == "rupee_entry" ? &profile.rupee_entry
                                   : name == "item_entry"  ? &profile.item_entry
                                   : name == "int_add"     ? &profile.int_add
                                                           : nullptr;
                    const auto decoded = entry(item);
                    if (!target || !decoded) return std::nullopt;
                    *target = *decoded;
                }
            } else if (field == "item_callers") {
                if (!value.is_array() || value.size() > 16) return std::nullopt;
                for (const auto& item : value) {
                    const auto at = offset(item);
                    if (!at) return std::nullopt;
                    profile.item_callers.push_back(*at);
                }
            } else if (field == "pickup" || field == "wallet" || field == "token" || field == "world") {
                if (!World::DecodeProfileSection(field, value, profile)) return {};
            } else if (field == "give") {
                // Generic gives: {"body": [lo, hi], "deny_callers": [...], "per_player": [names
                // from a pickup caller], "never": [names from any caller]}.
                if (!value.is_object() || !value.contains("body")) return std::nullopt;
                for (const auto& [name, item] : value.items()) {
                    if (name == "body") {
                        if (!item.is_array() || item.size() != 2) return std::nullopt;
                        const auto lo = offset(item[0]), hi = offset(item[1]);
                        if (!lo || !hi || *hi <= *lo) return std::nullopt;
                        profile.give_body = {*lo, *hi};
                    } else if (name == "deny_callers") {
                        if (!item.is_array() || item.size() > 32) return std::nullopt;
                        for (const auto& at : item) {
                            const auto decoded = offset(at);
                            if (!decoded) return std::nullopt;
                            profile.give_deny_callers.push_back(*decoded);
                        }
                    } else if (name == "per_player" || name == "never") {
                        if (!item.is_array() || item.size() > 256) return std::nullopt;
                        auto& names = name == "never" ? profile.give_never : profile.give_per_player;
                        for (const auto& text : item) {
                            if (!text.is_string() || !ItemName(text.get<std::string>())) return std::nullopt;
                            names.push_back(text.get<std::string>());
                        }
                    } else {
                        return std::nullopt;
                    }
                }
                profile.give_all = true;
            } else if (field == "chest_members") {
                // [[member hash, backing Bool], ...]: the chest-opened struct's members (race guard)
                if (!value.is_array() || value.size() > 1024) return std::nullopt;
                for (const auto& pair : value) {
                    if (!pair.is_array() || pair.size() != 2) return std::nullopt;
                    const auto member = Number(pair[0], 0xFFFFFFFF), key = Number(pair[1], 0xFFFFFFFF);
                    if (!member || !*member || !key || !*key ||
                        !profile.chest_members.emplace(static_cast<u32>(*member), static_cast<u32>(*key)).second)
                        return std::nullopt;
                }
            } else if (field == "rupee_callers") {
                if (!value.is_array() || value.size() > 8) return std::nullopt;
                for (const auto& item : value) {
                    const auto at = offset(item);
                    if (!at) return std::nullopt;
                    profile.rupee_callers.push_back(*at);
                }
            } else if (field == "event_got") {
                const auto cell = Number(value, MaxModuleOffset);
                if (!cell || !*cell || (*cell & 7)) return std::nullopt;
                profile.event_got = *cell;
            } else if (field == "item_guards") {
                if (!value.is_array() || value.size() > 8) return std::nullopt;
                for (const auto& pair : value) {
                    if (!pair.is_array() || pair.size() != 2) return std::nullopt;
                    const auto cell = Number(pair[0], MaxModuleOffset);
                    const auto member = Number(pair[1], 0xFFFF);
                    if (!cell || !*cell || (*cell & 7) || !member || (*member & 7)) return std::nullopt;
                    profile.item_guards.emplace_back(*cell, *member);
                }
            } else if (field == "ghost") {
                if (!value.is_object()) return std::nullopt;
                auto& ghost = profile.ghost;
                for (const auto& [name, item] : value.items()) {
                    if (name == "playing_info_cell" || name == "actor_mgr_cell") {
                        const auto cell = Number(item, MaxModuleOffset);
                        if (!cell || !*cell || (*cell & 7)) return std::nullopt;
                        (name == "playing_info_cell" ? ghost.playing_info_cell : ghost.actor_mgr_cell) = *cell;
                    } else if (name == "scale_const") {
                        const auto at = offset(item);
                        if (!at) return std::nullopt;
                        ghost.scale_const = *at;
                    } else if (name == "pos" || name == "rot") {
                        const auto field_offset = Number(item, 0xFFFF);
                        if (!field_offset || (*field_offset & 3)) return std::nullopt;
                        (name == "pos" ? ghost.pos : ghost.rot) = *field_offset;
                    } else if (name == "banc_hash" || name == "child_hash" || name == "loading_hash") {
                        const auto hash = Number(item, 0xFFFFFFFF);
                        if (!hash || !*hash) return std::nullopt;
                        (name == "banc_hash" ? ghost.banc_hash : name == "child_hash" ? ghost.child_hash
                                                                                     : ghost.loading_hash) =
                            static_cast<u32>(*hash);
                    } else if (name == "create" || name == "get" || name == "release" || name == "set_mtx") {
                        const auto decoded = entry(item);
                        if (!decoded) return std::nullopt;
                        (name == "create" ? ghost.create : name == "get" ? ghost.get
                                         : name == "release" ? ghost.release : ghost.set_mtx) = *decoded;
                    } else if (name == "str64") {
                        if (!item.is_array() || item.size() != 3) return std::nullopt;
                        for (size_t i = 0; i < 3; ++i) {
                            const auto number = Number(item[i], 0xFFFF);
                            if (!number || !*number) return std::nullopt;
                            ghost.str64[i] = *number;
                        }
                    } else if (name == "link") {
                        if (!item.is_array() || item.size() != 4) return std::nullopt;
                        for (size_t i = 0; i < 4; ++i) {
                            const auto number = Number(item[i], 0xFFFF);
                            if (!number || !*number || (*number & 3)) return std::nullopt;
                            ghost.link[i] = *number;
                        }
                    } else if (name == "lifecycle") {
                        if (!item.is_array() || item.size() != 3) return std::nullopt;
                        const auto field_offset = Number(item[0], 0xFFFF);
                        const auto lo = Number(item[1], 0xFFFFFFFF);
                        const auto hi = Number(item[2], 0xFFFFFFFF);
                        if (!field_offset || !*field_offset || (*field_offset & 3) || !lo || !hi || *lo > *hi)
                            return std::nullopt;
                        ghost.lifecycle = *field_offset;
                        ghost.lifecycle_lo = static_cast<u32>(*lo);
                        ghost.lifecycle_hi = static_cast<u32>(*hi);
                    } else if (name == "actor_fields") {
                        if (!item.is_array() || item.empty() || item.size() > 4) return std::nullopt;
                        for (const auto& f : item) {
                            const auto number = Number(f, 0xFFFF);
                            if (!number || !*number || (*number & 7)) return std::nullopt;
                            ghost.actor_fields.push_back(*number);
                        }
                    } else if (name == "actor_fallback") {
                        if (!item.is_string() || !ItemName(item.get<std::string>())) return std::nullopt;
                        ghost.actor_fallback = item.get<std::string>();
                    } else if (name == "alpha") {
                        if (!item.is_object()) return std::nullopt;
                        for (const auto& [key, part] : item.items()) {
                            if (key == "set") {
                                const auto decoded = entry(part);
                                if (!decoded) return std::nullopt;
                                ghost.alpha_set = *decoded;
                            } else if (key == "chain") {
                                if (!part.is_array() || part.size() != 2) return std::nullopt;
                                for (size_t i = 0; i < 2; ++i) {
                                    const auto number = Number(part[i], 0xFFFF);
                                    if (!number || !*number || (*number & 7)) return std::nullopt;
                                    ghost.alpha_chain[i] = *number;
                                }
                            } else if (key == "percent") {
                                const auto number = Number(part, 100);
                                if (!number || !*number) return std::nullopt;
                                ghost.alpha_percent = static_cast<u32>(*number);
                            } else {
                                return std::nullopt;
                            }
                        }
                        if (!ghost.alpha_set || !ghost.alpha_chain[0] || !ghost.alpha_percent) return std::nullopt;
                    } else if (name == "tint") {
                        // {"set": entry, "colors": [[r, g, b, a], ...]}: one colour per slot.
                        if (!item.is_object() || item.size() != 2 || !item.contains("set") ||
                            !item.contains("colors") || !item.at("colors").is_array() ||
                            item.at("colors").empty() || item.at("colors").size() > GhostSlots)
                            return std::nullopt;
                        const auto decoded = entry(item.at("set"));
                        if (!decoded) return std::nullopt;
                        ghost.tint_set = *decoded;
                        for (const auto& color : item.at("colors")) {
                            if (!color.is_array() || color.size() != 4) return std::nullopt;
                            std::array<float, 4> rgba{};
                            for (size_t i = 0; i < 4; ++i) {
                                if (!color[i].is_number()) return std::nullopt;
                                const double v = color[i].get<double>();
                                if (!(v >= 0.0 && v <= 1.0)) return std::nullopt;
                                rgba[i] = static_cast<float>(v);
                            }
                            ghost.tint_colors.push_back(rgba);
                        }
                    } else if (name == "watch") {
                        // {"pos": actor field, "hide": model component byte}; either may be absent.
                        if (!item.is_object() || item.empty()) return std::nullopt;
                        for (const auto& [key, part] : item.items()) {
                            const auto number = Number(part, 0xFFFF);
                            if ((key != "pos" && key != "hide") || !number || !*number) return std::nullopt;
                            if (key == "pos" && (*number & 3)) return std::nullopt;
                            (key == "pos" ? ghost.watch_pos : ghost.watch_hide) = *number;
                        }
                    } else if (name == "anim") {
                        if (!item.is_object()) return std::nullopt;
                        for (const auto& [key, part] : item.items()) {
                            if (key == "play") {
                                const auto decoded = entry(part);
                                if (!decoded) return std::nullopt;
                                ghost.anim_play = *decoded;
                            } else if (key == "idle" || key == "walk" || key == "run") {
                                if (!part.is_string() || !ItemName(part.get<std::string>())) return std::nullopt;
                                (key == "idle" ? ghost.anim_idle : key == "walk" ? ghost.anim_walk
                                                                                 : ghost.anim_run) =
                                    part.get<std::string>();
                            } else {
                                return std::nullopt;
                            }
                        }
                        if (!ghost.anim_play) return std::nullopt;
                    } else if (name == "actor_child" || name == "actor_adult") {
                        if (!item.is_string() || !ItemName(item.get<std::string>())) return std::nullopt;
                        (name == "actor_child" ? ghost.actor_child : ghost.actor_adult) = item.get<std::string>();
                    } else {
                        return std::nullopt;
                    }
                }
                if (!ghost.Valid()) return std::nullopt;
                // The tint and the hide read go through the model component the alpha chain names.
                if ((ghost.tint_set || ghost.watch_hide) && !ghost.alpha_chain[0]) return std::nullopt;
            } else {
                return std::nullopt;
            }
        }
        if (!World::ValidateProfile(profile)) return {};
        // Two sites at one offset would make Identify ambiguous.
        for (size_t i = 0; i < profile.sites.size(); ++i)
            for (size_t j = i + 1; j < profile.sites.size(); ++j)
                if (profile.sites[i].offset == profile.sites[j].offset) return std::nullopt;
        return profile;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::shared_ptr<const OotGameDataTrace::Profile> LoadProfile(std::string_view build_id,
                                                             std::string* why) {
    const auto fail = [&](std::string text) {
        if (why) *why = std::move(text);
        return nullptr;
    };
    const auto path = Env("EDEN_NX2_OOT_COOP_KEYS");
    if (path.empty()) return fail("no private profile (EDEN_NX2_OOT_COOP_KEYS is unset)");
    std::ifstream in{std::filesystem::path{path}};
    if (!in) return fail("cannot read the private profile");
    std::string text;
    char c;
    while (in.get(c) && text.size() <= MaxFrame) text.push_back(c);
    const auto parsed = DecodeLine(text);
    if (!parsed) return fail("the private profile is not a JSON object");
    auto profile = DecodeProfile(*parsed, build_id);
    if (!profile) return fail("the private profile has no valid offsets for this main build");
    return std::make_shared<const OotGameDataTrace::Profile>(std::move(*profile));
}

bool Enabled() {
    static const bool enabled = ParseEndpoint(Env("EDEN_NX2_OOT_COOP")).has_value();
    return enabled;
}

Json EncodeHello(const Hello& hello) {
    Json packet{{"type", "hello"}, {"version", ProtocolVersion}, {"build_id", hello.build_id},
            {"seed_hash", hello.seed_hash}, {"policy_id", hello.policy_id}, {"room", hello.room},
            {"team", hello.team},
            {"client_id", hello.client_id}, {"name", hello.name}};
    if (hello.world_wallet) packet["world_wallet"] = World::EncodeWallet(*hello.world_wallet);
    return packet;
}

bool MatchesHello(const Json& packet, const Hello& hello) {
    try {
        return packet.at("type") == "hello_ack" &&
               Unsigned(packet.at("version"), ProtocolVersion) &&
               packet.at("version") == ProtocolVersion && packet.at("build_id") == hello.build_id &&
               packet.at("seed_hash") == hello.seed_hash && packet.at("policy_id") == hello.policy_id &&
               packet.at("client_id") == hello.client_id;
    } catch (const Json::exception&) {
        return false;
    }
}

std::optional<std::set<u64>> DecodeDenyList(const Json& packet) {
    if (!packet.is_array() || packet.size() > StateLimit) return std::nullopt;
    std::set<u64> denied;
    for (const auto& key : packet) {
        if (!Unsigned(key, std::numeric_limits<u64>::max())) return std::nullopt;
        denied.insert(key.get<u64>());
    }
    return denied;
}

std::optional<std::set<std::pair<Kind, u64>>> DecodeKeyPolicy(const Json& packet) {
    // Legacy arrays remain Bool64 policies. Typed files keep namespaces separate.
    const Json document = packet.is_array() ? Json{{"bit", packet}} : packet;
    if (!document.is_object()) return std::nullopt;
    std::set<std::pair<Kind, u64>> keys;
    size_t count = 0;
    for (const auto& [name, values] : document.items()) {
        if (name == "offsets" || name == "deny" || name == "dungeon") continue; // DecodeProfile / DecodeKeyExtras
        if (name != "bool" && name != "bit") return std::nullopt;
        const auto list = DecodeDenyList(values);
        if (!list || list->contains(0) || (count += values.size()) > StateLimit) return std::nullopt;
        const Kind kind = name == "bool" ? Kind::Bool : Kind::Bit;
        for (const u64 key : *list) {
            if (kind == Kind::Bool && key > std::numeric_limits<u32>::max()) return std::nullopt;
            keys.emplace(kind, key);
        }
    }
    return keys;
}

std::optional<KeyExtras> DecodeKeyExtras(const Json& document) {
    KeyExtras extras;
    if (!document.is_object()) return extras; // a legacy bare array has no extras
    const auto u32_key = [](const Json& value) -> std::optional<u32> {
        if (!Unsigned(value, std::numeric_limits<u32>::max()) || value.get<u64>() == 0) return std::nullopt;
        return static_cast<u32>(value.get<u64>());
    };
    if (document.contains("deny")) {
        const auto& deny = document.at("deny");
        if (!deny.is_object()) return std::nullopt;
        for (const auto& [name, list] : deny.items()) {
            if (name != "bool" || !list.is_array() || list.size() > DenyLimit) return std::nullopt;
            for (const auto& value : list) {
                const auto key = u32_key(value);
                if (!key) return std::nullopt;
                extras.deny.insert(*key);
            }
        }
    }
    if (document.contains("dungeon")) {
        const auto& dungeon = document.at("dungeon");
        if (!dungeon.is_object()) return std::nullopt;
        for (const auto& [name, list] : dungeon.items()) {
            if (!list.is_array() || list.size() > 64) return std::nullopt;
            if (name == "bool") {
                for (const auto& value : list) {
                    const auto key = u32_key(value);
                    if (!key) return std::nullopt;
                    extras.dungeon_bools.push_back(*key);
                }
            } else if (name == "keys") {
                for (const auto& pair : list) {
                    if (!pair.is_array() || pair.size() != 2) return std::nullopt;
                    const auto total = u32_key(pair[0]), count = u32_key(pair[1]);
                    if (!total || !count || *total == *count) return std::nullopt;
                    extras.dungeon_keys.push_back({*total, *count});
                }
            } else {
                return std::nullopt;
            }
        }
    }
    return extras;
}

bool State::Merge(const Change& change, bool deduplicate) {
    if (deduplicate) {
        if (change.origin.empty() || change.seq == 0 || change.seq > MaxSequence) return false;
        if (!seen.contains(change.origin) && seen.size() >= StateLimit) return false;
        auto& high = seen[change.origin];
        if (change.seq <= high) return false;
        high = change.seq;
    }
    // Events are deduplicated by origin/sequence above and never folded into state.
    if (change.kind == Kind::Item) return ValidItem(change);
    if (denied.contains(change.key)) return false;
    if (change.kind == Kind::Rupee) return change.key != 0 && RupeeDelta(change.value);
    if (change.kind == Kind::Key) return change.key != 0 && KeyGain(change.value);
    const auto key = std::pair{StorageKind(change.kind), change.key};
    const auto it = values.find(key);
    if (it == values.end() && values.size() >= StateLimit) return false;
    const s64 old = it == values.end() ? 0 : it->second.value;
    Change next = change;
    switch (change.kind) {
    case Kind::Bool:
    case Kind::Bit:
        if (change.value != 1 || old == 1) return false;
        break;
    case Kind::Quest:
        if (change.value < 0 || change.value > 255 ||
            (it != values.end() && change.value <= old)) return false;
        break;
    case Kind::Int:
    case Kind::Enum:
        if (change.value < std::numeric_limits<s32>::min() ||
            change.value > std::numeric_limits<s32>::max() ||
            (it != values.end() && change.value == old)) return false;
        break;
    case Kind::PouchAdd:
        if (change.value <= 0 || change.value > std::numeric_limits<s32>::max() ||
            old > static_cast<s64>(MaxSequence) - change.value) return false;
        next.kind = Kind::PouchTotal;
        next.value += old;
        break;
    case Kind::PouchTotal:
        if (change.value <= old || change.value > static_cast<s64>(MaxSequence)) return false;
        break;
    case Kind::Rupee:
    case Kind::Item:
    case Kind::Key:
    case Kind::Pose:
        return false;
    }
    values[key] = std::move(next);
    return true;
}

bool State::AdoptSeen(const HighWater& high_water) {
    size_t added = 0;
    for (const auto& [origin, seq] : high_water) {
        if (!Identity(origin) || seq > MaxSequence) return false;
        added += seen.contains(origin) ? 0 : 1;
    }
    if (seen.size() + added > StateLimit) return false;
    for (const auto& [origin, seq] : high_water) {
        auto& current = seen[origin];
        current = std::max(current, seq);
    }
    return true;
}

std::optional<Change> State::Find(Kind kind, u64 key) const {
    const auto it = values.find({StorageKind(kind), key});
    return it == values.end() ? std::nullopt : std::optional<Change>{it->second};
}

std::vector<Change> State::Snapshot() const {
    std::vector<Change> result;
    for (const auto& [key, value] : values) {
        auto change = value;
        change.origin.clear();
        change.seq = 0;
        change.source = {}; // Baseline flags are state, never a benefit receipt.
        result.push_back(std::move(change));
    }
    return result;
}

bool ShouldApply(const Change& change, const State& observed) {
    const auto local = observed.Find(change.kind, change.key);
    if (change.kind == Kind::Bool || change.kind == Kind::Bit) {
        return change.value == 1 && (!local || local->value == 0);
    }
    if (change.kind == Kind::Quest) {
        return change.value >= 0 && change.value <= 255 && (!local || change.value > local->value);
    }
    if (change.kind == Kind::Int || change.kind == Kind::Enum) {
        return !local || local->value != change.value;
    }
    if (change.kind == Kind::Rupee) return change.key != 0 && RupeeDelta(change.value);
    if (change.kind == Kind::Item) return ValidItem(change);
    if (change.kind == Kind::Key) return change.key != 0 && KeyGain(change.value);
    // Pouch totals require a save baseline/application receipt, not a comparison to inventory.
    return change.kind == Kind::PouchAdd && change.value > 0;
}

struct Session::Impl {
    using Backend = GameDataBackend<Dynarmic::A64::Jit, Memory::Memory>;
    struct Connection;
    struct GhostPort;
    Impl(Endpoint endpoint_, Hello hello_, std::set<u64> denied_, std::set<Kind> allowed_,
         std::set<std::pair<Kind, u64>> keys, u64 wallet, const World::Config& world)
        : endpoint{std::move(endpoint_)}, hello{std::move(hello_)}, denied{std::move(denied_)},
          local{denied}, remote{denied}, allowed_classes{std::move(allowed_)}, retry{io} {
        ApplyPolicy policy{true, allowed_classes, {}, wallet};
        for (const auto& [kind, key] : keys) {
            if (!denied.contains(key) && (kind != Kind::Bit || world.families.empty() || world.AdmitsFlag(key)))
                policy.keys.emplace(kind, key);
        }
        for (const auto& [kind, key] : policy.keys)
            if (kind == Kind::Bit) world_keys.insert(key);
        runtime = std::make_unique<FrameDrain<Backend>>(std::move(policy));
    }
    ~Impl() { Stop(); }
    void Start();
    void Stop();
    void Connect();
    bool Receive(const Json& packet);

    Endpoint endpoint;
    Hello hello;
    std::set<u64> denied;
    std::mutex mutex;
    State local;
    State remote;
    std::set<Kind> allowed_classes;
    std::deque<Change> outgoing;
    std::deque<std::variant<Change, HighWater>> pending;
    u64 next_seq{1};
    std::mutex guest_mutex;
    GuestBinding binding;
    std::unique_ptr<FrameDrain<Backend>> runtime;
    u64 frame_thread{}, tick{}, rebinds{}, item_lines{}, item_waiting_logged{}, rupee_lines{};
    // Class dungeon (guest side, under guest_mutex): small-key Int pairs and their watches.
    std::vector<DungeonKeys> dungeon_keys;
    std::map<u32, KeyWatch> key_watch; // by total key
    // Same-chest race (guest side, under guest_mutex).
    ChestClaims claims;
    World::Claims world_claims;
    World::Pickups pickups;
    struct LocalPickup {
        Change change;
        u64 lr{}, stack{}, epoch{}, tick{};
    };
    std::map<u64, LocalPickup> local_pickups; // thread -> exact give call awaiting its return
    std::set<u64> world_keys;
    std::set<u64> world_flags_sent;
    std::map<World::Origin, std::vector<Change>> world_flags_waiting;
    bool world_quarantined{}, wallet_reconciled{};
    std::optional<World::Wallet> wallet_probe;
    std::optional<World::Wallet> wallet; // protected by mutex
    std::map<std::string, World::Wallet> peer_wallets; // protected by mutex

    struct HeldGive {
        Change change;
        u64 event{}, tick{};
    };
    std::vector<HeldGive> held_gives;
    std::set<u32> chest_bools; // the backing Bools of profile->chest_members
    u64 actor_flags{}, race_lines{};
    u64 key_lines{};
    bool stable_frame{};
    bool baseline_ready{};
    bool logged_class{};
    bool logged_overflow{};
    std::atomic<bool> stopped{};
    // Class "pose" (v3): own pose out, peers' poses in, puppets drawn by the frame drain.
    bool pose_enabled{};
    PoseSender sender;
    std::optional<Pose> pose_out;                         // under mutex: newest unsent own pose
    std::map<std::string, PeerTrack, std::less<>> peers;  // under mutex: by origin
    std::unique_ptr<GhostDriver<GhostPort>> ghost;        // guest side, under guest_mutex
    std::optional<Backend::Context> ghost_saved;
    u64 ghost_stack{}, poses_in{}, poses_out{};
    bool logged_bad_pose{};
    // Relay connection attempts: the delay doubles per failed attempt (1 s .. 30 s) and resets
    // once a connection is ready. Network thread only.
    u32 attempts{}, reconnects{};
    bool ever_ready{};
    std::chrono::seconds backoff{1};
    Ux::SessionUx ux; // toasts and name tags (host only)
    void OnPose(std::string origin, Pose pose) {
        std::scoped_lock lock{mutex};
        if (!peers.contains(origin)) {
            if (peers.size() >= 16) return;
            LOG_INFO(Core_ARM, "OoT co-op: peer {} online (first pose: banc={} loading={})", origin.substr(0, 8),
                     pose.banc.empty() ? "-" : pose.banc, pose.loading);
        }
        auto& track = peers[origin];
        track.online = true;
        track.Add(static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                      Clock::now().time_since_epoch()).count()), std::move(pose));
        ++poses_in;
    }
    boost::asio::io_context io;
    boost::asio::steady_timer retry;
    std::shared_ptr<Connection> connection;
    std::thread thread;
};

struct Session::Impl::Connection : std::enable_shared_from_this<Connection> {
    explicit Connection(Impl& owner_) : owner{owner_}, resolver{owner.io}, socket{owner.io},
                                        timer{owner.io} {}

    void Start() {
        Tick();
        resolver.async_resolve(owner.endpoint.host, std::to_string(owner.endpoint.port),
            [self = shared_from_this()](auto error, tcp::resolver::results_type endpoints) {
                if (self->failed) return;
                if (error) return self->Fail("resolve: " + error.message());
                boost::asio::async_connect(self->socket, endpoints,
                    [self](auto connect_error, const tcp::endpoint&) {
                        if (self->failed) return;
                        if (connect_error) return self->Fail("connect: " + connect_error.message());
                        auto hello = self->owner.hello;
                        {
                            std::scoped_lock lock{self->owner.mutex};
                            hello.world_wallet = self->owner.wallet;
                        }
                        self->Send(EncodeHello(hello));
                        self->Read();
                    });
            });
    }

    void Fail(std::string_view why = "connection error") {
        if (std::exchange(failed, true)) return;
        size_t dropped = 0;
        {
            // No relay, no live peers: their ghosts park until poses arrive again.
            std::scoped_lock lock{owner.mutex};
            dropped = owner.peers.size();
            owner.peers.clear();
            owner.peer_wallets.clear();
        }
        resolver.cancel();
        boost::system::error_code ignored;
        socket.close(ignored);
        timer.cancel();
        if (owner.stopped) return;
        const auto delay = owner.backoff;
        ++owner.attempts;
        if (owner.attempts <= 64 || owner.attempts % 100 == 0) {
            LOG_WARNING(Core_ARM, "OoT co-op: relay {} ({}; {} peer(s) dropped); reconnecting in {} s (attempt {})",
                        ready ? "connection lost" : "connect failed", why, dropped, delay.count(), owner.attempts);
        }
        owner.backoff = std::min(owner.backoff * 2, std::chrono::seconds{30});
        owner.retry.expires_after(delay);
        owner.retry.async_wait([self = shared_from_this()](auto error) {
            if (!error && !self->owner.stopped) self->owner.Connect();
        });
    }

    void Tick() {
        timer.expires_after(50ms);
        timer.async_wait([self = shared_from_this()](auto error) {
            if (error || self->failed) return;
            const auto now = Clock::now();
            if (now - self->last_receive > (self->ready ? 90s : 10s))
                return self->Fail(self->ready ? "nothing received for 90 s" : "no handshake within 10 s");
            if (self->ready) {
                std::vector<Change> changes;
                {
                    std::scoped_lock lock{self->owner.mutex};
                    for (const auto& change : self->owner.outgoing) {
                        if (change.seq > self->sent_seq && changes.size() < 64) {
                            changes.push_back(change);
                        }
                    }
                }
                for (const auto& change : changes) {
                    self->Send(EncodeChange(change));
                    self->sent_seq = change.seq;
                }
                bool request_baseline = false;
                {
                    std::scoped_lock lock{self->owner.mutex};
                    request_baseline = self->owner.baseline_ready &&
                                       self->owner.outgoing.empty() && !self->baseline_requested;
                }
                std::optional<Pose> pose;
                {
                    std::scoped_lock lock{self->owner.mutex};
                    pose = std::exchange(self->owner.pose_out, std::nullopt);
                }
                if (pose) self->Send(EncodePose(*pose));
                if (request_baseline) {
                    self->baseline_requested = true;
                    Json request{{"type", "team_state_request"}};
                    {
                        std::scoped_lock lock{self->owner.mutex};
                        if (self->owner.wallet) request["world_wallet"] = World::EncodeWallet(*self->owner.wallet);
                    }
                    self->Send(request);
                }
                if (now - self->last_heartbeat >= 30s) {
                    self->Send({{"type", "heartbeat"}});
                    self->last_heartbeat = now;
                }
            }
            self->Tick();
        });
    }

    void Send(const Json& packet) {
        if (failed) return;
        if (output.size() >= QueueLimit) return Fail("send queue full");
        const bool writing = !output.empty();
        output.push_back(EncodeFrame(packet));
        if (!writing) Write();
    }

    void Write() {
        boost::asio::async_write(socket, boost::asio::buffer(output.front()),
            [self = shared_from_this()](auto error, size_t) {
                if (self->failed) return;
                if (error) return self->Fail("write: " + error.message());
                self->output.pop_front();
                if (!self->output.empty()) self->Write();
            });
    }

    void Read() {
        boost::asio::async_read(socket, boost::asio::buffer(header),
            [self = shared_from_this()](auto error, size_t) {
                if (self->failed) return;
                if (error) return self->Fail("read: " + error.message());
                const auto length = FrameLength({self->header.data(), self->header.size()});
                if (!length) return self->Fail("bad frame length");
                self->input.resize(*length);
                self->ReadPayload();
            });
    }

    void ReadPayload() {
        boost::asio::async_read(socket, boost::asio::buffer(input),
            [self = shared_from_this()](auto error, size_t) {
                if (self->failed) return;
                if (error) return self->Fail("read: " + error.message());
                const auto packet = DecodeLine(self->input);
                if (!packet) return self->Fail("malformed packet");
                try {
                    self->last_receive = Clock::now();
                    const auto type = packet->at("type").get<std::string>();
                    if (!self->handshaken) {
                        if (!MatchesHello(*packet, self->owner.hello)) {
                            LOG_WARNING(Core_ARM, "OoT co-op: handshake refused or mismatched");
                            return self->Fail(type == "error" && packet->contains("reason") &&
                                                      packet->at("reason").is_string()
                                                  ? "handshake: " + packet->at("reason").get<std::string>().substr(0, 80)
                                                  : std::string{"handshake mismatch"});
                        }
                        self->handshaken = true;
                    } else if (type == "heartbeat") {
                        self->Send({{"type", "heartbeat_ack"}});
                    } else if (type == "pose") {
                        // Poses are never grants: a bad one is ignored, never a disconnect.
                        std::string origin;
                        auto pose = DecodePose(*packet, &origin);
                        if (pose && self->owner.pose_enabled) {
                            self->owner.OnPose(std::move(origin), std::move(*pose));
                        } else if (!pose && !self->owner.logged_bad_pose) {
                            self->owner.logged_bad_pose = true;
                            LOG_WARNING(Core_ARM, "OoT co-op: ignored a malformed pose");
                        }
                    } else if (type == "client_state") {
                        // A peer that went offline hides its ghost (the frame drain parks it).
                        const auto& state = packet->at("state");
                        if (state.is_object() && state.contains("world_wallet")) {
                            const auto wallet = World::DecodeWallet(state.at("world_wallet"));
                            if (!wallet || !Identity(packet->at("origin"))) return self->Fail("invalid wallet baseline");
                            std::scoped_lock lock{self->owner.mutex};
                            if (self->owner.peer_wallets.size() < 128)
                                self->owner.peer_wallets[packet->at("origin").get<std::string>()] = *wallet;
                        }
                        if (packet->contains("origin") && packet->at("origin").is_string() &&
                            state.is_object() && state.contains("name") && state.at("name").is_string()) {
                            self->owner.ux.SetName(packet->at("origin").get<std::string>(),
                                                   state.at("name").get<std::string>());
                        }
                        if (packet->contains("origin") && packet->at("origin").is_string() &&
                            state.is_object() && state.contains("online") &&
                            state.at("online") == false) {
                            const auto origin = packet->at("origin").get<std::string>();
                            std::scoped_lock lock{self->owner.mutex};
                            self->owner.peer_wallets.erase(origin);
                            if (self->owner.peers.erase(origin))
                                LOG_INFO(Core_ARM, "OoT co-op: peer {} offline (relay)", origin.substr(0, 8));
                        }
                    } else if (type == "heartbeat_ack") {
                        // Liveness only.
                    } else if (type == "error") {
                        LOG_WARNING(Core_ARM, "OoT co-op: relay rejected a packet");
                        return self->Fail("relay error");
                    } else {
                        if (!self->ready && type != "team_state") return self->Fail("no baseline first");
                        if (!self->owner.Receive(*packet)) return self->Fail("rejected " + type);
                        if (self->baseline_requested && type == "team_state") {
                            LOG_INFO(Core_ARM, "OoT co-op: baseline exchanged");
                        }
                        if (!self->ready) {
                            self->ready = true;
                            self->Send({{"type", "client_state"},
                                        {"state", {{"apply_enabled", !self->owner.allowed_classes.empty()},
                                                   {"name", self->owner.hello.name}}}});
                            const bool again = std::exchange(self->owner.ever_ready, true);
                            if (again) ++self->owner.reconnects;
                            self->owner.attempts = 0;
                            self->owner.backoff = std::chrono::seconds{1};
                            if (again) {
                                LOG_INFO(Core_ARM, "OoT co-op: relay reconnected (#{}); baseline re-exchanged, "
                                                   "ghosts return with the peers' next poses",
                                         self->owner.reconnects);
                            } else {
                                LOG_INFO(Core_ARM, "OoT co-op: relay connected; frame drain class-gated");
                            }
                        }
                    }
                } catch (const std::exception&) {
                    return self->Fail("bad packet");
                }
                self->Read();
            });
    }

    Impl& owner;
    tcp::resolver resolver;
    tcp::socket socket;
    boost::asio::steady_timer timer;
    Clock::time_point last_receive{Clock::now()};
    Clock::time_point last_heartbeat{Clock::now()};
    bool failed{};
    bool handshaken{};
    bool baseline_requested{};
    bool ready{};
    u64 sent_seq{};
    std::array<char, 4> header{};
    std::string input;
    std::deque<std::string> output;
};

void Session::Impl::Start() {
    Connect();
    thread = std::thread{[this] {
        Common::SetCurrentThreadName("OotCoop");
        try {
            io.run();
        } catch (const std::exception& error) {
            stopped = true;
            LOG_WARNING(Core_ARM, "OoT co-op stopped: {}", error.what());
        }
    }};
}

void Session::Impl::Stop() {
    stopped = true;
    io.stop();
    if (thread.joinable()) thread.join();
    if (connection) {
        boost::system::error_code ignored;
        connection->resolver.cancel();
        connection->socket.close(ignored);
        connection->timer.cancel();
    }
    retry.cancel();
}

void Session::Impl::Connect() {
    if (stopped) return;
    connection = std::make_shared<Connection>(*this);
    connection->Start();
}

bool Session::Impl::Receive(const Json& packet) {
    const auto type = packet.at("type").get<std::string>();
    if (type == "ack") {
        if (!Unsigned(packet.at("seq"), MaxSequence) || packet.at("seq") == 0) return false;
        const u64 seq = packet.at("seq").get<u64>();
        std::scoped_lock lock{mutex};
        if (seq >= next_seq) return false;
        while (!outgoing.empty() && outgoing.front().seq <= seq) outgoing.pop_front();
        return true;
    }
    std::vector<Change> changes, world_replay;
    std::map<std::string, World::Wallet> wallets;
    HighWater snapshot_seen;
    if (type == "team_state") {
        if (packet.contains("world_wallet")) {
            const auto& map = packet.at("world_wallet");
            if (!map.is_object() || map.size() > 128) return false;
            for (const auto& [sender, value] : map.items()) {
                const auto wallet = World::DecodeWallet(value);
                if (!Identity(sender) || !wallet) return false;
                if (sender != hello.client_id) wallets.emplace(sender, *wallet);
            }
        }
        if (!Unsigned(packet.at("revision"), MaxSequence) || !packet.at("changes").is_array() ||
            !packet.at("replay").is_array() || !packet.at("seen").is_object() ||
            packet.at("changes").size() > StateLimit || packet.at("replay").size() > 512 ||
            packet.at("seen").size() > StateLimit) return false;
        State checkpoint{denied};
        for (const auto& [origin, value] : packet.at("seen").items()) {
            if (!Identity(origin) || !Unsigned(value, MaxSequence)) return false;
            snapshot_seen.emplace(origin, value.get<u64>());
        }
        for (const auto& value : packet.at("changes")) {
            // A wallet delta is not state: a reconnect must never re-apply one.
            if (value.is_object() && value.contains("type") &&
                (value.at("type") == "rupee_add" || value.at("type") == "item_give" ||
                 value.at("type") == "key_add")) continue;
            const auto change = DecodeChange(value, true);
            if (!change || !WireKind(change->kind)) return false;
            checkpoint.Merge(*change, false);
        }
        u64 revision = packet.at("revision").get<u64>();
        for (const auto& value : packet.at("replay")) {
            const auto change = DecodeChange(value);
            if (!change || !WireKind(change->kind) || !Unsigned(value.at("rev"), MaxSequence) ||
                value.at("rev").get<u64>() != ++revision) return false;
            if (!EventKind(change->kind)) checkpoint.Merge(*change);
            else if (ever_ready && change->kind == Kind::Item &&
                     (change->source.kind == World::OriginKind::Bit || change->source.kind == World::OriginKind::Token))
                world_replay.push_back(*change);
        }
        changes = checkpoint.Snapshot();
        changes.insert(changes.end(), world_replay.begin(), world_replay.end());
    } else {
        const auto change = DecodeChange(packet);
        if (!change || !WireKind(change->kind) || !Unsigned(packet.at("rev"), MaxSequence) ||
            packet.at("rev") == 0) return false;
        changes.push_back(*change);
    }
    std::scoped_lock lock{mutex};
    if (pending.size() + changes.size() + (type == "team_state" ? 1 : 0) > QueueLimit) {
        if (!logged_overflow) {
            LOG_WARNING(Core_ARM, "OoT co-op: pending queue full; reconnect for a fresh snapshot");
            logged_overflow = true;
        }
        return false;
    }
    if (type == "team_state") peer_wallets = std::move(wallets);
    for (auto& change : changes) pending.emplace_back(std::move(change));
    if (type == "team_state") pending.emplace_back(std::move(snapshot_seen));
    return true;
}

// The ghost's persistent guest memory: the rest of our sentinel page (the brk word is at +0).
inline constexpr u64 GhostBlock = 0x400;
static_assert(GhostBlock + GhostSlots * GhostDriver<int>::SlotSize <= 0x1000);

struct Session::Impl::GhostPort {
    Backend& backend;
    Memory::Memory& memory;
    Impl& impl;
    std::optional<u64> R64(u64 address) { return OotGameDataTrace::Detail::Read64(memory, address); }
    std::optional<u32> R32(u64 address) { return OotGameDataTrace::Detail::Read32(memory, address); }
    bool Write(u64 address, const void* data, size_t size) {
        const u64 low = impl.binding.sentinel + GhostBlock, high = impl.binding.sentinel + 0x1000;
        if (!impl.binding.sentinel || address < low || address + size > high ||
            !memory.IsValidVirtualAddressRange(address, size))
            return false;
        return memory.WriteBlock(address, data, size);
    }
    std::optional<u64> Verified(const OotGameDataTrace::Entry& entry) { return backend.VerifiedEntry(entry); }
    /// The grant path's call: full context captured, private stack, sentinel return. Up to three
    /// stack arguments read as zero.
    bool Call(u64 entry, const std::array<u64, 8>& args, std::optional<float> s0 = std::nullopt) {
        if (!backend.CallReady()) return false;
        auto saved = backend.Capture();
        if (!saved) return false;
        const u64 sp = impl.binding.stack - ItemFrame;
        ApplyTarget target{true, false, 0, 0, entry, impl.binding.sentinel, sp, args};
        target.stores = {{sp, 0}, {sp + 8, 0}, {sp + 0x10, 0}, {sp + 0x18, 0}};
        if (s0) {
            u32 bits;
            std::memcpy(&bits, &*s0, 4);
            target.s0_bits = bits;
        }
        if (!backend.Redirect(target)) return false;
        impl.ghost_saved = std::move(saved);
        impl.ghost_stack = sp;
        return true;
    }
};

Session::Session(Kernel::KernelCore& kernel, Kernel::KProcess* owner_, u64 base_,
                 std::shared_ptr<const OotGameDataTrace::Profile> profile_, Endpoint endpoint, Hello hello,
                 std::set<u64> denied, std::set<Kind> allowed_classes,
                 std::set<std::pair<Kind, u64>> allowed_keys)
    : owner{owner_}, base{base_}, profile{std::move(profile_)},
      impl{std::make_unique<Impl>(std::move(endpoint), std::move(hello), std::move(denied),
                                   std::move(allowed_classes), std::move(allowed_keys),
                                   profile->wallet_hash, profile->world)} {
    impl->binding.base = base;
    if (profile->world.cleanup == World::Cleanup::Death)
        LOG_WARNING(Core_ARM, "OoT co-op: world cleanup remains flag_only: no verified resident actor walk");
    for (const auto& [member, key] : profile->chest_members) impl->chest_bools.insert(key);
    if (!impl->allowed_classes.empty()) {
        Kernel::KProcessAddress stack{}, sentinel{};
        const auto result = owner->AllocateDebugCallMemory(kernel, &stack, &sentinel);
        if (result.IsSuccess()) {
            impl->binding.stack = GetInteger(stack);
            impl->binding.sentinel = GetInteger(sentinel);
        } else {
            LOG_WARNING(Core_ARM,
                        "OoT co-op: private stack/sentinel allocation failed (result {:#x}, "
                        "module {}, description {})",
                        result.raw, static_cast<u32>(result.GetModule()), result.GetDescription());
        }
    }
    if (impl->allowed_classes.contains(Kind::Pose)) {
        if (profile->ghost.Valid() && impl->binding.sentinel) {
            impl->pose_enabled = true;
            impl->ghost = std::make_unique<GhostDriver<Impl::GhostPort>>(profile->ghost, base);
            LOG_INFO(Core_ARM, "OoT co-op: pose class on (ghost block {:#x})",
                     impl->binding.sentinel + GhostBlock);
        } else {
            LOG_WARNING(Core_ARM, "OoT co-op: pose class off: {}",
                        profile->ghost.Valid() ? "no private call memory" : "the private profile has no ghost object");
        }
    }
    {
        std::string why;
        impl->ux.Configure(Ux::LoadConfig(profile->build_id, &why));
        LOG_INFO(Core_ARM, "OoT co-op: UX {}", why);
    }
    impl->Start();
}
Session::~Session() = default;
void Session::Stop() { impl->Stop(); }

void Session::Observe(Change change) {
    // Wallet deltas are observed only at the wallet entry's listed callers (ObserveRupee).
    if (impl->stopped || !WireKind(change.kind)) return;
    if (change.kind == Kind::Item && !ValidItem(change)) return;
    std::scoped_lock lock{impl->mutex};
    if (impl->outgoing.size() >= QueueLimit || impl->next_seq > MaxSequence) {
        if (!impl->logged_overflow) {
            LOG_WARNING(Core_ARM, "OoT co-op: outgoing queue full; observation skipped");
            impl->logged_overflow = true;
        }
        return;
    }
    if (!impl->local.Merge(change, false)) return;
    change.origin = impl->hello.client_id;
    change.seq = impl->next_seq++;
    impl->outgoing.push_back(std::move(change));
}

void Session::ObserveItem(Dynarmic::A64::Jit& jit, Memory::Memory& memory, u64 thread) {
    const auto call = ReadItemCall(jit, memory, base, *profile, impl->binding.sentinel,
                                   impl->allowed_classes.contains(Kind::Item));
    // Every give is logged (bounded): the record of what the game passes, forwarded or not.
    if (impl->item_lines < 256) {
        ++impl->item_lines;
        LOG_INFO(Core_ARM, "OoT co-op: {}", call.line);
    }
    if (call.replay || call.nested || (impl->runtime->Calling() && thread == impl->frame_thread) ||
        !impl->allowed_classes.contains(Kind::Item)) return;
    const u64 event = EventContext(memory).value_or(0);
    if (!profile->chest_members.empty() && impl->claims.SkipLocal(event)) {
        // This event opens a chest a peer opened first and whose gives we take: return from the
        // give at once, as if it had run (w0 = 1); the event's presentation goes on.
        jit.SetRegister(0, 1);
        jit.SetPC(jit.GetRegister(30));
        LOG_INFO(Core_ARM, "OoT co-op: chest race: local give skipped name={} (the chest was claimed by a "
                           "peer first; its give is the one)", call.change.name);
        return;
    }
    if (call.pickup && profile->pickup_entry) {
        const auto source = impl->pickups.Take(thread, impl->rebinds, impl->tick);
        if (!call.forward) return;
        if (impl->world_quarantined || !source || !impl->allowed_classes.contains(Kind::Bit)) {
            LOG_INFO(Core_ARM, "OoT co-op: pickup unlinked or gated; give stays local name={}", call.change.name);
            return;
        }
        if (!impl->world_claims.Reserve(*source, {})) {
            jit.SetRegister(0, 1);
            jit.SetPC(jit.GetRegister(30));
            LOG_INFO(Core_ARM, "OoT co-op: pickup race: local give skipped name={}", call.change.name);
            return;
        }
        if (impl->local_pickups.contains(thread) || impl->local_pickups.size() >= 64) {
            impl->world_quarantined = true;
            impl->runtime->Invalidate();
            return;
        }
        auto change = call.change;
        change.source = *source;
        impl->local_pickups.emplace(thread, Impl::LocalPickup{std::move(change), jit.GetRegister(30),
            jit.GetSP(), impl->rebinds, impl->tick});
        return;
    }
    if (!call.forward) return;
    if (event && !profile->chest_members.empty()) {
        // Held until the event ends, then sent tagged with the chest the event opened.
        impl->claims.Hold(event);
        impl->held_gives.push_back({call.change, event, impl->tick});
        return;
    }
    Observe(call.change);
    impl->ux.ItemShared(call.change.name, call.change.value);
}

std::optional<u64> Session::EventContext(Memory::Memory& memory) const {
    using OotGameDataTrace::Detail::Read64;
    if (!profile->event_got) return std::nullopt;
    const auto cell = Read64(memory, base + profile->event_got);
    const auto events = cell && *cell ? Read64(memory, *cell) : std::nullopt;
    return events && *events ? Read64(memory, *events + 0x68) : std::nullopt;
}

void Session::ObserveActorFlag(Dynarmic::A64::Jit& jit, Memory::Memory& memory) {
    if (++impl->actor_flags % 20000 == 0) {
        LOG_INFO(Core_ARM, "OoT co-op: actor flag setter trapped {} times", impl->actor_flags);
    }
    if ((jit.GetRegister(1) & 1) == 0) return;
    const auto member = OotGameDataTrace::Detail::Read32(memory, jit.GetRegister(8) + 8);
    const auto it = member ? profile->chest_members.find(*member) : profile->chest_members.end();
    if (it == profile->chest_members.end()) return;
    const u32 key = it->second;
    const u64 event = EventContext(memory).value_or(0);
    const auto open = impl->claims.LocalOpen(World::Origin::Chest(key), event);
    size_t dropped = 0;
    if (open == ChestClaims::Open::TakeOver) {
        dropped = impl->runtime->DropPending(
            [&](const Change& c) { return c.kind == Kind::Item && c.source == World::Origin::Chest(key); });
    }
    LOG_INFO(Core_ARM, "OoT co-op: chest open (local) key={:08x} event={:#x}: {}", key, event,
             open == ChestClaims::Open::Ours ? "ours (first claim)"
             : open == ChestClaims::Open::SkipLocal
                 ? "claimed by a peer first: our gives in this event are skipped"
                 : fmt::format("we had already given locally: ours; {} queued peer give(s) dropped", dropped));
}

void Session::ObserveRupee(Dynarmic::A64::Jit& jit, u64 thread) {
    const auto call = ReadRupeeCall(jit, base, *profile, impl->binding.sentinel,
                                    impl->allowed_classes.contains(Kind::Rupee),
                                    impl->runtime->Calling() && thread == impl->frame_thread);
    if (impl->rupee_lines < 256) {
        ++impl->rupee_lines;
        LOG_INFO(Core_ARM, "OoT co-op: {}", call.line);
    }
    if (call.forward) Observe(call.change);
}

void Session::SetDungeonKeys(std::vector<DungeonKeys> keys) {
    std::scoped_lock lock{impl->guest_mutex};
    impl->dungeon_keys = std::move(keys);
    impl->key_watch.clear();
}

bool Session::IsSentinel(u64 pc) const {
    std::scoped_lock lock{impl->guest_mutex};
    return impl->binding.sentinel && pc == impl->binding.sentinel;
}

bool Session::Hook(OotGameDataTrace::Point point, u64 pc, Dynarmic::A64::Jit& jit,
                   Memory::Memory& memory, u64 thread, u64& tls, u64& tls_ro,
                   std::function<void()> clear_exclusive) {
    std::scoped_lock guest_lock{impl->guest_mutex};
    auto& binding = impl->binding;
    auto& runtime = *impl->runtime;
    Impl::Backend backend{jit, memory, binding, *profile, tls, tls_ro, std::move(clear_exclusive)};
    const auto log = [](std::string_view line) { LOG_INFO(Core_ARM, "{}", line); };
    const DrainContext site{thread, 1, impl->tick, true};
    if (binding.sentinel && pc == binding.sentinel && impl->ghost && impl->ghost->Calling()) {
        const u64 result = jit.GetRegister(0);
        if (thread != impl->frame_thread || jit.GetSP() != impl->ghost_stack || !impl->ghost_saved ||
            !backend.Restore(*impl->ghost_saved)) {
            log(fmt::format("OoT co-op: ghost sentinel refused thread={} pc={:#x}", thread, pc));
            return false;
        }
        impl->ghost_saved.reset();
        Impl::GhostPort port{backend, memory, *impl};
        impl->ghost->Returned(port, binding.sentinel + GhostBlock, result, log);
        return true;
    }
    if (binding.sentinel && pc == binding.sentinel) {
        const u64 result = jit.GetRegister(0); // the call's return value, before the restore
        const auto event = runtime.Sentinel(backend, site, pc, log);
        if (event == ApplyEvent::Returned && runtime.LastGrant() &&
            runtime.LastGrant()->kind == Kind::Item) {
            log(fmt::format("OoT co-op: item give returned w0={} name={}", result & 1,
                            runtime.LastGrant()->name));
            const auto& grant = *runtime.LastGrant();
            if (result & 1) impl->ux.ItemReceived(grant.origin, grant.seq, grant.name, grant.value);
            if (grant.source.kind == World::OriginKind::Bit || grant.source.kind == World::OriginKind::Token) {
                if (!(result & 1) || !impl->world_claims.Complete(grant.source, grant.origin)) {
                    impl->world_quarantined = true;
                    runtime.Invalidate();
                    log("OoT co-op: world give outcome uncertain; quarantined without flags");
                } else {
                    log(fmt::format("OoT co-op: pickup benefit received key={:016x} partner={:016x} sender={} seq={}",
                                    grant.source.key, grant.source.partner, grant.origin, grant.seq));
                    for (const u64 key : World::Flags(grant.source)) {
                        Change flag{Kind::Bit, grant.origin, grant.seq, key, 1};
                        flag.source = grant.source;
                        if (!runtime.Queue(flag) && !runtime.Suppress(flag)) runtime.Invalidate();
                    }
                    impl->world_flags_waiting.erase(grant.source);
                }
            }
        }
        if (event == ApplyEvent::Returned && runtime.LastGrant() &&
            runtime.LastGrant()->kind == Kind::Rupee) {
            using OotGameDataTrace::Detail::Read32;
            using OotGameDataTrace::Detail::Read64;
            const u64 pouch = profile->pouch_cell
                                  ? Read64(memory, binding.base + profile->pouch_cell).value_or(0)
                                  : 0;
            const auto pending = pouch ? Read32(memory, pouch + 0x3BD4) : std::nullopt;
            log(fmt::format("OoT co-op: rupee pending={} after the call (pouch {:#x})",
                            static_cast<s32>(pending.value_or(0)), pouch));
            const auto& grant = *runtime.LastGrant();
            impl->ux.RupeeReceived(grant.origin, grant.seq, grant.value);
        }
        // An unowned/invalid sentinel must reach the normal breakpoint fault, never resume.
        if (event != ApplyEvent::Returned) {
            log(fmt::format("OoT co-op: sentinel refused thread={} pc={:#x}", thread, pc));
        }
        return event == ApplyEvent::Returned;
    }
    if (impl->stopped) return false;
    if (point == OotGameDataTrace::Point::Pickup) {
        const auto key = backend.VerifiedEntry(profile->pickup_entry) &&
            !(runtime.Calling() && thread == impl->frame_thread)
            ? World::PickupKey(memory, jit.GetRegister(0), profile->world) : std::nullopt;
        auto source = key ? profile->world.Source(*key) : std::nullopt;
        if (source) {
            for (const u64 flag : World::Flags(*source)) {
                if (!impl->world_keys.contains(flag) ||
                    !backend.ReadFlag(Change{Kind::Bit, {}, 0, flag, 1}).has_value()) source.reset();
            }
        }
        impl->pickups.Put({source.value_or(World::Origin{}), thread, impl->rebinds, impl->tick,
                          EventContext(memory).value_or(0)});
        return false;
    }
    if (point == OotGameDataTrace::Point::PickupReturn) {
        const auto it = impl->local_pickups.find(thread);
        if (it == impl->local_pickups.end()) return false;
        const auto local = it->second;
        const auto ret = std::find_if(profile->pickup_returns.begin(), profile->pickup_returns.end(),
            [&](const auto& candidate) { return base + candidate.offset == pc; });
        if (local.lr != pc || local.stack != jit.GetSP() || local.epoch != impl->rebinds ||
            impl->tick < local.tick || impl->tick - local.tick > 1800 ||
            ret == profile->pickup_returns.end() || !backend.VerifiedEntry(*ret)) {
            impl->world_quarantined = true;
            runtime.Invalidate();
            log("OoT co-op: pickup return identity changed; quarantined");
            return false;
        }
        impl->local_pickups.erase(it);
        if (jit.GetRegister(0) & 1) {
            impl->world_claims.Complete(local.change.source, {});
            impl->world_flags_waiting.erase(local.change.source);
            log(fmt::format("OoT co-op: pickup benefit returned key={:016x} partner={:016x} thread={}",
                            local.change.source.key, local.change.source.partner, thread));
            Observe(local.change);
            impl->ux.ItemShared(local.change.name, local.change.value);
        } else {
            impl->world_claims.Cancel(local.change.source, {});
            log("OoT co-op: pickup give declined; no benefit receipt or flags sent");
        }
        return false;
    }
    if (point == OotGameDataTrace::Point::BitSet) {
        const u64 key = jit.GetRegister(2);
        const auto source = impl->world_claims.GaveFlag(key);
        if (!impl->world_quarantined && !(runtime.Calling() && thread == impl->frame_thread) &&
            (jit.GetRegister(1) & 1) && source &&
            profile->world.AdmitsFlag(key) && impl->allowed_classes.contains(Kind::Bit)) {
            Change flag{Kind::Bit, {}, 0, key, 1};
            flag.source = *source;
            if (!runtime.Suppress(flag) && impl->world_flags_sent.insert(key).second) Observe(flag);
        }
        return false;
    }
    if (point == OotGameDataTrace::Point::ItemGive) {
        ObserveItem(jit, memory, thread);
        return false;
    }
    if (point == OotGameDataTrace::Point::RupeeAdd) {
        ObserveRupee(jit, thread);
        return false;
    }
    if (point == OotGameDataTrace::Point::ActorFlagSet) {
        ObserveActorFlag(jit, memory);
        return false;
    }
    if (point != OotGameDataTrace::Point::Frame || !thread) return false;
    if (!impl->frame_thread) impl->frame_thread = thread;
    if (impl->frame_thread != thread) return false;
    ++impl->tick;
    for (const auto& [pickup_thread, pickup] : impl->local_pickups) {
        if (pickup.epoch != impl->rebinds || impl->tick - pickup.tick > 1800) {
            impl->world_quarantined = true;
            runtime.Invalidate();
            log("OoT co-op: pickup return expired; quarantined without retry");
            break;
        }
    }
    if (!binding.invalid && backend.Moved()) {
        const u64 old = binding.manager;
        const auto now =
            OotGameDataTrace::Detail::Read64(memory, binding.base + profile->manager_cell);
        if (impl->ghost) impl->ghost->DropAll(log, "save or title: GameData moved");
        if (runtime.Rebind(backend, log)) {
            impl->stable_frame = false;
            impl->key_watch.clear(); // a new save epoch: re-baseline the key counts
            impl->pickups.Clear();
            impl->local_pickups.clear();
            if (!profile->world.families.empty()) {
                impl->world_quarantined = true;
                runtime.Invalidate();
                log("OoT co-op: world save identity changed; restart with a matched save baseline");
            }
            if (impl->rebinds < 16) {
                log(fmt::format("OoT co-op: manager moved {:#x} -> {:#x}; baseline re-bound", old,
                                now.value_or(0)));
            }
            ++impl->rebinds;
        }
    }
    const bool bound = backend.BindFromCell();
    if (binding.invalid || (!bound && impl->stable_frame)) {
        if (impl->ghost) impl->ghost->DropAll(log, "GameData binding lost");
        runtime.Invalidate();
        runtime.Frame(backend, {thread, 1, impl->tick, true}, [](Change, bool) {}, log);
        return false;
    }
    if (!bound) return false;
    if (!std::exchange(impl->stable_frame, true)) return false;

    std::deque<std::variant<Change, HighWater>> batch;
    {
        std::scoped_lock lock{impl->mutex};
        for (size_t count = 0; count < 64 && !impl->pending.empty(); ++count) {
            batch.push_back(std::move(impl->pending.front()));
            impl->pending.pop_front();
        }
    }
    for (const auto& entry : batch) {
        if (const auto* seen = std::get_if<HighWater>(&entry)) {
            if (!impl->remote.AdoptSeen(*seen)) runtime.Invalidate();
        } else {
            const auto& change = std::get<Change>(entry);
            if (change.kind == Kind::Key) {
                // One gain, two adds: the dungeon's total and its current count. The credit keeps
                // our own watch from sending the gain back.
                const auto pair = std::find_if(impl->dungeon_keys.begin(), impl->dungeon_keys.end(),
                                               [&](const DungeonKeys& d) { return d.total == change.key; });
                if (!impl->remote.Merge(change, !change.origin.empty())) continue;
                Change count = change;
                if (pair != impl->dungeon_keys.end()) count.key = pair->count;
                if (pair == impl->dungeon_keys.end() || !runtime.Queue(change) || !runtime.Queue(count)) {
                    if (!impl->logged_class) {
                        log("OoT co-op: not applied: class/key policy disabled or queue full");
                        impl->logged_class = true;
                    }
                    continue;
                }
                impl->key_watch[change.key].credit += change.value;
                log(fmt::format("OoT co-op: key gain queued total={:08x} count={:08x} +{}", change.key,
                                count.key, change.value));
                continue;
            }
            const bool world_source = change.source.kind == World::OriginKind::Bit ||
                                      change.source.kind == World::OriginKind::Token;
            if (world_source) {
                const auto flags = World::Flags(change.source);
                if (!std::all_of(flags.begin(), flags.end(), [&](u64 key) { return impl->world_keys.contains(key); })) continue;
                if (impl->world_quarantined || !profile->world.Admits(change.source) ||
                    !impl->allowed_classes.contains(Kind::Bit)) continue;
                if (change.kind == Kind::Item) {
                    if (!impl->allowed_classes.contains(Kind::Item) ||
                        !impl->remote.Merge(change, !change.origin.empty())) continue;
                    if (!impl->world_claims.Reserve(change.source, change.origin)) {
                        log("OoT co-op: pickup race: peer give dropped (source reserved or applied)");
                        continue;
                    }
                    if (!runtime.Queue(change)) {
                        impl->world_claims.Cancel(change.source, change.origin);
                        impl->world_quarantined = true;
                        runtime.Invalidate();
                    }
                    continue;
                }
                if (change.kind == Kind::Bit && !impl->world_claims.Applied(change.source)) {
                    // A source flag may precede its reward, but must never stand in for a receipt.
                    if (impl->world_flags_waiting.size() >= World::Claims::Limit) runtime.Invalidate();
                    else impl->world_flags_waiting.try_emplace(change.source);
                    continue;
                }
            }
            if (change.kind == Kind::Item && change.source.kind == World::OriginKind::Chest && !profile->chest_members.empty() &&
                !impl->claims.AcceptRemote(change)) {
                if (impl->remote.Merge(change, !change.origin.empty()) && impl->race_lines++ < 256) {
                    log(fmt::format("OoT co-op: chest race: peer give dropped name={} chest={:08x} (owner {})",
                                    change.name, change.source.key, impl->claims.Owner(change.source)));
                }
                continue;
            }
            if (change.kind == Kind::Bool && !change.origin.empty() && change.value == 1 &&
                impl->chest_bools.contains(static_cast<u32>(change.key))) {
                impl->claims.RemoteClaim(World::Origin::Chest(static_cast<u32>(change.key)), change.origin);
            }
            if (impl->remote.Merge(change, !change.origin.empty()) && !runtime.Queue(change) &&
                !runtime.Suppress(change) && !impl->logged_class) {
                log("OoT co-op: not applied: class/key policy disabled or queue full");
                impl->logged_class = true;
            }
        }
    }
    const auto send = [&](Change change, bool baseline) {
        if (change.kind == Kind::Bit && !profile->world.families.empty()) {
            if (impl->world_quarantined || !profile->world.AdmitsFlag(change.key)) return;
            if (const auto source = impl->world_claims.GaveFlag(change.key)) change.source = *source;
            else if (!baseline) {
                const auto source = profile->world.Source(change.key);
                if (source && impl->world_claims.Busy(*source)) return;
            }
            if (change.source && !impl->world_flags_sent.insert(change.key).second) return;
        }
        Observe(change);
        log(fmt::format("OoT co-op: {} class={} key={:016x} value=1 path=observer",
                        baseline ? "baseline" : "observed", ClassName(change.kind), change.key));
    };
    if (profile->world.capacity_hash && impl->allowed_classes.contains(Kind::Rupee) &&
        !runtime.Calling() && !runtime.Pending() && impl->local_pickups.empty()) {
        using OotGameDataTrace::Detail::Read32;
        using OotGameDataTrace::Detail::Read64;
        const auto pouch = Read64(memory, base + profile->pouch_cell);
        const auto pending_delta = pouch && *pouch &&
            *pouch <= UINT64_MAX - profile->world.pending_off - sizeof(u32)
                ? Read32(memory, *pouch + profile->world.pending_off) : std::nullopt;
        const auto balance = backend.ReadInt(profile->wallet_hash);
        const auto capacity = backend.ReadInt(profile->world.capacity_hash);
        const auto sample = balance && capacity && *balance >= 0 && *balance <= *capacity &&
            pending_delta == 0 ? std::optional{World::Wallet{*balance, *capacity}} : std::nullopt;
        if (sample && impl->wallet_probe == sample) {
            std::map<std::string, World::Wallet> peers;
            {
                std::scoped_lock lock{impl->mutex};
                impl->wallet = sample;
                peers = impl->peer_wallets;
            }
            if (!impl->wallet_reconciled && !peers.empty()) {
                // A deterministic two-peer baseline target; this is not a live ordered wallet ledger.
                const auto& [sender, target] = *peers.begin();
                log(fmt::format("OoT co-op: wallet baseline ours={} peer={} capacity={}/{}",
                                sample->balance, target.balance, sample->capacity, target.capacity));
                impl->wallet_reconciled = true;
                if (sender < impl->hello.client_id) {
                    const auto delta = World::ReconcileDelta(*sample, target, profile->world.reconcile);
                    if (delta && *delta && !runtime.Queue(Change{Kind::Rupee, {}, 0, profile->wallet_hash, *delta}))
                        runtime.Invalidate();
                    if (!delta && profile->world.reconcile == World::Reconcile::TargetCurrent)
                        log("OoT co-op: wallet baseline correction refused (capacity/range mismatch)");
                }
            }
        }
        impl->wallet_probe = sample;
    } else {
        impl->wallet_probe.reset();
    }
    const auto event = runtime.Frame(backend, {thread, 1, impl->tick, true}, send, log);
    // A give waits while an event runs or a guard pointer is dead; say so now and then.
    if (event == ApplyEvent::Blocked && runtime.Pending() && runtime.LastGrant() &&
        runtime.LastGrant()->kind == Kind::Item && impl->tick - impl->item_waiting_logged >= 600) {
        impl->item_waiting_logged = impl->tick;
        log(fmt::format("OoT co-op: item give waiting name={} (event running, guard null or "
                        "not callable)", runtime.LastGrant()->name));
    }
    // Same-chest race: a held local give goes out once its event has ended, tagged with the chest.
    if (!impl->held_gives.empty()) {
        const u64 now_event = EventContext(memory).value_or(0);
        std::map<u64, World::Origin> ended;
        for (auto& held : impl->held_gives) {
            if (held.event == now_event && impl->tick - held.tick < 1800) continue;
            if (!ended.contains(held.event)) ended[held.event] = impl->claims.End(held.event);
            held.change.source = ended[held.event];
            log(fmt::format("OoT co-op: item give sent after its event name={} chest={:08x}", held.change.name,
                            held.change.source.key));
            Observe(held.change);
            impl->ux.ItemShared(held.change.name, held.change.value);
            held.event = ~u64{0}; // sent
        }
        std::erase_if(impl->held_gives, [](const auto& h) { return h.event == ~u64{0}; });
    }
    // Class dungeon: watch every dungeon's small-key pair; send a local gain as key_add.
    if (runtime.BaselineReady() && impl->allowed_classes.contains(Kind::Key) &&
        !runtime.Calling()) {
        // A real pickup gains one key in one dungeon. Several dungeons at once, or more than two keys
        // in a frame, is a save load or a reset seen in place: re-baseline, never forward.
        std::vector<std::pair<u32, s64>> gains;
        for (const auto& pair : impl->dungeon_keys) {
            auto& watch = impl->key_watch[pair.total];
            const auto total = backend.ReadInt(pair.total), count = backend.ReadInt(pair.count);
            const bool first = !watch.total;
            const s64 gain = watch.Step(total, count);
            if (first && watch.total && impl->key_lines < 64) {
                ++impl->key_lines;
                log(fmt::format("OoT co-op: key baseline total={:08x}={} count={:08x}={}", pair.total,
                                *watch.total, pair.count, *watch.count));
            }
            if (gain > 0) gains.emplace_back(pair.total, gain);
        }
        const bool bulk = gains.size() > 1 ||
                          std::any_of(gains.begin(), gains.end(), [](const auto& g) { return g.second > 2; });
        for (const auto& [total_key, gain] : gains) {
            const auto& watch = impl->key_watch[total_key];
            if (impl->key_lines < 64) {
                ++impl->key_lines;
                log(fmt::format("OoT co-op: key gain {} total={:08x} now {} count {} +{}",
                                bulk ? "not forwarded (bulk change: a load, not a pickup)" : "observed",
                                total_key, watch.total.value_or(0), watch.count.value_or(0), gain));
            }
            if (!bulk && KeyGain(gain)) Observe(Change{Kind::Key, {}, 0, total_key, gain});
        }
    }
    if (runtime.BaselineReady()) {
        std::scoped_lock lock{impl->mutex};
        if (!impl->baseline_ready && (!profile->world.capacity_hash ||
            !impl->allowed_classes.contains(Kind::Rupee) || impl->wallet)) {
            impl->baseline_ready = true;
            log("OoT co-op: baseline exported");
        }
    }
    if (impl->pose_enabled) FramePose(backend, memory, log, runtime.Calling());
    return runtime.Calling() || (impl->ghost && impl->ghost->Calling());
}

template <typename Backend, typename Log>
void Session::FramePose(Backend& backend, Memory::Memory& memory, Log log, bool grant_calling) {
    // Own pose: pure reads, paced by the sender; the network thread sends the newest one.
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
    const auto local = ReadLocalPose(memory, base, impl->binding.manager, profile->ghost,
                                     static_cast<u32>(impl->tick));
    std::vector<PeerView> peers;
    {
        std::scoped_lock lock{impl->mutex};
        if (auto out = impl->sender.Sample(local, now)) {
            impl->pose_out = std::move(out);
            if (impl->poses_out++ < 4 || impl->poses_out % 900 == 0) {
                log(fmt::format("OoT co-op: pose out #{} banc={} child={} loading={} pos=({:.2f}, {:.2f}, "
                                "{:.2f}) yaw={:.1f}", impl->poses_out, impl->pose_out->banc.empty() ? "-" : impl->pose_out->banc,
                                impl->pose_out->child, impl->pose_out->loading, impl->pose_out->pos[0],
                                impl->pose_out->pos[1], impl->pose_out->pos[2], impl->pose_out->yaw));
            }
        }
        for (const auto& [origin, track] : impl->peers) peers.push_back({origin, track.At(now)});
    }
    {
        std::vector<Ux::TagPeer> tag_peers;
        for (const auto& peer : peers) {
            if (!peer.pose) continue;
            tag_peers.push_back({peer.origin, peer.pose->banc, peer.pose->loading, peer.pose->child,
                                 peer.pose->pos}); // name: the origin, resolved by the session
        }
        impl->ux.Frame(memory, base, local.banc, local.loading, std::move(tag_peers), log);
    }
    // Grants keep priority: the one call slot of this frame is theirs when they used it.
    if (grant_calling) return;
    Impl::GhostPort port{backend, memory, *impl};
    impl->ghost->Frame(port, impl->binding.sentinel + GhostBlock, local, peers, log);
}

void BeginSession() {
    std::shared_ptr<Session> old;
    {
        std::scoped_lock lock{sessions_mutex};
        old = std::exchange(current_session, nullptr);
    }
    if (old) old->Stop();
    Ux::Reset();
}

void RegisterMain(Kernel::KernelCore& kernel, Kernel::KProcess* owner, u64 program_id, u64 base,
                  std::string_view build_id) {
    BeginSession();
    const auto endpoint_text = Env("EDEN_NX2_OOT_COOP") == "1"
        ? Env("EDEN_NX2_OOT_COOP_RELAY", "127.0.0.1:43390") : Env("EDEN_NX2_OOT_COOP");
    if (!Gate(endpoint_text, program_id) || base == 0) {
        if (!endpoint_text.empty()) LOG_INFO(Loader, "OoT co-op off: endpoint/title gate");
        return;
    }
    std::string why;
    auto profile = LoadProfile(build_id, &why);
    if (!profile) {
        LOG_INFO(Loader, "OoT co-op off: {}", why);
        return;
    }
    try {
        std::set<u64> denied;
        const auto path = Env("EDEN_NX2_OOT_COOP_DENY_LIST");
        if (!path.empty()) {
            std::ifstream in{std::filesystem::path{path}};
            if (!in) throw std::runtime_error{"cannot read deny-list"};
            std::string text;
            char c;
            while (in.get(c) && text.size() < MaxFrame) text.push_back(c);
            const auto parsed = DecodeLine("{\"deny\":" + text + "}");
            const auto list = parsed ? DecodeDenyList(parsed->at("deny")) : std::nullopt;
            if (!list) throw std::runtime_error{"invalid deny-list"};
            denied = *list;
        }
        const auto allowed = DecodeAllowClasses(Env("EDEN_NX2_OOT_COOP_ALLOW"));
        if (!allowed) throw std::runtime_error{"invalid grant class allow-list"};
        std::set<std::pair<Kind, u64>> keys;
        std::vector<DungeonKeys> dungeon_keys;
        const auto keys_path = Env("EDEN_NX2_OOT_COOP_KEYS");
        if (!keys_path.empty()) {
            std::ifstream in{std::filesystem::path{keys_path}};
            if (!in) throw std::runtime_error{"cannot read key allow-list"};
            std::string text;
            char c;
            while (in.get(c) && text.size() <= MaxFrame) text.push_back(c);
            const auto parsed = DecodeLine("{\"keys\":" + text + "}");
            const auto list = parsed ? DecodeKeyPolicy(parsed->at("keys")) : std::nullopt;
            if (!list) throw std::runtime_error{"invalid key allow-list"};
            keys = *list;
            const auto extras = DecodeKeyExtras(parsed->at("keys"));
            if (!extras) throw std::runtime_error{"invalid key file deny/dungeon section"};
            denied.insert(extras->deny.begin(), extras->deny.end());
            if (allowed->contains(Kind::Key)) {
                for (const u32 key : extras->dungeon_bools) keys.emplace(Kind::Bool, key);
                for (const auto& pair : extras->dungeon_keys) {
                    keys.emplace(Kind::Key, pair.total);
                    keys.emplace(Kind::Key, pair.count);
                }
                dungeon_keys = extras->dungeon_keys;
            }
            if (!extras->deny.empty() || !extras->dungeon_keys.empty()) {
                LOG_INFO(Loader, "OoT co-op: key file: {} per-player Bools denied; dungeon class {} "
                                 "({} Bools, {} key pairs)", extras->deny.size(),
                         allowed->contains(Kind::Key) ? "on" : "off", extras->dungeon_bools.size(),
                         extras->dungeon_keys.size());
            }
        }
        Hello hello;
        hello.build_id = std::string{build_id};
        hello.seed_hash = Env("EDEN_NX2_OOT_COOP_SEED", "0");
        hello.policy_id = Env("EDEN_NX2_OOT_COOP_POLICY", "or-v2");
        hello.room = Env("EDEN_NX2_OOT_COOP_ROOM", "default");
        hello.team = Env("EDEN_NX2_OOT_COOP_TEAM", "default");
        hello.name = Env("EDEN_NX2_OOT_COOP_NAME", "Player");
        std::random_device random;
        hello.client_id = fmt::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
        auto session = std::make_shared<Session>(kernel, owner, base, std::move(profile),
            *ParseEndpoint(endpoint_text),
            std::move(hello), std::move(denied), *allowed, std::move(keys));
        session->SetDungeonKeys(std::move(dungeon_keys));
        std::scoped_lock lock{sessions_mutex};
        current_session = std::move(session);
    } catch (const std::exception& error) {
        LOG_WARNING(Loader, "OoT co-op off: {}", error.what());
    }
}

std::shared_ptr<Session> FindSession(const Kernel::KProcess* owner) {
    std::scoped_lock lock{sessions_mutex};
    return current_session && current_session->owner == owner ? current_session : nullptr;
}

} // namespace Core::OotCoop
