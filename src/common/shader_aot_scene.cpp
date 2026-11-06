// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdlib>
#include <mutex>

#include "common/settings.h"
#include "common/shader_aot_scene.h"

namespace Common::ShaderAotScene {
namespace {
std::mutex mutex;
std::string current;
std::atomic<u64> generation{};
std::atomic<size_t> hooked_files{};
std::atomic<u64> prefetches{};
bool entered_any{};

bool EnvIs(const char* name, char value) {
    const char* text = std::getenv(name);
    return text != nullptr && text[0] == value;
}
} // Anonymous namespace

bool Wanted() {
    const char* manifest = std::getenv("EDEN_NX2_AOT_MANIFEST");
    if (manifest == nullptr || manifest[0] == '\0' || EnvIs("EDEN_NX2_AOT_SCENES", '0')) {
        return false;
    }
    return Settings::values.use_shader_aot_translation.GetValue() ||
           EnvIs("EDEN_NX2_AOT_TRANSLATE", '1');
}

void SetHookedFiles(size_t count) {
    hooked_files.store(count, std::memory_order_release);
}

size_t HookedFiles() {
    return hooked_files.load(std::memory_order_acquire);
}

void Post(std::string_view scene, bool entered) {
    std::scoped_lock lock{mutex};
    if (scene.empty()) {
        return;
    }
    if (!entered && entered_any) {
        if (current != scene) {
            prefetches.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    entered_any = entered_any || entered;
    if (current == scene) {
        return;
    }
    current = scene;
    generation.fetch_add(1, std::memory_order_release);
}

u64 Prefetches() {
    return prefetches.load(std::memory_order_relaxed);
}

u64 Generation() {
    return generation.load(std::memory_order_acquire);
}

std::string Current(u64& generation_out) {
    std::scoped_lock lock{mutex};
    generation_out = generation.load(std::memory_order_acquire);
    return current;
}

std::optional<SceneFile> SceneOfPath(std::string_view path) {
    // /Banc/<Dir>/<Scene>_Static.bcett.byml.zs or _Dynamic, and
    // /Banc/GameBancParam/<Scene>.game__banc__GameBancParam.bgyml: two levels under Banc.
    if (path.starts_with('/')) {
        path.remove_prefix(1);
    }
    if (!path.starts_with("Banc/")) {
        return std::nullopt;
    }
    path.remove_prefix(5);
    const size_t slash = path.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return std::nullopt;
    }
    const std::string_view dir = path.substr(0, slash);
    const std::string_view name = path.substr(slash + 1);
    if (name.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto strip = [&](std::string_view suffix) -> std::optional<std::string> {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            return std::string{name.substr(0, name.size() - suffix.size())};
        }
        return std::nullopt;
    };
    if (dir == "GameBancParam") {
        if (auto scene = strip(".game__banc__GameBancParam.bgyml")) {
            return SceneFile{std::move(*scene), true};
        }
        return std::nullopt;
    }
    for (const std::string_view suffix : {"_Static.bcett.byml.zs", "_Dynamic.bcett.byml.zs"}) {
        if (auto scene = strip(suffix)) {
            return SceneFile{std::move(*scene), false};
        }
    }
    return std::nullopt;
}

} // namespace Common::ShaderAotScene
