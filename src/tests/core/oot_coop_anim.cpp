// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_anim.h"

namespace {
namespace Anim = Core::OotCoop::Anim;
using Json = nlohmann::json;
Json Row(u32 id, u32 index, const char* command, const char* clip, bool loop = false,
         bool child = true) {
    return {{"id", id}, {"command_index", index}, {"command", command}, {"clip", clip},
            {"age", child ? "Child" : "Adult"}, {"frames", 20}, {"loop", loop},
            {"slot", 0}, {"root_zero_verified", true},
            {"end_policy", loop ? "loop" : "hold_until_sender_transition"}};
}
Json Document() {
    return {{"schema", 1}, {"unknown_id", 0}, {"entries", Json::array({
        Row(1, 0, "SyntheticIdle", "SyntheticClipA", true),
        Row(2, 1, "SyntheticAction", "SyntheticClipB"),
        Row(3, 2, "SyntheticLocomotion", "SyntheticSlow", true),
        Row(4, 2, "SyntheticLocomotion", "SyntheticFast", true),
        Row(5, 0, "SyntheticIdle", "SyntheticAdult", true, false)})}};
}
Anim::Keys MappingKeys() {
    Anim::Keys keys;
    keys.cmd_table[1] = {"SyntheticIdle", "SyntheticAction", "SyntheticLocomotion"};
    keys.cmd_table[0] = {"SyntheticIdle"};
    keys.move_command = "SyntheticLocomotion";
    keys.move_ids[1] = {3, 4};
    keys.representatives[1][2] = 4;
    return keys;
}
Anim::Lifetime Life() { return {"synthetic-session", "synthetic-scene", 256, 7, 2, true}; }
Anim::State State(u32 id = 2, u32 sequence = 1, float rate = 1) {
    return {id, sequence, {}, rate};
}
void Commit(Anim::Receiver& receiver, const Anim::Lifetime& lifetime, u64 now = 1) {
    for (const auto stage : {Anim::Stage::Play, Anim::Stage::Frame, Anim::Stage::Rate,
                             Anim::Stage::Anchor}) {
        const auto operation = receiver.Next(now, lifetime);
        REQUIRE(operation);
        REQUIRE(operation->stage == stage);
        REQUIRE(receiver.Complete(*operation, true, now, lifetime));
    }
}
struct Memory {
    std::array<u8, 8192> bytes{};
    bool IsValidVirtualAddressRange(u64 address, u64 size) const {
        return address && address <= bytes.size() && size <= bytes.size() - address;
    }
    u8 Read8(u64 address) const { return bytes.at(address); }
    u16 Read16(u64 address) const {
        return static_cast<u16>(Read8(address) | (u16{Read8(address + 1)} << 8));
    }
    u32 Read32(u64 address) const {
        return u32{Read16(address)} | (u32{Read16(address + 2)} << 16);
    }
    u64 Read64(u64 address) const {
        return u64{Read32(address)} | (u64{Read32(address + 4)} << 32);
    }
    void Put(u64 address, u64 value, size_t size = 8) {
        for (size_t i = 0; i < size; ++i) bytes.at(address + i) = static_cast<u8>(value >> (8 * i));
    }
    void Float(u64 address, float value) { Put(address, std::bit_cast<u32>(value), 4); }
    void String(u64 address, const char* text) {
        std::memcpy(bytes.data() + address, text, std::strlen(text) + 1);
    }
};
Anim::Keys MemoryKeys(Memory& memory) {
    auto keys = MappingKeys();
    keys.sender_enabled = true;
    keys.ctl_chain = {8, 16};
    keys.ctl_count_off = 4;
    keys.ctl_entries_off = 16;
    keys.ctl_stride = 32;
    keys.cmd_index_off = 24;
    keys.cmd_name_off = 8;
    keys.frame_off = 32;
    keys.rate_off = 36;
    memory.Put(256 + 8, 512);
    memory.Put(512 + 16, 768);
    memory.Put(768 + 4, 1, 4);
    memory.Put(768 + 16, 1024);
    memory.Put(1024, 1280);
    memory.Put(1280 + 24, 1, 2);
    memory.Put(1280 + 8, 1536);
    memory.String(1536, "SyntheticAction");
    memory.Float(1280 + 32, 7.25f);
    memory.Float(1280 + 36, 1.25f);
    return keys;
}
} // namespace

