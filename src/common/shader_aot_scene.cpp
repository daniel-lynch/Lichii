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

void Post(std::string_view scene) {
    std::scoped_lock lock{mutex};
    if (scene.empty() || current == scene) {
        return;
    }
    current = scene;
    generation.fetch_add(1, std::memory_order_release);
}

u64 Generation() {
    return generation.load(std::memory_order_acquire);
}

std::string Current(u64& generation_out) {
    std::scoped_lock lock{mutex};
    generation_out = generation.load(std::memory_order_acquire);
    return current;
}

std::string SceneOfPath(std::string_view path) {
    // /Banc/<Dir>/<Scene>_Static.bcett.byml.zs or _Dynamic: exactly two levels under Banc.
    if (path.starts_with('/')) {
        path.remove_prefix(1);
    }
    if (!path.starts_with("Banc/")) {
        return {};
    }
    path.remove_prefix(5);
    const size_t slash = path.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return {};
    }
    std::string_view name = path.substr(slash + 1);
    if (name.find('/') != std::string_view::npos) {
        return {};
    }
    for (const std::string_view suffix : {"_Static.bcett.byml.zs", "_Dynamic.bcett.byml.zs"}) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            return std::string{name.substr(0, name.size() - suffix.size())};
        }
    }
    return {};
}

} // namespace Common::ShaderAotScene
