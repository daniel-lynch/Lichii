// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/coop/coop_boot.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <QProcessEnvironment>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/dynarmic/oot_coop_gui.h"
#include "frontend_common/config.h"

namespace {
namespace Gui = Core::OotCoop::Gui;
std::optional<std::set<std::string>> external_names;
std::string Env(std::string_view name) {
    return qgetenv(std::string{name}.c_str()).toStdString();
}
std::optional<std::string> Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>{in}, {}};
    return in.bad() ? std::nullopt : std::optional{std::move(text)};
}
} // namespace

bool CoopEnvironmentIsExternal(std::string_view name) {
    return external_names ? external_names->contains(std::string{name})
                          : qEnvironmentVariableIsSet(std::string{name}.c_str());
}
void ApplyCoopEnvironment(u64 title_id, const Config& per_game) {
    if (!Gui::CoopTitle(title_id)) return;
    if (!external_names) {
        external_names.emplace();
        for (const auto& key : QProcessEnvironment::systemEnvironment().keys()) {
            if (key.startsWith(QStringLiteral("EDEN_NX2_OOT_COOP")))
                external_names->insert(key.toStdString());
        }
    }
    const auto settings = Gui::FromIni(per_game.ReadRawSection("Coop"));
    const auto session = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) /
                         "coop-session" / fmt::format("{:016X}", title_id);
    auto plan = Gui::PlanEnvironment(settings, *external_names, session, Env, Read);
    if (plan.derived_text && !Gui::WriteSessionKeys(session, std::filesystem::path(Common::FS::ToU8String(settings.keys_path)),
                                                  *plan.derived_text)) {
        LOG_ERROR(Frontend, "OoT co-op GUI: session key copy failed; using original key file");
        auto original = settings;
        original.quest = original.teleport = Gui::Override::Follow;
        original.cleanup = Gui::Cleanup::Follow;
        plan = Gui::PlanEnvironment(original, *external_names, session, Env, Read);
    }
    for (const auto& [name, value] : plan.vars) {
        const bool ok = value ? qputenv(name.c_str(), QByteArray::fromStdString(*value)) : qunsetenv(name.c_str());
        if (!ok) LOG_ERROR(Frontend, "OoT co-op GUI: could not update {}", name);
    }
    if (!plan.note.empty()) LOG_WARNING(Frontend, "OoT co-op GUI: {}", plan.note);
    const auto keys = std::filesystem::path(Common::FS::ToU8String(Env("EDEN_NX2_OOT_COOP_KEYS"))).filename();
    const auto policy = Env("EDEN_NX2_OOT_COOP_POLICY");
    // Only a digest prefix is safe to log; an arbitrary user override is not a digest.
    const bool digest = policy.starts_with("flags-") && policy.size() == 38 &&
        policy.find_first_not_of("0123456789abcdef", 6) == std::string::npos;
    LOG_INFO(Frontend, "OoT co-op GUI: page {}, external {} vars, keys={} ({}), policy={}",
             settings.enabled ? "on" : "off", external_names->size(), Common::FS::PathToUTF8String(keys),
             plan.derived_keys ? "derived" : "original", digest ? policy.substr(0, 14) : "custom/unset");
}