TEST_CASE("OoT animation table hashes exact bytes and enforces bounds", "[core][oot_coop][oot_coop_anim]") {
    const auto bytes = Document().dump();
    const auto table = Anim::Table::Parse(bytes);
    REQUIRE(table);
    REQUIRE(table->Size() == 5);
    REQUIRE(Anim::ValidHash(table->Hash()));
    const auto padded = Anim::Table::Parse(bytes + "\n");
    REQUIRE(padded);
    REQUIRE(padded->Hash() != table->Hash());
    REQUIRE(table->Find(true, 2)->clip == "SyntheticClipB");
    REQUIRE_FALSE(table->Find(false, 2));
    REQUIRE_FALSE(table->Find(true, 0));
    REQUIRE_FALSE(table->Find(true, Anim::IdLimit));
    REQUIRE_FALSE(Anim::Table::Parse(std::string(Anim::TableByteLimit + 1, ' ')));
    REQUIRE_FALSE(Anim::Table::Parse(std::string(50, '[') + std::string(50, ']')));
    REQUIRE_FALSE(Anim::Table::Parse("{\"schema\":1,\"schema\":1}"));
    for (const auto& bad_id : {Json(0), Json(-1), Json(1.0), Json(true), Json(u64{Anim::IdLimit} + 1)}) {
        auto bad = Document();
        bad["entries"][0]["id"] = bad_id;
        REQUIRE_FALSE(Anim::Table::Parse(bad.dump()));
    }
    auto bad = Document();
    bad["entries"][1]["id"] = 1;
    REQUIRE_FALSE(Anim::Table::Parse(bad.dump()));
    bad = Document();
    bad["entries"][0]["clip"] = std::string(64, 'x');
    REQUIRE_FALSE(Anim::Table::Parse(bad.dump()));
    bad = Document();
    bad["entries"][0]["frames"] = 0;
    REQUIRE_FALSE(Anim::Table::Parse(bad.dump()));
    bad = Document();
    bad["entries"][0]["command_index"] = Anim::CommandLimit;
    REQUIRE_FALSE(Anim::Table::Parse(bad.dump()));
}

TEST_CASE("OoT animation mapping retains speed selection and rejects ambiguity", "[core][oot_coop][oot_coop_anim]") {
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    auto keys = MappingKeys();
    REQUIRE(table->Map(true, 2, "SyntheticLocomotion", 1, keys)->aid == 3);
    REQUIRE(table->Map(true, 2, "SyntheticLocomotion", 2, keys)->aid == 4);
    REQUIRE_FALSE(table->Map(true, 2, "SyntheticLocomotion", 0, keys));
    REQUIRE_FALSE(table->Map(true, 1, "SyntheticIdle", 0, keys));
    REQUIRE_FALSE(table->Map(false, 1, "SyntheticAction", 0, keys));
    REQUIRE_FALSE(table->Map(true, Anim::CommandLimit, "SyntheticAction", 0, keys));
    keys.move_command.clear();
    keys.representatives[1].clear();
    REQUIRE_FALSE(table->Map(true, 2, "SyntheticLocomotion", 2, keys));
    REQUIRE(table->Map(true, 1, "SyntheticAction", 0, keys)->aid == 2);
}

