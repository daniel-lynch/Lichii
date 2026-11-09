// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Hidden check of a PRIVATE key file and its sidecars with the real decoders (the same strict
// decoders the emulator runs). Nothing game-derived lives here: the file is named at run time.
//   EDEN_NX2_OOT_COOP_KEYS=<kit>/chest-keys-cNN.json
//   EDEN_NX2_OOT_COOP_KEYS_BUILD=<40-hex main build id>
//   tests "[.oot_coop_keyfile]"
// The sidecars (coop-anim.json, coop-equip.json, coop-ux.json, coop-ledger.json) are read beside the key file.
// Optional: EDEN_NX2_OOT_COOP_KEYS_LOAD=<load/<title id>> also prints the ledger mod hash of that tree.

#include <cstdlib>
#include <fstream>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "core/arm/dynarmic/oot_coop_anim.h"
#include "core/arm/dynarmic/oot_coop_equip.h"
#include "core/arm/dynarmic/oot_coop_ledger.h"
#include "core/arm/dynarmic/oot_coop_quest.h"
#include "core/arm/dynarmic/oot_coop_room.h"
#include "core/arm/dynarmic/oot_coop_teleport.h"
#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_ux.h"

namespace {
std::string Env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}
} // namespace

TEST_CASE("OoT co-op private key file loads with the strict decoders", "[.oot_coop_keyfile]") {
    namespace Coop = Core::OotCoop;
    const auto path = Env("EDEN_NX2_OOT_COOP_KEYS");
    const auto build = Env("EDEN_NX2_OOT_COOP_KEYS_BUILD");
    if (path.empty() || build.empty()) SKIP("EDEN_NX2_OOT_COOP_KEYS / EDEN_NX2_OOT_COOP_KEYS_BUILD unset");

    std::string why;
    const auto profile = Coop::LoadProfile(build, &why);
    INFO(why);
    REQUIRE(profile);

    std::ifstream in{path};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto document = nlohmann::json::parse(text, nullptr, false);
    REQUIRE_FALSE(document.is_discarded());
    const auto policy = Coop::DecodeKeyPolicy(document);
    REQUIRE(policy);
    const auto extras = Coop::DecodeKeyExtras(document);
    REQUIRE(extras);

    const char* cleanup = profile->world.cleanup == Coop::World::Cleanup::Death ? "death" : "flag_only";
    WARN(fmt::format("profile ok: policy keys {}, deny {}, dungeon bools {} / key pairs {}; "
                     "world cleanup={} hearts={} tokens={} reconcile={}; chest enforce={}; "
                     "anim {} (sender={} receiver={}); equip {} (enabled={}); ghost {}",
                     policy->size(), extras->deny.size(), extras->dungeon_bools.size(),
                     extras->dungeon_keys.size(), cleanup, profile->world.hearts, profile->world.tokens,
                     profile->world.reconcile == Coop::World::Reconcile::Off ? "off" : "target_current",
                     profile->chest.enforce, profile->anim ? "present" : "absent",
                     profile->anim && profile->anim->sender_enabled,
                     profile->anim && profile->anim->receiver_enabled,
                     profile->equip ? "present" : "absent", profile->equip && profile->equip->enabled,
                     profile->ghost.Valid() ? "valid" : "INVALID"));

    if (profile->anim && (profile->anim->sender_enabled || profile->anim->receiver_enabled)) {
        std::string table_why;
        const auto table = Coop::Anim::LoadTable(&table_why);
        INFO(table_why);
        REQUIRE(table);
        WARN(fmt::format("coop-anim.json: {} entries, sha256 {}", table->Size(), table->Hash()));
    }
    if (profile->equip && profile->equip->enabled) {
        const auto names = Coop::Equip::LoadNames(build);
        REQUIRE(names);
        REQUIRE_FALSE(names->players.empty());
        WARN(fmt::format("coop-equip.json: {} rows, players {}", names->rows.size(),
                         fmt::join(names->players, ",")));
    }
    std::string ux_why;
    const auto ux = Coop::Ux::LoadConfig(build, &ux_why);
    REQUIRE(ux);
    WARN(fmt::format("coop-ux.json: {} (camera keys {})", ux_why, ux->camera ? "present" : "absent"));

    // C16 sections (integration check): quest, teleport, room window sidecar fields, ledger sidecar.
    if (profile->quest) {
        size_t steps = 0;
        for (const auto& [challenge, list] : profile->quest->mirror) steps += list.size();
        WARN(fmt::format("quest present (enabled={}): mirror {} challenges / {} safe steps, {} commit observers, "
                         "deny.quest {}", profile->quest->enabled, profile->quest->mirror.size(), steps,
                         profile->quest->observers.size(), extras->deny_quest.size()));
    } else WARN("quest absent");
    if (profile->teleport) {
        const auto& t = *profile->teleport;
        WARN(fmt::format("teleport present (enabled={}): requester [{}] answer [{}], allow bancs {}, deny bancs {}, "
                         "cross {} (enabled={} explicit={})", t.enabled, fmt::join(t.requester_commands, ","),
                         fmt::join(t.answer_commands, ","), t.allow_bancs.size(), t.deny_bancs.size(),
                         t.cross ? "present" : "absent", t.cross && t.cross->enabled,
                         t.cross && t.cross->explicit_position));
    } else WARN("teleport absent");
    if (ux->room) {
        WARN(fmt::format("coop-ux.json room: {} area names, finish {}, minimap {}", ux->room->area_names.size(),
                         ux->room->finish ? "present" : "absent", ux->room->minimap ? "present" : "absent"));
    }
    std::string ledger_why;
    const auto pattern = Coop::Ledger::LoadConfig(path, ledger_why);
    WARN(fmt::format("coop-ledger.json: {}", pattern ? std::string{"save pattern loaded"} : ledger_why));
    if (const auto load = Env("EDEN_NX2_OOT_COOP_KEYS_LOAD"); !load.empty()) {
        const auto hash = Coop::Ledger::ModHash(path, load);
        WARN(fmt::format("mod hash {} ({} mod files, {} bytes)\n{}", hash.digest, hash.files, hash.bytes, hash.text));
    }
}
