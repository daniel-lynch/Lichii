// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <istream>
#include <random>
#include <sstream>
#include <system_error>
#include <type_traits>

#include <fmt/format.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "video_core/shader_recipe_histogram.h"

namespace VideoCommon {
namespace {
namespace TC = TranslationCache;

constexpr std::array<char, 8> Magic{'L', 'X', 'R', 'E', 'C', 'I', 'P', '1'};
constexpr u32 FormatVersion = 1;

class Writer {
public:
    template <typename T>
    void Put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
    }
    template <size_t N>
    void PutBits(const std::bitset<N>& bits) {
        std::array<u8, N / 8> bytes{};
        for (size_t i = 0; i < N; ++i) {
            bytes[i / 8] |= static_cast<u8>(bits[i] ? 1U << (i % 8) : 0U);
        }
        Put(bytes);
    }
    std::vector<u8> data;
};

class Reader {
public:
    Reader(std::istream& in_, u64 size) : in{in_}, remaining{size} {}
    template <typename T>
    bool Get(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (sizeof(T) > remaining ||
            !in.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)))) {
            return false;
        }
        remaining -= sizeof(T);
        return true;
    }
    template <size_t N>
    bool GetBits(std::bitset<N>& bits) {
        std::array<u8, N / 8> bytes{};
        if (!Get(bytes)) {
            return false;
        }
        for (size_t i = 0; i < N; ++i) {
            bits[i] = (bytes[i / 8] >> (i % 8)) & 1;
        }
        return true;
    }
    [[nodiscard]] bool AtEnd() const noexcept {
        return remaining == 0;
    }

private:
    std::istream& in;
    u64 remaining;
};

void PutRuntimeInfo(Writer& out, const Shader::RuntimeInfo& info) {
    out.Put(info.generic_input_types);
    out.PutBits(info.previous_stage_stores.mask);
    out.Put(static_cast<u32>(info.per_primitive_generics.to_ulong()));
    out.Put(static_cast<u32>(info.previous_stage_legacy_stores_mapping.size()));
    for (const auto& [from, to] : info.previous_stage_legacy_stores_mapping) {
        out.Put(from);
        out.Put(to);
    }
    out.Put(info.convert_depth_mode);
    out.Put(info.force_early_z);
    out.Put(info.tess_primitive);
    out.Put(info.tess_spacing);
    out.Put(info.tess_clockwise);
    out.Put(info.input_topology);
    out.Put(info.fixed_state_point_size.has_value());
    out.Put(info.fixed_state_point_size.value_or(0.0f));
    out.Put(info.alpha_test_func.has_value());
    out.Put(info.alpha_test_func.value_or(Shader::CompareFunction{}));
    out.Put(info.alpha_test_reference);
    out.Put(info.y_negate);
    out.Put(info.glasm_use_storage_buffers);
    const u32 xfb = std::min<u32>(info.xfb_count, static_cast<u32>(info.xfb_varyings.size()));
    out.Put(xfb);
    for (u32 i = 0; i < xfb; ++i) {
        out.Put(info.xfb_varyings[i]);
    }
    out.Put(info.color_output_types);
    out.Put(info.dual_source_blend);
}