TEST_CASE("OoT animation codec rejects nonfinite and noninteger fields", "[core][oot_coop][oot_coop_anim]") {
    Json packet;
    auto state = State();
    Anim::EncodeState(packet, state);
    REQUIRE(packet["af"].is_null());
    REQUIRE(Anim::DecodeState(packet) == state);
    state.af = 4.125f;
    Anim::EncodeState(packet, state);
    REQUIRE(packet["af"].is_number());
    REQUIRE(Anim::DecodeState(packet) == state);
    for (const auto* field : {"av", "aid", "aseq"}) {
        for (const auto& bad : {Json(-1), Json(1.5), Json(true)}) {
            auto invalid = packet;
            invalid[field] = bad;
            REQUIRE_FALSE(Anim::DecodeState(invalid));
        }
    }
    for (const auto* field : {"af", "ar"}) {
        for (const auto bad : {std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(), -1.0}) {
            auto invalid = packet;
            invalid[field] = bad;
            REQUIRE_FALSE(Anim::DecodeState(invalid));
        }
    }
    packet["av"] = 2;
    REQUIRE_FALSE(Anim::DecodeState(packet));
    packet["av"] = 1;
    packet["ar"] = 0;
    REQUIRE(Anim::DecodeState(packet)->ar == 0);
    packet["aid"] = 0;
    REQUIRE_FALSE(Anim::DecodeState(packet));
}

TEST_CASE("OoT animation sender snapshot validates controller identity", "[core][oot_coop][oot_coop_anim]") {
    Memory memory;
    auto keys = MemoryKeys(memory);
    auto read = [&] { return Anim::ReadSnapshot(memory, 256, true, keys, [] { return true; }); };
    const auto snapshot = read();
    REQUIRE(snapshot);
    REQUIRE(snapshot->controllers.size() == 1);
    REQUIRE(snapshot->controllers[0].frame == 7.25f);
    REQUIRE(snapshot->controllers[0].rate == 1.25f);
    REQUIRE(Anim::ReadControllerFrame(memory, 256, keys) == 7.25f);
    memory.Put(1280 + 24, Anim::CommandLimit, 2);
    REQUIRE_FALSE(read());
    memory.Put(1280 + 24, 0, 2);
    REQUIRE_FALSE(read());
    memory.Put(1280 + 24, 1, 2);
    memory.Float(1280 + 32, std::numeric_limits<float>::quiet_NaN());
    REQUIRE_FALSE(read());
    memory.Float(1280 + 32, 1);
    memory.Float(1280 + 36, 4.1f);
    REQUIRE_FALSE(read());
    memory.Float(1280 + 36, 1);
    unsigned checks{};
    REQUIRE_FALSE(Anim::ReadSnapshot(memory, 256, true, keys, [&] { return ++checks == 1; }));
    // A second controller without a normal command (fallback sentinel) does not veto the
    // source slot; it is carried as unknown. As the source slot itself it fails the snapshot.
    memory.Put(768 + 4, 2, 4);
    memory.Put(1024 + 32, 1792);
    memory.Put(1792 + 24, 0xFFFE, 2);
    memory.Put(1792 + 8, 1536);
    const auto partial = read();
    REQUIRE(partial);
    REQUIRE(partial->controllers.size() == 2);
    REQUIRE(partial->controllers[0].index == 1);
    REQUIRE(partial->controllers[1].index == Anim::UnknownCommand);
    keys.source_slot = 1;
    REQUIRE_FALSE(read());
    keys.source_slot = 0;
    memory.Put(1024 + 32, 0); // an empty entry pointer is not a controller
    REQUIRE_FALSE(read());
    memory.Put(768 + 4, 65, 4);
    REQUIRE_FALSE(read());
    const Anim::Reader reader{memory};
    REQUIRE_FALSE(reader.Int(std::numeric_limits<u64>::max(), 8, 4));
}

TEST_CASE("OoT animation sender phase and restart evidence are explicit", "[core][oot_coop][oot_coop_anim]") {
    Memory memory;
    auto keys = MemoryKeys(memory);
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    auto snapshot = Anim::ReadSnapshot(memory, 256, true, keys, [] { return true; });
    REQUIRE(snapshot);
    Anim::Sender sender;
    const auto initial = sender.Sample(*table, keys, *snapshot, true, 0, 100);
    REQUIRE(initial.aid == 2);
    REQUIRE_FALSE(initial.af);
    REQUIRE(sender.Sample(*table, keys, *snapshot, true, 0, 100).aseq == initial.aseq);
    snapshot->controllers[0].frame = 0;
    REQUIRE(sender.Sample(*table, keys, *snapshot, true, 0, 100).aseq == initial.aseq);
    const auto restart = sender.Sample(*table, keys, *snapshot, true, 0, 101);
    REQUIRE(restart.aseq == initial.aseq + 1);
    keys.phase_known_ids.insert(2);
    REQUIRE(sender.Sample(*table, keys, *snapshot, true, 0, 101).af == 0);
    snapshot->controllers[0].frame = 21;
    REQUIRE_FALSE(sender.Sample(*table, keys, *snapshot, true, 0, 101).af);
}

