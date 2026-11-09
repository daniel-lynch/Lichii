// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/dynarmic/oot_coop_anim.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <openssl/evp.h>

namespace Core::OotCoop::Anim {
namespace {
using Json = nlohmann::json;
using namespace std::string_view_literals;
constexpr u64 OffsetLimit = 0x10000000;

bool Text(const Json& value, std::string& out) {
    if (!value.is_string()) return false;
    out = value.get<std::string>();
    return !out.empty() && out.size() < 64 &&
           std::all_of(out.begin(), out.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
std::optional<u64> Number(const Json& value, u64 limit, bool strings = false) {
    u64 n{};
    if (value.is_number_unsigned()) n = value.get<u64>();
    else if (value.is_number_integer()) {
        const auto s = value.get<s64>();
        if (s < 0) return std::nullopt;
        n = static_cast<u64>(s);
    } else if (strings && value.is_string()) {
        auto text = value.get<std::string>();
        const bool hex = text.starts_with("0x");
        const auto* begin = text.data() + (hex ? 2 : 0);
        const auto [end, error] = std::from_chars(begin, text.data() + text.size(), n, hex ? 16 : 10);
        if (error != std::errc{} || end != text.data() + text.size() || begin == end)
            return std::nullopt;
    } else return std::nullopt;
    return n <= limit ? std::optional<u64>{n} : std::nullopt;
}
bool Float(const Json& value, float& out, float lo, float hi) {
    if (!value.is_number()) return false;
    const auto d = value.get<double>();
    if (!std::isfinite(d) || d < lo || d > hi) return false;
    out = static_cast<float>(d);
    return std::isfinite(out);
}
bool Chain(const Json& value, std::vector<u64>& out, size_t maximum = 8) {
    if (!value.is_array() || value.empty() || value.size() > maximum) return false;
    for (const auto& item : value) {
        const auto n = Number(item, OffsetLimit, true);
        if (!n) return false;
        out.push_back(*n);
    }
    return true;
}
bool Native(const Json& value, OotGameDataTrace::Entry& out) {
    if (!value.is_object() || value.size() != 2 || !value.contains("offset") ||
        !value.contains("words")) return false;
    const auto offset = Number(value.at("offset"), OffsetLimit, true);
    const auto& words = value.at("words");
    if (!offset || !*offset || (*offset & 3) || !words.is_array() || words.empty() ||
        words.size() > 8) return false;
    out.offset = *offset;
    for (const auto& word : words) {
        const auto n = Number(word, 0xFFFFFFFF, true);
        if (!n) return false;
        out.words.push_back(static_cast<u32>(*n));
    }
    return true;
}
template <typename T, size_t N>
bool Offsets(const Json& value, T& out,
             const std::array<std::pair<std::string_view, u64 T::*>, N>& fields) {
    for (const auto& [name, member] : fields) {
        const auto it = value.find(std::string{name});
        if (it == value.end()) return false;
        const auto n = Number(*it, OffsetLimit, true);
        if (!n) return false;
        out.*member = *n;
    }
    return true;
}
bool BoundedJson(std::string_view bytes) {
    unsigned depth{};
    bool quoted{}, escaped{};
    for (const char c : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 16) return false;
        } else if (c == '}' || c == ']') {
            if (!depth) return false;
            --depth;
        }
    }
    return !quoted && !depth;
}
} // namespace

bool Newer(u32 next, u32 previous) {
    const u32 delta = next - previous;
    return delta != 0 && delta < 0x80000000U;
}
bool ValidHash(std::string_view hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::optional<State> DecodeState(const Json& packet) {
    try {
        if (!packet.is_object() || !packet.contains("av") || !packet.contains("aid") ||
            !packet.contains("aseq") || !packet.contains("af") || !packet.contains("ar"))
            return std::nullopt;
        const auto revision = Number(packet.at("av"), Revision);
        const auto aid = Number(packet.at("aid"), IdLimit);
        const auto aseq = Number(packet.at("aseq"), 0xFFFFFFFF);
        State state;
        if (!revision || *revision != Revision || !aid || !aseq ||
            !Float(packet.at("ar"), state.ar, 0, 4)) return std::nullopt;
        state.aid = static_cast<u32>(*aid);
        state.aseq = static_cast<u32>(*aseq);
        if (!packet.at("af").is_null()) {
            float frame{};
            if (!Float(packet.at("af"), frame, 0, 1000000)) return std::nullopt;
            state.af = frame;
        }
        if (!state.aid && state.af) return std::nullopt;
        return state;
    } catch (...) {
        return std::nullopt;
    }
}
void EncodeState(Json& packet, const State& state) {
    packet["av"] = Revision;
    packet["aid"] = state.aid;
    packet["aseq"] = state.aseq;
    packet["af"] = state.af ? Json(*state.af) : Json(nullptr);
    packet["ar"] = state.ar;
}

std::shared_ptr<const Table> Table::Parse(std::string_view bytes, std::string* why) {
    const auto fail = [&](const char* message) -> std::shared_ptr<const Table> {
        try {
            if (why) *why = message;
        } catch (...) {
        }
        return {};
    };
    try {
        if (bytes.empty() || bytes.size() > TableByteLimit || !BoundedJson(bytes))
            return fail("animation table size/depth invalid");
        bool duplicate{};
        std::map<int, std::set<std::string>> seen;
        auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
            if (event == Json::parse_event_t::object_start) seen[depth + 1].clear();
            if (event == Json::parse_event_t::key &&
                !seen[depth].insert(value.get<std::string>()).second) duplicate = true;
            return true;
        };
        const auto document = Json::parse(bytes, callback, false);
        if (document.is_discarded() || duplicate || !document.is_object() ||
            !document.contains("schema") || Number(document.at("schema"), 1) != 1 ||
            !document.contains("unknown_id") || Number(document.at("unknown_id"), 0) != 0 ||
            !document.contains("entries")) return fail("animation table schema invalid");
        const auto& source = document.at("entries");
        if (!source.is_array() || source.empty() || source.size() > TableEntryLimit)
            return fail("animation table entry count invalid");
        auto table = std::make_shared<Table>();
        std::array<std::map<u32, std::string>, 2> names;
        for (const auto& row : source) {
            Entry entry;
            if (!row.is_object()) return fail("animation entry invalid");
            const auto id = Number(row.at("id"), IdLimit);
            const auto index = Number(row.at("command_index"), CommandLimit - 1);
            if (!id || !*id || !index || !row.at("age").is_string() ||
                (row.at("age") != "Child" && row.at("age") != "Adult") ||
                !Text(row.at("command"), entry.command) || !Text(row.at("clip"), entry.clip) ||
                !Float(row.at("frames"), entry.frames, 0.0001f, 1000000) ||
                !row.at("loop").is_boolean() || !row.at("root_zero_verified").is_boolean() ||
                Number(row.at("slot"), 0) != 0 || !row.at("end_policy").is_string())
                return fail("animation entry fields invalid");
            entry.aid = static_cast<u32>(*id);
            entry.command_index = static_cast<u32>(*index);
            entry.child = row.at("age") == "Child";
            entry.loop = row.at("loop").get<bool>();
            entry.root_zero_verified = row.at("root_zero_verified").get<bool>();
            entry.end_policy = row.at("end_policy").get<std::string>();
            if (entry.end_policy != (entry.loop ? "loop" : "hold_until_sender_transition"))
                return fail("animation end policy invalid");
            if (row.contains("phase_known")) {
                if (!row.at("phase_known").is_boolean()) return fail("phase flag invalid");
                entry.phase_known = row.at("phase_known").get<bool>();
            }
            auto [name, inserted] = names[entry.child].emplace(entry.command_index, entry.command);
            if (!inserted && name->second != entry.command) return fail("command names disagree");
            table->commands[entry.child][entry.command_index].push_back(entry.aid);
            if (!table->entries.emplace(entry.aid, std::move(entry)).second)
                return fail("duplicate animation id");
        }
        std::array<unsigned char, 32> digest{};
        unsigned int size{};
        if (!EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) ||
            size != digest.size()) return fail("animation table hash failed");
        constexpr char Hex[] = "0123456789abcdef";
        table->hash.reserve(64);
        for (const auto byte : digest) {
            table->hash.push_back(Hex[byte >> 4]);
            table->hash.push_back(Hex[byte & 15]);
        }
        if (why) *why = "animation table loaded";
        return table;
    } catch (...) {
        return fail("animation table rejected");
    }
}
const Entry* Table::Find(bool child, u32 aid) const {
    const auto it = entries.find(aid);
    return it != entries.end() && it->second.child == child ? &it->second : nullptr;
}
const Entry* Table::Map(bool child, u32 index, std::string_view name, u32 category,
                        const Keys& keys) const {
    if (index >= CommandLimit || index >= keys.cmd_table[child].size() ||
        keys.cmd_table[child][index] != name) return nullptr;
    u32 aid{};
    if (!keys.move_command.empty() && name == keys.move_command) {
        if (category != 1 && category != 2) return nullptr;
        aid = keys.move_ids[child][category - 1];
    } else if (const auto selected = keys.representatives[child].find(index);
               selected != keys.representatives[child].end()) {
        aid = selected->second;
    } else {
        const auto candidates = commands[child].find(index);
        if (candidates == commands[child].end() || candidates->second.size() != 1) return nullptr;
        aid = candidates->second.front();
    }
    const auto* entry = Find(child, aid);
    return entry && entry->command_index == index && entry->command == name &&
                   entry->root_zero_verified ? entry : nullptr;
}
std::shared_ptr<const Table> LoadTable(std::string* why) {
    try {
        std::filesystem::path path;
        if (const auto* value = std::getenv("EDEN_NX2_OOT_COOP_ANIM"); value && *value) path = value;
        else if (const auto* keys = std::getenv("EDEN_NX2_OOT_COOP_KEYS"); keys && *keys)
            path = std::filesystem::path{keys}.parent_path() / "coop-anim.json";
        std::ifstream input{path, std::ios::binary};
        if (!input) {
            if (why) *why = "animation table unavailable";
            return {};
        }
        std::string bytes;
        std::array<char, 4096> chunk{};
        while (input) {
            input.read(chunk.data(), chunk.size());
            const auto size = static_cast<size_t>(input.gcount());
            if (size > TableByteLimit - bytes.size()) {
                if (why) *why = "animation table exceeds size limit";
                return {};
            }
            bytes.append(chunk.data(), size);
        }
        if (!input.eof()) return {};
        return Table::Parse(bytes, why);
    } catch (...) {
        try {
            if (why) *why = "animation table read failed";
        } catch (...) {
        }
        return {};
    }
}