bool GetRuntimeInfo(Reader& in, Shader::RuntimeInfo& info) {
    u32 primitive_generics{}, legacy{};
    if (!in.Get(info.generic_input_types) || !in.GetBits(info.previous_stage_stores.mask) ||
        !in.Get(primitive_generics) || !in.Get(legacy) ||
        legacy > RecipeHistogram::MaxLegacyMappings) {
        return false;
    }
    info.per_primitive_generics = std::bitset<32>{primitive_generics};
    for (u32 i = 0; i < legacy; ++i) {
        Shader::IR::Attribute from{}, to{};
        if (!in.Get(from) || !in.Get(to)) {
            return false;
        }
        info.previous_stage_legacy_stores_mapping.emplace(from, to);
    }
    bool has_point_size{}, has_alpha_test{};
    float point_size{};
    Shader::CompareFunction alpha_test{};
    u32 xfb{};
    if (!in.Get(info.convert_depth_mode) || !in.Get(info.force_early_z) ||
        !in.Get(info.tess_primitive) || !in.Get(info.tess_spacing) ||
        !in.Get(info.tess_clockwise) || !in.Get(info.input_topology) ||
        !in.Get(has_point_size) || !in.Get(point_size) || !in.Get(has_alpha_test) ||
        !in.Get(alpha_test) || !in.Get(info.alpha_test_reference) || !in.Get(info.y_negate) ||
        !in.Get(info.glasm_use_storage_buffers) || !in.Get(xfb) ||
        xfb > info.xfb_varyings.size()) {
        return false;
    }
    if (has_point_size) {
        info.fixed_state_point_size = point_size;
    }
    if (has_alpha_test) {
        info.alpha_test_func = alpha_test;
    }
    info.xfb_count = xfb;
    for (u32 i = 0; i < xfb; ++i) {
        if (!in.Get(info.xfb_varyings[i])) {
            return false;
        }
    }
    return in.Get(info.color_output_types) && in.Get(info.dual_source_blend);
}

TC::Digest RecipeDigest(const Shader::RuntimeInfo& runtime_info,
                        const Shader::Backend::Bindings& binding_in) {
    return TC::MakeVariantKey(TC::Digest{}, TC::DigestRuntimeInfo(runtime_info), binding_in);
}
} // Anonymous namespace

u64 RecipeHistogram::ClassKey(Shader::Stage stage, const std::bitset<512>* loads) {
    u64 key = (static_cast<u64>(stage) + 1) * 0x9e3779b97f4a7c15ULL;
    if (loads != nullptr) {
        key ^= std::hash<std::bitset<512>>{}(*loads) * 0xff51afd7ed558ccdULL + 1;
    }
    return key;
}

std::vector<RecipeHistogram::Recipe>::iterator RecipeHistogram::Victim(std::vector<Recipe>& list) {
    // A full class makes room for a new recipe: first the weakest recipe this session has not
    // seen (it only came from the file, so earlier sessions' recipes cannot hold the slots
    // forever), else a recipe seen once this session (as weak as the newcomer).
    const auto weakest = [&](auto&& eligible) {
        auto best = list.end();
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (eligible(*it) && (best == list.end() || it->count < best->count)) {
                best = it;
            }
        }
        return best;
    };
    if (const auto unseen = weakest([](const Recipe& recipe) { return !recipe.seen; });
        unseen != list.end()) {
        return unseen;
    }
    return weakest([](const Recipe& recipe) { return recipe.count <= 1; });
}

u64 RecipeHistogram::MaxFileBytes() {
    using Info = Shader::RuntimeInfo;
    constexpr u64 runtime_info =
        sizeof(Info::generic_input_types) + 512 / 8 + sizeof(u32) + sizeof(u32) +
        MaxLegacyMappings * 2 * sizeof(Shader::IR::Attribute) + 2 * sizeof(bool) +
        sizeof(Info::tess_primitive) + sizeof(Info::tess_spacing) + sizeof(bool) +
        sizeof(Info::input_topology) + sizeof(bool) + sizeof(float) + sizeof(bool) +
        sizeof(Shader::CompareFunction) + sizeof(float) + 2 * sizeof(bool) + sizeof(u32) +
        sizeof(Info::xfb_varyings) + sizeof(Info::color_output_types) + sizeof(bool);
    constexpr u64 recipe = sizeof(u64) + runtime_info + sizeof(Shader::Backend::Bindings);
    constexpr u64 entry = sizeof(Shader::Stage) + sizeof(u8) + 512 / 8 + sizeof(u32) +
                          MaxPerClass * recipe;
    return sizeof(Magic) + sizeof(FormatVersion) + sizeof(TC::Digest) + sizeof(u32) +
           MaxClasses * entry;
}