TEST_CASE("OoT animation receiver restarts sequences and coalesces in flight", "[core][oot_coop][oot_coop_anim]") {
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    const auto life = Life();
    Anim::Receiver receiver;
    REQUIRE(receiver.Queue(State(), 1, life, *table, true));
    const auto first = receiver.Next(1, life);
    REQUIRE(first);
    REQUIRE(first->stage == Anim::Stage::Play);
    REQUIRE_FALSE(receiver.Next(2, life));
    REQUIRE(receiver.Queue(State(2, 2), 2, life, *table, true));
    REQUIRE(receiver.Queue(State(2, 3), 3, life, *table, true));
    REQUIRE(receiver.Complete(*first, true, 3, life));
    REQUIRE_FALSE(receiver.Committed());
    Commit(receiver, life, 4);
    REQUIRE(receiver.Committed()->aseq == 3);
    REQUIRE_FALSE(receiver.Queue(State(2, 3), 3, life, *table, true));
    REQUIRE_FALSE(receiver.Queue(State(2, 2), 4, life, *table, true));
    REQUIRE(receiver.Queue(State(2, 4), 5, life, *table, true));
    Commit(receiver, life, 5);
    REQUIRE(receiver.Committed()->aseq == 4);
}

TEST_CASE("OoT animation receiver applies rates without replay or premature commit", "[core][oot_coop][oot_coop_anim]") {
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    const auto life = Life();
    Anim::Receiver receiver;
    REQUIRE(receiver.Queue(State(), 1, life, *table, true));
    Commit(receiver, life);
    REQUIRE(receiver.Queue(State(2, 1, 0.5f), 2, life, *table, true));
    auto operation = receiver.Next(2, life);
    REQUIRE(operation);
    REQUIRE(operation->stage == Anim::Stage::Rate);
    REQUIRE(receiver.Queue(State(2, 1, 1.5f), 3, life, *table, true));
    REQUIRE(receiver.Complete(*operation, true, 3, life));
    REQUIRE(receiver.Committed()->ar == 0.5f);
    operation = receiver.Next(4, life);
    REQUIRE(operation->stage == Anim::Stage::Rate);
    REQUIRE(operation->state.ar == 1.5f);
}

TEST_CASE("OoT animation one shots clamp and hold until a sender transition", "[core][oot_coop][oot_coop_anim]") {
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    const auto life = Life();
    Anim::Receiver receiver;
    REQUIRE(receiver.Queue(State(), 1, life, *table, true));
    Commit(receiver, life);
    receiver.ObserveFrame(21);
    auto operation = receiver.Next(2, life);
    REQUIRE(operation->stage == Anim::Stage::Frame);
    REQUIRE(operation->state.af == std::nextafter(20.0f, 0.0f));
    REQUIRE(receiver.Complete(*operation, true, 2, life));
    operation = receiver.Next(2, life);
    REQUIRE(operation->stage == Anim::Stage::Rate);
    REQUIRE(operation->state.ar == 0);
    REQUIRE(receiver.Complete(*operation, true, 2, life));
    REQUIRE_FALSE(receiver.Next(2, life));
    REQUIRE(receiver.Queue(State(), 3, life, *table, true));
    REQUIRE_FALSE(receiver.Next(3, life));
    REQUIRE(receiver.Committed()->ar == 0);
    REQUIRE(receiver.Queue(State(2, 2), 4, life, *table, true));
    REQUIRE(receiver.Next(4, life)->stage == Anim::Stage::Play);
}