std::optional<Keys> DecodeKeys(const Json& value) {
    try {
        if (!value.is_object()) return std::nullopt;
        Keys keys;
        const std::array fields{
            std::pair{"ctl_count_off"sv, &Keys::ctl_count_off},
            std::pair{"ctl_entries_off"sv, &Keys::ctl_entries_off},
            std::pair{"ctl_stride"sv, &Keys::ctl_stride},
            std::pair{"cmd_index_off"sv, &Keys::cmd_index_off},
            std::pair{"cmd_name_off"sv, &Keys::cmd_name_off},
            std::pair{"frame_off"sv, &Keys::frame_off},
            std::pair{"rate_off"sv, &Keys::rate_off},
        };
        if (!Offsets(value, keys, fields) || !keys.ctl_stride ||
            !Chain(value.at("ctl_chain"), keys.ctl_chain) ||
            !Native(value.at("play_wrapper"), keys.play_wrapper) ||
            !Native(value.at("frame_setter"), keys.frame_setter) ||
            !Native(value.at("rate_setter"), keys.rate_setter)) return std::nullopt;
        const auto slot = Number(value.at("source_slot"), 63);
        if (!slot) return std::nullopt;
        keys.source_slot = static_cast<u32>(*slot);
        for (const auto& [name, member] : std::array{
                 std::pair{"sender_enabled", &Keys::sender_enabled},
                 std::pair{"receiver_enabled", &Keys::receiver_enabled},
                 std::pair{"restart_frame_reset", &Keys::restart_frame_reset},
                 std::pair{"endpoint_inclusive", &Keys::endpoint_inclusive}}) {
            if (value.contains(name)) {
                if (!value.at(name).is_boolean()) return std::nullopt;
                keys.*member = value.at(name).get<bool>();
            }
        }
        if (!Text(value.at("move_command"), keys.move_command)) return std::nullopt;
        for (unsigned age = 0; age < 2; ++age) {
            const auto* name = age ? "child" : "adult";
            const auto& commands = value.at("cmd_table").at(name);
            if (!commands.is_array() || commands.size() != CommandLimit) return std::nullopt;
            for (const auto& command : commands) {
                std::string text;
                if (!Text(command, text)) return std::nullopt;
                keys.cmd_table[age].push_back(std::move(text));
            }
            const auto& representatives = value.at("representatives").at(name);
            if (!representatives.is_array() || representatives.size() > CommandLimit)
                return std::nullopt;
            for (const auto& representative : representatives) {
                if (!representative.is_object() || representative.size() != 2)
                    return std::nullopt;
                const auto index = Number(representative.at("index"), CommandLimit - 1);
                const auto aid = Number(representative.at("aid"), IdLimit);
                if (!index || !aid || !*aid ||
                    !keys.representatives[age].emplace(static_cast<u32>(*index),
                                                     static_cast<u32>(*aid)).second)
                    return std::nullopt;
            }
            const auto& move = value.at("move_ids").at(name);
            if (!move.is_array() || move.size() != 2) return std::nullopt;
            for (unsigned category = 0; category < 2; ++category) {
                const auto aid = Number(move.at(category), IdLimit);
                if (!aid || !*aid) return std::nullopt;
                keys.move_ids[age][category] = static_cast<u32>(*aid);
            }
        }
        if (value.contains("phase_known_ids")) {
            const auto& known = value.at("phase_known_ids");
            if (!known.is_array() || known.size() > TableEntryLimit) return std::nullopt;
            for (const auto& id : known) {
                const auto n = Number(id, IdLimit);
                if (!n || !*n || !keys.phase_known_ids.insert(static_cast<u32>(*n)).second)
                    return std::nullopt;
            }
        }
        if (value.contains("player")) {
            auto player = ::Core::OotCoop::Player::DecodeKeys(value.at("player"));
            if (!player) return std::nullopt;
            keys.player = std::move(*player);
        }
        if (value.contains("blackboard")) {
            const auto& b = value.at("blackboard");
            BlackboardKeys board;
            const std::array board_fields{
                std::pair{"instance_off"sv, &BlackboardKeys::instance_off},
                std::pair{"int_off"sv, &BlackboardKeys::int_off},
                std::pair{"float_off"sv, &BlackboardKeys::float_off},
                std::pair{"bool_off"sv, &BlackboardKeys::bool_off},
                std::pair{"data_off"sv, &BlackboardKeys::data_off},
                std::pair{"resource_off"sv, &BlackboardKeys::resource_off},
                std::pair{"manager_off"sv, &BlackboardKeys::manager_off},
                std::pair{"blob_off"sv, &BlackboardKeys::blob_off},
                std::pair{"table_off"sv, &BlackboardKeys::table_off},
                std::pair{"pool_off"sv, &BlackboardKeys::pool_off},
                std::pair{"magic_off"sv, &BlackboardKeys::magic_off},
                std::pair{"version_off"sv, &BlackboardKeys::version_off},
                std::pair{"type_stride"sv, &BlackboardKeys::type_stride},
                std::pair{"count_off"sv, &BlackboardKeys::count_off},
                std::pair{"start_off"sv, &BlackboardKeys::start_off},
                std::pair{"names_off"sv, &BlackboardKeys::names_off},
            };
            if (!b.is_object() || !Offsets(b, board, board_fields) || !board.type_stride)
                return std::nullopt;
            const auto magic = Number(b.at("magic"), 0xFFFFFFFF, true);
            const auto version = Number(b.at("version"), 0xFFFFFFFF, true);
            const auto& fields_json = b.at("fields");
            if (!magic || !version || !fields_json.is_array() || fields_json.size() > 128)
                return std::nullopt;
            board.magic = static_cast<u32>(*magic);
            board.version = static_cast<u32>(*version);
            std::set<std::pair<u32, u32>> unique;
            for (const auto& field : fields_json) {
                BlackboardField out;
                const auto type = Number(field.at("type"), 4);
                const auto index = Number(field.at("index"), 4095);
                if (!field.is_object() || field.size() != 3 || !type ||
                    (*type != 1 && *type != 3 && *type != 4) || !index ||
                    !Text(field.at("name"), out.name)) return std::nullopt;
                out.type = static_cast<u32>(*type);
                out.index = static_cast<u32>(*index);
                if (!unique.emplace(out.type, out.index).second) return std::nullopt;
                board.fields.push_back(std::move(out));
            }
            for (const auto& [name, ignored] : b.items()) {
                (void)ignored;
                if (std::any_of(board_fields.begin(), board_fields.end(),
                                [&](const auto& f) { return f.first == name; })) continue;
                if (name != "magic" && name != "version" && name != "fields") return std::nullopt;
            }
            keys.blackboard = std::move(board);
        }
        for (const auto& [name, ignored] : value.items()) {
            (void)ignored;
            if (std::any_of(fields.begin(), fields.end(),
                            [&](const auto& f) { return f.first == name; })) continue;
            if (name != "ctl_chain" && name != "play_wrapper" && name != "frame_setter" &&
                name != "rate_setter" && name != "source_slot" && name != "sender_enabled" &&
                name != "receiver_enabled" && name != "restart_frame_reset" &&
                name != "endpoint_inclusive" && name != "cmd_table" && name != "representatives" &&
                name != "move_ids" && name != "move_command" && name != "phase_known_ids" &&
                name != "player" && name != "blackboard") return std::nullopt;
        }
        if (keys.sender_enabled && !keys.player) return std::nullopt;
        return keys;
    } catch (...) {
        return std::nullopt;
    }
}

