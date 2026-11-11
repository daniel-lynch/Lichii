// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Co-op checks overlay (host side only, pure reads, no guest writes): the randomizer checks of the
// banc the player stands in, each checked or not, with per-area and overall counts. A check's state is
// a GameData flag read through the co-op client's existing readers (the frame poll's Bool/Bit lookup);
// a flag a peer shared is applied to our game by co-op sync, so it shows here too.
//
// Nothing game-derived is in this file. The check list, names, banc names and flag keys come from a
// private sidecar the randomizer writes next to its spoiler (oot-rando `rando.py checks`):
//   EDEN_NX2_OOT_COOP_CHECKS   0/off/false/no: off; a path: that sidecar; unset: "coop-checks.json"
//                              in the directory of EDEN_NX2_OOT_COOP_KEYS. Missing file: off.
//   "coop-checks-spoiler.json" beside it: item names, shown only with the "spoiler" option.
// Shown while the trigger file exists (EDEN_NX2_OOT_COOP_CHECKS_TRIGGER, default "coop-checks.show"
// beside the keys; EDEN_NX2_OOT_COOP_CHECKS_WINDOW=on|off forces it). The trigger's text holds
// options, any of: "all" (also list checked), "spoiler" (item names), "vanilla" (unshuffled checks
// too), "dense" (more and wider rows, implies all; the other areas' open checks follow the current
// area's). An empty trigger shows the default (conservative) list: the current area only.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/common_types.h"

namespace Core::OotCoop::Checks {

enum class FlagKind : u8 { None, Bool, Bit };
struct Flag {
    FlagKind kind{FlagKind::None};
    u64 key{};
    bool operator==(const Flag&) const = default;
};

struct Check {
    std::string id, name, area;
    std::vector<std::string> bancs;
    bool shuffled{true};
    Flag flag;
    [[nodiscard]] bool Tracked() const { return flag.kind != FlagKind::None; }
    [[nodiscard]] bool In(std::string_view banc) const;
};

inline constexpr size_t CheckLimit = 4096;
inline constexpr size_t NameLimit = 40;
inline constexpr size_t BancLimit = 16;

struct Sidecar {
    std::string seed, file_hash;
    std::map<std::string, std::string, std::less<>> areas; ///< banc -> display name
    std::vector<Check> checks;
    std::map<std::string, std::string, std::less<>> items; ///< id -> item name (spoiler file only)
};

/// The checks document. Strict on what it uses (schema 1, every check's id/name/bancs, a flag of
/// exactly one of "bool" (32-bit) or "bit" (64-bit) as "0x" hex), lenient on extra fields. A bad
/// check is skipped; a bad document gives nothing and `why`.
[[nodiscard]] std::optional<Sidecar> Decode(const nlohmann::json& document, std::string* why = nullptr);
/// Adds the spoiler document's item names; false (and nothing added) unless its file_hash matches.
bool DecodeSpoiler(const nlohmann::json& document, Sidecar& sidecar, std::string* why = nullptr);

struct Options {
    bool all{}, spoiler{}, vanilla{}, dense{};
    bool operator==(const Options&) const = default;
};
/// Words of the trigger file (unknown words ignored, at most 256 characters read).
[[nodiscard]] Options ParseOptions(std::string_view text);

enum class State : u8 { Open, Done, Untracked };

struct Counts {
    size_t done{}, tracked{}, untracked{};
    bool operator==(const Counts&) const = default;
};

enum class Style : u8 { Title, Open, Done, Untracked, Note };
struct Row {
    std::string text;
    Style style{Style::Open};
    bool operator==(const Row&) const = default;
};

/// Check states over one sidecar. Unreadable flags keep their last known state (initially open).
class Model {
public:
    explicit Model(Sidecar sidecar_);
    [[nodiscard]] const Sidecar& Data() const { return sidecar; }
    [[nodiscard]] State StateOf(size_t index) const;
    /// One flag read result for check `index` (nothing: unreadable, the state is kept).
    void Set(size_t index, std::optional<bool> value);
    /// Re-read every tracked check through `read(const Flag&) -> std::optional<bool>`.
    template <typename Read>
    void Evaluate(Read&& read) {
        for (size_t i = 0; i < sidecar.checks.size(); ++i)
            if (sidecar.checks[i].Tracked()) Set(i, read(sidecar.checks[i].flag));
    }
    [[nodiscard]] Counts Here(std::string_view banc, bool vanilla) const;
    [[nodiscard]] Counts Total(bool vanilla) const;
    /// The area name for a banc: the sidecar's "areas", else the first listed check's area, else "".
    [[nodiscard]] std::string AreaName(std::string_view banc) const;
    /// The panel: a title row with counts, then this banc's checks (open, untracked, then done
    /// when listed), cut to the row budget with a "+N more" note. Empty banc: the title only.
    /// Dense: the current area stays on top; the rows left in the budget list the other areas'
    /// open checks below it, by area name, each under an area row with its counts.
    [[nodiscard]] std::vector<Row> Rows(std::string_view banc, bool loading, const Options& options) const;

private:
    void Elsewhere(std::vector<Row>& out, std::string_view banc, std::string_view here, size_t room,
                   const Options& options) const;

    Sidecar sidecar;
    std::vector<bool> done;
};

/// The trigger file: EDEN_NX2_OOT_COOP_CHECKS_TRIGGER, else "coop-checks.show" beside the keys
/// (empty when neither is set). `env` returns "" for an unset variable.
[[nodiscard]] std::string TriggerPath(const std::function<std::string(std::string_view)>& env);

// ---- Process-wide panel: the co-op session writes, the presenter reads. ----
struct Panel {
    std::vector<Row> rows;
    bool dense{};
    u64 generation{};
};
/// Immutable panel, republished only when rows change; empty rows hide it.
void Publish(std::vector<Row> rows, bool dense);
[[nodiscard]] std::shared_ptr<const Panel> Current();
void Reset();

/// Per session, on the frame thread: trigger polling, lazy sidecar load, paced evaluation.
class Window {
public:
    using Reader = std::function<std::optional<bool>(const Flag&)>;
    ~Window();
    /// `read` is called only while the window is shown (every 500 ms, all tracked checks).
    void Frame(const Reader& read, std::string_view banc, bool loading, u64 now_ms);

private:
    void Initialize();
    void Load();
    bool initialized{}, enabled{}, shown{}, published{}, loaded{};
    std::string mode, trigger, sidecar_path;
    std::optional<u64> polled, evaluated;
    Options options;
    std::unique_ptr<Model> model;
    std::vector<Row> rows;
};

} // namespace Core::OotCoop::Checks