bool RecipeHistogram::Record(Shader::Stage stage, const std::bitset<512>& loads,
                             const Shader::RuntimeInfo& runtime_info,
                             const Shader::Backend::Bindings& binding_in) {
    if (runtime_info.previous_stage_legacy_stores_mapping.size() > MaxLegacyMappings) {
        return false;
    }
    const TC::Digest digest = RecipeDigest(runtime_info, binding_in);
    const u64 own_class = ClassKey(stage, &loads);
    bool first = false;
    for (const u64 class_key : {own_class, ClassKey(stage, nullptr)}) {
        auto it = classes.find(class_key);
        if (it == classes.end()) {
            if (classes.size() >= MaxClasses) {
                continue;
            }
            const bool by_loads = class_key == own_class;
            it = classes
                     .emplace(class_key,
                              Class{stage, by_loads, by_loads ? loads : std::bitset<512>{}, {}})
                     .first;
        }
        ++revision;
        auto& list = it->second.recipes;
        const auto known = std::ranges::find(list, digest, &Recipe::digest);
        if (known != list.end()) {
            ++known->count;
            known->seen = true;
        } else if (list.size() < MaxPerClass) {
            list.push_back({runtime_info, binding_in, digest, 1, true});
            first = first || (list.size() == 1 && class_key == own_class);
        } else if (const auto victim = Victim(list); victim != list.end()) {
            *victim = Recipe{runtime_info, binding_in, digest, 1, true};
        } else {
            // Every resident was seen this session more than once: the newcomer is tallied
            // aside, and promoted over the weakest resident once its tally is higher.
            auto& shadow = it->second.shadow;
            auto tally = std::ranges::find(shadow, digest, &Recipe::digest);
            if (tally != shadow.end()) {
                ++tally->count;
            } else if (shadow.size() < MaxShadow) {
                tally = shadow.insert(shadow.end(), Recipe{runtime_info, binding_in, digest, 1, true});
            } else {
                tally = std::ranges::min_element(shadow, {}, &Recipe::count);
                *tally = Recipe{runtime_info, binding_in, digest, 1, true};
            }
            const auto weakest = std::ranges::min_element(list, {}, &Recipe::count);
            if (tally->count > weakest->count) {
                *weakest = std::move(*tally);
                shadow.erase(tally);
            }
        }
    }
    return first;
}

std::optional<RecipeHistogram::Recipe> RecipeHistogram::Best(Shader::Stage stage,
                                                             const std::bitset<512>& loads,
                                                             bool& class_match) const {
    for (const bool by_class : {true, false}) {
        const auto it = classes.find(ClassKey(stage, by_class ? &loads : nullptr));
        if (it == classes.end() || it->second.recipes.empty()) {
            continue;
        }
        class_match = by_class;
        return *std::ranges::max_element(it->second.recipes, {}, &Recipe::count);
    }
    return std::nullopt;
}

bool RecipeHistogram::HasRecipes(u64 class_key) const {
    const auto it = classes.find(class_key);
    return it != classes.end() && !it->second.recipes.empty();
}

void RecipeHistogram::Clear() {
    classes.clear();
    ++revision;
}

size_t RecipeHistogram::Recipes() const noexcept {
    size_t count = 0;
    for (const auto& [key, entry] : classes) {
        count += entry.recipes.size();
    }
    return count;
}

std::vector<u8> RecipeHistogram::Serialize(const TC::Digest& config) const {
    Writer out;
    out.Put(Magic);
    out.Put(FormatVersion);
    out.Put(config);
    u32 count = 0;
    for (const auto& [key, entry] : classes) {
        count += entry.recipes.empty() ? 0 : 1;
    }
    out.Put(count);
    for (const auto& [key, entry] : classes) {
        if (entry.recipes.empty()) {
            continue;
        }
        out.Put(entry.stage);
        out.Put(static_cast<u8>(entry.by_loads ? 1 : 0));
        if (entry.by_loads) {
            out.PutBits(entry.loads);
        }
        out.Put(static_cast<u32>(entry.recipes.size()));
        for (const Recipe& recipe : entry.recipes) {
            out.Put(recipe.count);
            PutRuntimeInfo(out, recipe.runtime_info);
            out.Put(recipe.binding_in);
        }
    }
    return std::move(out.data);
}

