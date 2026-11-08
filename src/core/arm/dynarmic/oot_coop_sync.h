// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/arm/dynarmic/oot_gamedata_trace.h"

namespace Kernel { class KernelCore; }
namespace Dynarmic::A64 { class Jit; }
namespace Core::Memory { class Memory; }

namespace Core::OotCoop {

inline constexpr u32 ProtocolVersion = 2;
inline constexpr u64 MaxSequence = (u64{1} << 53) - 1;
inline constexpr size_t MaxFrame = 1 << 20;
inline constexpr size_t QueueLimit = 4096;
inline constexpr size_t StateLimit = 4096;

enum class Kind { Bool, Int, Enum, Quest, Bit, PouchAdd, PouchTotal };

struct Change {
    Kind kind{Kind::Bool};
    std::string origin;
    u64 seq{};
    u64 key{}; // Hash, challenge hash, or 64-bit actor hash; never a guest address.
    s64 value{}; // Boolean, int/enum, step index, or pouch count.
    u32 step{};
    bool operator==(const Change&) const = default;
};

[[nodiscard]] nlohmann::json EncodeChange(const Change& change);
[[nodiscard]] std::optional<Change> DecodeChange(const nlohmann::json& packet,
                                                 bool snapshot = false);
[[nodiscard]] std::optional<nlohmann::json> DecodeLine(std::string_view line);
[[nodiscard]] std::string EncodeFrame(const nlohmann::json& packet);
[[nodiscard]] std::optional<u32> FrameLength(std::string_view header);
[[nodiscard]] bool WireKind(Kind kind);
[[nodiscard]] std::optional<std::set<Kind>> DecodeAllowClasses(std::string_view text);

struct Endpoint {
    std::string host;
    u16 port{};
};
[[nodiscard]] std::optional<Endpoint> ParseEndpoint(std::string_view text);
[[nodiscard]] bool Gate(std::string_view endpoint, u64 program_id, std::string_view build_id);
[[nodiscard]] bool Enabled();

struct Hello {
    std::string build_id{OotGameDataTrace::BuildId};
    std::string seed_hash{"0"};
    std::string policy_id{"or-v2"};
    std::string room{"default"};
    std::string team{"default"};
    std::string client_id;
    std::string name{"Player"};
};
[[nodiscard]] nlohmann::json EncodeHello(const Hello& hello);
[[nodiscard]] bool MatchesHello(const nlohmann::json& packet, const Hello& hello);
[[nodiscard]] std::optional<std::set<u64>> DecodeDenyList(const nlohmann::json& packet);

using HighWater = std::map<std::string, u64, std::less<>>;

// Protocol state only. A successful Merge does NOT mean anything was applied to the guest.
class State {
public:
    explicit State(std::set<u64> denied_ = {}) : denied{std::move(denied_)} {}
    bool Merge(const Change& change, bool deduplicate = true);
    bool AdoptSeen(const HighWater& high_water);
    [[nodiscard]] std::optional<Change> Find(Kind kind, u64 key) const;
    [[nodiscard]] std::vector<Change> Snapshot() const;

private:
    std::set<u64> denied;
    std::map<std::pair<Kind, u64>, Change> values;
    HighWater seen;
};

[[nodiscard]] bool ShouldApply(const Change& change, const State& observed);

class Session {
public:
    Session(Kernel::KernelCore& kernel, Kernel::KProcess* owner_, u64 base_, Endpoint endpoint, Hello hello,
            std::set<u64> denied, std::set<Kind> allowed_classes = {},
            std::set<u64> allowed_keys = {});
    ~Session();
    void Stop();
    void Observe(Change change);
    bool Hook(OotGameDataTrace::Point point, u64 pc, Dynarmic::A64::Jit& jit,
              Memory::Memory& memory, u64 thread, u64& tls, u64& tls_ro,
              std::function<void()> clear_exclusive);
    bool IsSentinel(u64 pc) const;
    Kernel::KProcess* const owner;
    const u64 base;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Reset stops the previous transport even when a JIT still owns its shared session.
void BeginSession();
void RegisterMain(Kernel::KernelCore& kernel, Kernel::KProcess* owner, u64 program_id, u64 base,
                  std::string_view build_id);
[[nodiscard]] std::shared_ptr<Session> FindSession(const Kernel::KProcess* owner);

// Reuse only layouts already present in the read-only trace. No names or data tables are shipped.
template <typename Memory>
std::optional<u32> StepIndex(Memory& memory, u64 manager, u32 challenge, u32 step) {
    using namespace OotGameDataTrace::Detail;
    for (const u64 table : {u64{0x40}, u64{0x50}, u64{0x60}}) {
        const auto count = Read32(memory, manager + table);
        const auto data = Read64(memory, manager + table + 8);
        if (!count || !data || *count > 4096) {
            continue;
        }
        for (u32 i = 0; i < *count; ++i) {
            const u64 record = *data + u64{i} * 0x160;
            if (Read32(memory, record + 8) != challenge) {
                continue;
            }
            const u64 state = record + 0x18;
            const auto current = Read32(memory, state + 0x64);
            const auto length = Read32(memory, state + 0x68);
            const auto steps = Read64(memory, state + 0x70);
            if (!current || !length || !steps || *length > 256 || *current >= *length) {
                return std::nullopt;
            }
            for (u32 index = 0; index < *length; ++index) {
                if (Read32(memory, *steps + u64{index} * 0x90 + 0x78) == step) {
                    return index > *current ? std::optional<u32>{index} : std::nullopt;
                }
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace Core::OotCoop