TEST_CASE("OoT animation one shot that wraps natively between reads still holds", "[core][oot_coop][oot_coop_anim]") {
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    const auto life = Life();
    Anim::Receiver receiver;
    REQUIRE(receiver.Queue(State(), 1, life, *table, true));
    Commit(receiver, life);
    receiver.ObserveFrame(18.5f);
    REQUIRE_FALSE(receiver.Next(2, life));
    receiver.ObserveFrame(19.5f);
    REQUIRE_FALSE(receiver.Next(2, life));
    receiver.ObserveFrame(0.5f); // went past the end and wrapped: never seen at the end
    auto operation = receiver.Next(3, life);
    REQUIRE(operation);
    REQUIRE(operation->stage == Anim::Stage::Frame);
    REQUIRE(operation->state.af == std::nextafter(20.0f, 0.0f));
    REQUIRE(receiver.Complete(*operation, true, 3, life));
    operation = receiver.Next(3, life);
    REQUIRE(operation->stage == Anim::Stage::Rate);
    REQUIRE(operation->state.ar == 0);

    // A looping entry wraps without a hold; a frozen one-shot never counts as wrapped.
    Anim::Receiver looping;
    REQUIRE(looping.Queue(State(1, 1), 1, life, *table, true));
    Commit(looping, life);
    looping.ObserveFrame(19.5f);
    looping.ObserveFrame(0.5f);
    REQUIRE_FALSE(looping.Next(2, life));
    Anim::Receiver frozen;
    REQUIRE(frozen.Queue(State(2, 1, 0), 1, life, *table, true));
    Commit(frozen, life);
    frozen.ObserveFrame(5);
    frozen.ObserveFrame(4); // a seek by the sender's frame, not a wrap
    REQUIRE_FALSE(frozen.Next(2, life));
}

TEST_CASE("OoT animation exhausted retries yield to the legacy category until a new transition",
          "[core][oot_coop][oot_coop_anim]") {
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    const auto life = Life();
    Anim::Receiver receiver;
    REQUIRE(receiver.Queue(State(), 1, life, *table, true));
    REQUIRE_FALSE(receiver.Exhausted());
    u64 now = 1;
    for (u32 i = 0; i < Anim::Receiver::RetryLimit; ++i) {
        const auto operation = receiver.Next(now, life);
        REQUIRE(operation);
        REQUIRE_FALSE(receiver.Exhausted()); // in flight
        REQUIRE_FALSE(receiver.Complete(*operation, false, now, life));
        now += Anim::Receiver::RetryMs;
    }
    REQUIRE(receiver.Exhausted());
    REQUIRE_FALSE(receiver.Next(now, life));
    REQUIRE(receiver.Queue(State(2, 1, 0.5f), 2, life, *table, true)); // same transition
    REQUIRE_FALSE(receiver.Next(now, life));
    REQUIRE(receiver.Exhausted());
    REQUIRE(receiver.Queue(State(2, 2), 3, life, *table, true)); // the sender moved on
    REQUIRE_FALSE(receiver.Exhausted());
    const auto retry = receiver.Next(now, life);
    REQUIRE(retry);
    REQUIRE(retry->stage == Anim::Stage::Play);
    REQUIRE(retry->state.aseq == 2);
}

TEST_CASE("OoT animation receiver rejects generations and limits retries", "[core][oot_coop][oot_coop_anim]") {
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    auto life = Life();
    Anim::Receiver receiver;
    REQUIRE_FALSE(receiver.Queue(State(), 1, life, *table, false));
    REQUIRE_FALSE(receiver.Queue(State(5), 1, life, *table, true));
    auto out_of_range = State();
    out_of_range.af = 21;
    REQUIRE_FALSE(receiver.Queue(out_of_range, 1, life, *table, true));
    REQUIRE(receiver.Queue(State(), 1, life, *table, true));
    const auto old = receiver.Next(1, life);
    REQUIRE(old);
    ++life.generation;
    REQUIRE_FALSE(receiver.Complete(*old, true, 1, life));
    REQUIRE_FALSE(receiver.Next(1, life));
    REQUIRE(receiver.Queue(State(), 1, life, *table, true));
    for (u64 now : {1, 251, 501}) {
        const auto operation = receiver.Next(now, life);
        REQUIRE(operation);
        REQUIRE_FALSE(receiver.Complete(*operation, false, now, life));
        REQUIRE_FALSE(receiver.Next(now + 1, life));
    }
    REQUIRE_FALSE(receiver.Next(10000, life));
    REQUIRE(receiver.Queue(State(2, 2), 2, life, *table, true));
    REQUIRE(receiver.Next(10001, life));
    REQUIRE_FALSE(receiver.Complete(*old, true, 10001, life));
    REQUIRE(Anim::Newer(0, 0xFFFFFFFF));
    REQUIRE_FALSE(Anim::Newer(0xFFFFFFFF, 0));
    REQUIRE_FALSE(Anim::Newer(0x80000000, 0));
}