State Sender::Sample(const Table& table, const Keys& keys, const Snapshot& snapshot, bool child,
                     u32 category, std::optional<u64> successful_start) {
    if (snapshot.controllers.size() <= keys.source_slot) return State{0, last.aseq, {}, 0};
    const auto& controller = snapshot.controllers[keys.source_slot];
    const auto* entry = table.Map(child, controller.index, controller.name, category, keys);
    State state{entry ? entry->aid : 0, last.aseq, {}, controller.rate};
    if (!std::isfinite(controller.rate) || controller.rate < 0 || controller.rate > 4 ||
        !std::isfinite(controller.frame) || controller.frame < 0) state = {0, last.aseq, {}, 0};
    if (entry && state.aid && (entry->phase_known || keys.phase_known_ids.contains(entry->aid)) &&
        controller.frame <= entry->frames) state.af = controller.frame;
    const bool changed = std::exchange(transition, false) || !previous || actor != snapshot.actor ||
                         age != child ||
                         previous->address != controller.address || previous->index != controller.index ||
                         previous->name != controller.name || last.aid != state.aid;
    const bool observed_restart = successful_start && start && *successful_start != *start;
    const bool inferred_restart = keys.restart_frame_reset && entry && !entry->loop && previous &&
                                  controller.frame < previous->frame && controller.frame <= 1.0f;
    if (changed || observed_restart || inferred_restart) ++state.aseq;
    previous = controller;
    actor = snapshot.actor;
    age = child;
    start = successful_start;
    last = state;
    return state;
}
size_t CommandWatch::Observe(const Snapshot& snapshot, const Player& player) {
    if (!owner || *owner != player) {
        previous.clear();
        owner = player;
    }
    size_t fired{};
    for (size_t slot = 0; slot < snapshot.controllers.size(); ++slot) {
        const auto& next = snapshot.controllers[slot];
        const Controller* before = slot < previous.size() ? &previous[slot] : nullptr;
        if (before && before->address == next.address && before->index == next.index &&
            before->name == next.name) continue;
        const CommandChange change{static_cast<u32>(slot),
                                   before ? std::optional<Controller>{*before} : std::nullopt, next};
        for (const auto& subscriber : subscribers) {
            try {
                subscriber(change, snapshot, player);
            } catch (...) {
                // A consumer's failure never reaches the frame hook or the other consumers.
            }
        }
        ++fired;
    }
    previous = snapshot.controllers;
    return fired;
}