bool RecipeHistogram::Load(std::span<const u8> data, const TC::Digest& config, std::string& why) {
    std::istringstream stream{std::string{reinterpret_cast<const char*>(data.data()), data.size()}};
    return Load(stream, data.size(), config, why);
}

bool RecipeHistogram::Load(std::istream& stream, u64 size, const TC::Digest& config,
                           std::string& why) {
    Clear();
    const auto refuse = [&](const char* reason) {
        Clear();
        why = reason;
        return false;
    };
    if (size > MaxFileBytes()) {
        return refuse("larger than the format allows");
    }
    Reader in{stream, size};
    std::array<char, 8> magic{};
    u32 version{}, count{};
    TC::Digest file_config{};
    if (!in.Get(magic) || magic != Magic || !in.Get(version) || version != FormatVersion) {
        return refuse("another format");
    }
    if (!in.Get(file_config) || !(file_config == config)) {
        return refuse("another translation configuration");
    }
    if (!in.Get(count) || count > MaxClasses) {
        return refuse("truncated");
    }
    for (u32 c = 0; c < count; ++c) {
        Class entry{};
        u8 by_loads{};
        u32 recipes{};
        if (!in.Get(entry.stage) || !in.Get(by_loads) || by_loads > 1 ||
            (by_loads != 0 && !in.GetBits(entry.loads)) || !in.Get(recipes) ||
            recipes > MaxPerClass) {
            return refuse("truncated");
        }
        entry.by_loads = by_loads != 0;
        for (u32 r = 0; r < recipes; ++r) {
            Recipe recipe{};
            if (!in.Get(recipe.count) || !GetRuntimeInfo(in, recipe.runtime_info) ||
                !in.Get(recipe.binding_in)) {
                return refuse("truncated");
            }
            recipe.count = (recipe.count + 1) / 2;
            recipe.digest = RecipeDigest(recipe.runtime_info, recipe.binding_in);
            if (std::ranges::find(entry.recipes, recipe.digest, &Recipe::digest) !=
                entry.recipes.end()) {
                return refuse("a recipe repeated in its class");
            }
            entry.recipes.push_back(std::move(recipe));
        }
        const u64 key = ClassKey(entry.stage, entry.by_loads ? &entry.loads : nullptr);
        if (!classes.emplace(key, std::move(entry)).second) {
            return refuse("a class repeated");
        }
    }
    if (!in.AtEnd()) {
        return refuse("trailing bytes");
    }
    ++revision;
    return true;
}

bool WriteFileAtomically(const std::filesystem::path& path, std::span<const u8> data,
                         std::string& why) {
    static std::atomic<u64> counter{};
#ifdef _WIN32
    const auto pid = static_cast<u64>(_getpid());
#else
    const auto pid = static_cast<u64>(getpid());
#endif
    std::FILE* file = nullptr;
    std::filesystem::path temporary;
    std::random_device random;
    for (int attempt = 0; attempt < 8 && file == nullptr; ++attempt) {
        temporary = path;
        temporary += fmt::format(".tmp.{}.{}.{:08x}", pid, counter.fetch_add(1), random());
        // "x": created exclusively, never an existing file another writer has open.
#ifdef _WIN32
        file = _wfopen(temporary.c_str(), L"wbx");
#else
        file = std::fopen(temporary.c_str(), "wbx");
#endif
        if (file == nullptr && errno != EEXIST) {
            break;
        }
    }
    if (file == nullptr) {
        why = fmt::format("cannot create a temporary file next to {}: {}", path.string(),
                          std::strerror(errno));
        return false;
    }
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
    ok = std::fflush(file) == 0 && ok;
#ifdef _WIN32
    ok = _commit(_fileno(file)) == 0 && ok;
#else
    ok = fsync(fileno(file)) == 0 && ok;
#endif
    ok = std::fclose(file) == 0 && ok;
    std::error_code error;
    if (!ok) {
        why = fmt::format("cannot write {}: {}", temporary.string(), std::strerror(errno));
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        why = fmt::format("cannot replace {}: {}", path.string(), error.message());
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace VideoCommon