TEST_CASE("OoT animation native request has exact bytes and a direct string pointer", "[core][oot_coop][oot_coop_anim]") {
    const auto request = Anim::BuildPlayRequest(0x0102030405060708ULL, 0x1112131415161718ULL);
    std::array<u8, 0x30> expected{};
    const std::array<u8, 16> pointers{8, 7, 6, 5, 4, 3, 2, 1, 24, 23, 22, 21, 20, 19, 18, 17};
    std::copy(pointers.begin(), pointers.end(), expected.begin());
    expected[0x1A] = expected[0x1E] = 0x80;
    expected[0x1B] = expected[0x1F] = 0xBF;
    REQUIRE(request == expected);
}

TEST_CASE("OoT animation blackboard reads typed bounds and byte booleans", "[core][oot_coop][oot_coop_anim]") {
    Memory memory;
    Anim::BlackboardKeys keys;
    keys.instance_off = 8;
    keys.int_off = 16;
    keys.float_off = 24;
    keys.bool_off = 32;
    keys.data_off = 4;
    keys.resource_off = 40;
    keys.manager_off = 8;
    keys.blob_off = 8;
    keys.table_off = 16;
    keys.pool_off = 8;
    keys.magic_off = 0;
    keys.version_off = 4;
    keys.type_stride = 8;
    keys.count_off = 0;
    keys.start_off = 2;
    keys.names_off = 48;
    keys.magic = 1234;
    keys.version = 17;
    keys.fields = {{1, 1, "SyntheticInt"}, {3, 1, "SyntheticFloat"},
                   {4, 1, "SyntheticBool"}};
    memory.Put(256 + 8, 512);
    memory.Put(512 + 16, 768);
    memory.Put(512 + 24, 896);
    memory.Put(512 + 32, 1024);
    memory.Put(512 + 40, 1152);
    memory.Put(1152 + 8, 1280);
    memory.Put(1280 + 8, 1536);
    memory.Put(1280 + 16, 1792);
    memory.Put(1536, keys.magic, 4);
    memory.Put(1536 + 4, keys.version, 4);
    memory.Put(1536 + 8, 512, 4);
    for (const auto array : {768, 896, 1024}) memory.Put(array, 2, 4);
    memory.Put(768 + 8, static_cast<u32>(-7), 4);
    memory.Float(896 + 8, 1.75f);
    memory.Put(1024 + 5, 1, 1);
    for (const auto type : {1, 3, 4}) memory.Put(1792 + 8 * type, 2, 2);
    memory.Put(1792 + 8 + 2, 0, 2);
    memory.Put(1792 + 24 + 2, 2, 2);
    memory.Put(1792 + 32 + 2, 4, 2);
    memory.Put(1792 + 48 + 4, 0, 4);
    memory.Put(1792 + 48 + 12, 64, 4);
    memory.Put(1792 + 48 + 20, 128, 4);
    memory.String(2048, "SyntheticInt");
    memory.String(2112, "SyntheticFloat");
    memory.String(2176, "SyntheticBool");
    const auto read = [&] { return Anim::ReadBlackboard(memory, 256, keys); };
    const auto values = read();
    REQUIRE(values);
    REQUIRE(values->size() == 3);
    REQUIRE(values->at(0).value == -7);
    REQUIRE(values->at(1).value == 1.75);
    REQUIRE(values->at(2).value == 1);
    memory.Put(1024 + 5, 2, 1);
    REQUIRE_FALSE(read());
    memory.Put(1024 + 5, 1, 1);
    memory.Put(1024, 1, 4);
    REQUIRE_FALSE(read());
    memory.Put(1024, 2, 4);
    memory.String(2176, "ReplacedGraph");
    REQUIRE_FALSE(read());
    memory.String(2176, "SyntheticBool");
    memory.Put(1536 + 4, 18, 4);
    REQUIRE_FALSE(read());
}