void Sender::Reset() {
    const u32 sequence = last.aseq;
    *this = {};
    last.aseq = sequence;
}

bool Receiver::Queue(const State& state, u32 incoming_tick, const Lifetime& lifetime,
                     const Table& table, bool hash_matches) {
    if (!hash_matches || !lifetime.actor || lifetime.session.empty() || lifetime.scene.empty()) {
        Reset();
        return false;
    }
    if (owner && *owner != lifetime) Reset();
    if (tick && !Newer(incoming_tick, *tick)) return false;
    const auto* entry = table.Find(lifetime.child, state.aid);
    if (!entry || !entry->root_zero_verified || !std::isfinite(state.ar) || state.ar < 0 ||
        state.ar > 4 || (state.af && (!std::isfinite(*state.af) || *state.af < 0 ||
                                     *state.af > entry->frames))) {
        // A current unavailable ID cancels the native path; the owner selects its legacy fallback.
        Reset();
        return false;
    }
    const State* latest = pending ? &pending->state : current ? &current->state :
                         committed ? &*committed : nullptr;
    if (latest && state.aseq != latest->aseq && !Newer(state.aseq, latest->aseq)) return false;
    owner = lifetime;
    tick = incoming_tick;
    pending = Target{state, *entry};
    return true;
}
std::optional<Operation> Receiver::Next(u64 now, const Lifetime& lifetime) {
    if (!owner || *owner != lifetime) {
        Reset();
        return std::nullopt;
    }
    if (in_flight) return std::nullopt;
    if (pending) {
        if (!current || !SameTransition(current->state, pending->state)) {
            current = std::move(pending);
            pending.reset();
            holding = false;
            observed.reset();
            stage = committed && SameTransition(*committed, current->state) ? Stage::Rate : Stage::Play;
            retry_at = 0;
            retries = 0;
        } else {
            current->state.ar = holding ? 0 : pending->state.ar;
            // Preserve the selected phase until the frozen startup sequence has applied it.
            pending.reset();
        }
    }
    if (!current || now < retry_at || retries >= RetryLimit) return std::nullopt;
    if (stage == Stage::Rate && committed && SameTransition(*committed, current->state) &&
        committed->ar == current->state.ar) return std::nullopt;
    in_flight = Operation{stage, current->state, current->entry, lifetime, ++serial};
    return in_flight;
}
bool Receiver::Complete(const Operation& operation, bool success, u64 now,
                        const Lifetime& lifetime) {
    if (!in_flight || in_flight->serial != operation.serial ||
        operation.lifetime != lifetime || !owner || *owner != lifetime) return false;
    in_flight.reset();
    if (!success) {
        ++retries;
        retry_at = now + RetryMs;
        return false;
    }
    retries = 0;
    retry_at = 0;
    // New transitions must start from their own play, even if the old call just returned.
    if (pending && !SameTransition(pending->state, operation.state)) {
        current.reset();
        return true;
    }
    switch (operation.stage) {
    case Stage::Play: stage = Stage::Frame; break;
    case Stage::Frame: stage = Stage::Rate; break;
    case Stage::Rate: {
        const bool rate_only = committed && SameTransition(*committed, operation.state);
        // Commit precisely the arguments that returned; a newer rate still needs its own call.
        committed = operation.state;
        stage = rate_only ? Stage::Rate : Stage::Anchor;
        break;
    }
    case Stage::Anchor:
        stage = Stage::Rate;
        break;
    }
    return true;
}
void Receiver::ObserveFrame(float frame, bool endpoint_inclusive) {
    if (!current || !committed || !SameTransition(current->state, *committed) ||
        current->entry.loop || holding || in_flight || stage != Stage::Rate ||
        !std::isfinite(frame) || frame < 0)
        return;
    const float last_frame = endpoint_inclusive ? current->entry.frames :
                                               std::nextafter(current->entry.frames, 0.0f);
    // A frame that went backwards while playing forward means the raw clip wrapped past its end
    // between two reads (a native loop): clamp it back to the end, as if the end had been seen.
    const bool wrapped = observed && committed->ar > 0 && frame < *observed;
    observed = frame;
    if (frame < last_frame && !wrapped) return;
    holding = true;
    observed.reset();
    current->state.af = last_frame;
    current->state.ar = 0;
    if (pending && SameTransition(pending->state, current->state)) pending->state.ar = 0;
    stage = Stage::Frame;
}
void Receiver::Reset() {
    // Serial numbers outlive resets so a delayed completion cannot match a newer operation.
    const auto sequence = serial;
    *this = {};
    serial = sequence;
}

std::array<u8, 0x30> BuildPlayRequest(u64 actor, u64 c_string) {
    std::array<u8, 0x30> bytes{};
    const auto put = [&](size_t at, u64 value, size_t count) {
        for (size_t i = 0; i < count; ++i) bytes[at + i] = static_cast<u8>(value >> (i * 8));
    };
    put(0, actor, 8);
    put(8, c_string, 8);
    put(0x18, std::bit_cast<u32>(-1.0f), 4);
    put(0x1C, std::bit_cast<u32>(-1.0f), 4);
    return bytes;
}

} // namespace Core::OotCoop::Anim