TEST_CASE("OoT animation player identity carries the age the local pose reports", "[core][oot_coop][oot_coop_anim]") {
    Memory memory;
    Anim::Keys keys;
    keys.player = Core::OotCoop::Player::DecodeKeys(Json{
        {"user_cell", 8}, {"user_equipment", 16}, {"owner", 24}, {"holder", 32}, {"equipment", 40},
        {"actor_record", 48}, {"actor_id", 56}, {"record_actor", 8}, {"record_id", 16},
        {"actor_name", 64}, {"lifecycle", 72}, {"lifecycle_lo", 3}, {"lifecycle_hi", 5},
        {"names", {"SyntheticPlayer"}}});
    REQUIRE(keys.player);
    memory.Put(256 + 8, 512);
    memory.Put(512 + 16, 768);
    memory.Put(768 + 24, 1024);
    memory.Put(1024 + 32, 1280);
    memory.Put(1280 + 40, 768);
    memory.Put(1024 + 48, 1536);
    memory.Put(1024 + 56, 9, 4);
    memory.Put(1536 + 8, 1024);
    memory.Put(1536 + 16, 9, 4);
    memory.Put(1024 + 64, 1792);
    memory.String(1792, "SyntheticPlayer");
    memory.Put(1024 + 72, 1, 4);
    const auto child = Anim::FindPlayer(memory, 256, true, keys);
    REQUIRE(child);
    REQUIRE(child->actor == 1024);
    REQUIRE(child->equipment == 768);
    REQUIRE(Anim::ValidatePlayer(memory, 256, *child, keys));
    const auto adult = Anim::FindPlayer(memory, 256, false, keys);
    REQUIRE(adult);
    REQUIRE_FALSE(*adult == *child); // an age change is a new identity for the command watch
    memory.Put(1024 + 72, 4, 4);
    REQUIRE_FALSE(Anim::ValidatePlayer(memory, 256, *child, keys));
}

TEST_CASE("OoT animation private key decoder rejects partial and unknown configuration", "[core][oot_coop][oot_coop_anim]") {
    Json keys{{"ctl_chain", {8, 16}}, {"ctl_count_off", 4}, {"ctl_entries_off", 16},
              {"ctl_stride", 32}, {"cmd_index_off", 24}, {"cmd_name_off", 8},
              {"frame_off", 32}, {"rate_off", 36}, {"source_slot", 0},
              {"move_command", "SyntheticLocomotion"},
              {"play_wrapper", {{"offset", "0x100"}, {"words", {123}}}},
              {"frame_setter", {{"offset", "0x200"}, {"words", {234}}}},
              {"rate_setter", {{"offset", "0x300"}, {"words", {345}}}},
              {"move_ids", {{"adult", {1, 2}}, {"child", {3, 4}}}},
              {"representatives", {{"adult", Json::array()}, {"child", Json::array()}}},
              {"phase_known_ids", Json::array()}};
    for (const auto* age : {"adult", "child"}) {
        keys["cmd_table"][age] = Json::array();
        for (u32 i = 0; i < Anim::CommandLimit; ++i)
            keys["cmd_table"][age].push_back("SyntheticCommand" + std::to_string(i));
    }
    const auto decoded = Anim::DecodeKeys(keys);
    REQUIRE(decoded);
    REQUIRE_FALSE(decoded->sender_enabled);
    REQUIRE_FALSE(decoded->receiver_enabled);
    REQUIRE_FALSE(decoded->restart_frame_reset);
    REQUIRE_FALSE(decoded->endpoint_inclusive);
    REQUIRE(decoded->phase_known_ids.empty());
    REQUIRE(decoded->play_wrapper.offset == 256);
    REQUIRE(decoded->cmd_table[1].size() == Anim::CommandLimit);
    auto bad = keys;
    bad["unexpected"] = true;
    REQUIRE_FALSE(Anim::DecodeKeys(bad));
    bad = keys;
    bad.erase("ctl_stride");
    REQUIRE_FALSE(Anim::DecodeKeys(bad));
    bad = keys;
    bad["sender_enabled"] = true;
    REQUIRE_FALSE(Anim::DecodeKeys(bad));
    bad = keys;
    bad["play_wrapper"]["offset"] = 257;
    REQUIRE_FALSE(Anim::DecodeKeys(bad));
    bad = keys;
    bad["cmd_table"]["adult"].erase(0);
    REQUIRE_FALSE(Anim::DecodeKeys(bad));
    bad = keys;
    bad["phase_known_ids"] = {1, 1};
    REQUIRE_FALSE(Anim::DecodeKeys(bad));
}

TEST_CASE("OoT player command watch delivers one event per slot transition", "[core][oot_coop][oot_coop_anim]") {
    Anim::Snapshot snapshot;
    snapshot.controllers = {{1280, 1, "SyntheticAction", 3, 1}, {1792, Anim::UnknownCommand, "", 0, 0}};
    Anim::Player player{256, 512, 768, 7, true};
    Anim::CommandWatch watch;
    REQUIRE_FALSE(watch.HasSubscribers());
    std::vector<Anim::CommandChange> seen;
    watch.Subscribe([&](const Anim::CommandChange& change, const Anim::Snapshot& s, const Anim::Player& p) {
        REQUIRE(&s == &snapshot);
        REQUIRE(p == player);
        seen.push_back(change);
    });
    watch.Subscribe([](const Anim::CommandChange&, const Anim::Snapshot&, const Anim::Player&) {
        throw std::runtime_error("a failing consumer");
    });
    REQUIRE(watch.HasSubscribers());
    REQUIRE(watch.Observe(snapshot, player) == 2); // first sight of each slot
    REQUIRE_FALSE(seen[0].previous);
    REQUIRE(seen[0].next.name == "SyntheticAction");
    REQUIRE(seen[1].slot == 1);
    seen.clear();
    snapshot.controllers[0].frame = 9; // frame and rate alone are not transitions
    snapshot.controllers[0].rate = 2;
    REQUIRE(watch.Observe(snapshot, player) == 0);
    snapshot.controllers[0] = {1280, 0, "SyntheticIdle", 0, 1};
    REQUIRE(watch.Observe(snapshot, player) == 1);
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].slot == 0);
    REQUIRE(seen[0].previous->index == 1);
    REQUIRE(seen[0].previous->name == "SyntheticAction");
    REQUIRE(seen[0].next.index == 0);
    seen.clear();
    ++player.generation; // a new player identity restarts the history
    REQUIRE(watch.Observe(snapshot, player) == 2);
    REQUIRE_FALSE(seen[0].previous);
    watch.Reset();
    seen.clear();
    REQUIRE(watch.Observe(snapshot, player) == 2);
}

TEST_CASE("OoT animation sender numbers a watched transition once", "[core][oot_coop][oot_coop_anim]") {
    Memory memory;
    auto keys = MemoryKeys(memory);
    const auto table = Anim::Table::Parse(Document().dump());
    REQUIRE(table);
    const auto snapshot = Anim::ReadSnapshot(memory, 256, true, keys, [] { return true; });
    REQUIRE(snapshot);
    Anim::Sender sender;
    const auto first = sender.Sample(*table, keys, *snapshot, true, 0);
    REQUIRE(sender.Sample(*table, keys, *snapshot, true, 0).aseq == first.aseq);
    sender.Transition(); // the command watch saw a source-slot transition this frame
    const auto next = sender.Sample(*table, keys, *snapshot, true, 0);
    REQUIRE(next.aseq == first.aseq + 1);
    REQUIRE(sender.Sample(*table, keys, *snapshot, true, 0).aseq == next.aseq); // consumed
}
