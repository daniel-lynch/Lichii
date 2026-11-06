// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <version>

#include "shader_recompiler/translator_identity.h"
#include "shader_recompiler/translator_source_hash.h"

namespace Shader {
namespace {

#define TRANSLATOR_IDENTITY_STRINGIFY2(x) #x
#define TRANSLATOR_IDENTITY_STRINGIFY(x) TRANSLATOR_IDENTITY_STRINGIFY2(x)

// CMake hashes the flags into the source hash; these macros also tell apart the configurations
// of a multi-config generator, which share one generated header.
constexpr std::string_view CompileFacts =
#ifdef __VERSION__
    "version=" __VERSION__ ";"
#endif
#ifdef _MSC_FULL_VER
    "msc=" TRANSLATOR_IDENTITY_STRINGIFY(_MSC_FULL_VER) ";"
#endif
#ifdef __GLIBCXX__
    "glibcxx=" TRANSLATOR_IDENTITY_STRINGIFY(__GLIBCXX__) ";"
#endif
#ifdef _LIBCPP_VERSION
    "libcpp=" TRANSLATOR_IDENTITY_STRINGIFY(_LIBCPP_VERSION) ";"
#endif
#ifdef _MSVC_STL_UPDATE
    "msvc_stl=" TRANSLATOR_IDENTITY_STRINGIFY(_MSVC_STL_UPDATE) ";"
#endif
#ifdef NDEBUG
    "ndebug;"
#endif
#ifdef _DEBUG
    "debug;"
#endif
#ifdef __OPTIMIZE__
    "optimize;"
#endif
#ifdef __OPTIMIZE_SIZE__
    "optimize_size;"
#endif
#ifdef __FAST_MATH__
    "fast_math;"
#endif
#ifdef _M_FP_FAST
    "fp_fast;"
#endif
#ifdef __FP_FAST_FMA
    "fp_fast_fma;"
#endif
#ifdef __FMA__
    "fma;"
#endif
#ifdef __AVX__
    "avx;"
#endif
#ifdef __AVX2__
    "avx2;"
#endif
#ifdef __AVX512F__
    "avx512f;"
#endif
#ifdef __ARM_FEATURE_FMA
    "arm_fma;"
#endif
#if defined(__x86_64__) || defined(_M_X64)
    "x86_64;"
#elif defined(__aarch64__) || defined(_M_ARM64)
    "arm64;"
#endif
    "";

#undef TRANSLATOR_IDENTITY_STRINGIFY
#undef TRANSLATOR_IDENTITY_STRINGIFY2

} // Anonymous namespace

std::string_view TranslatorSourceHash() {
    return Generated::TranslatorSourceHash;
}

u32 TranslatorSourceFileCount() {
    return Generated::TranslatorSourceFileCount;
}

std::string_view TranslatorCompileFacts() {
    return CompileFacts;
}

std::string TranslatorIdentity() {
    std::string identity{"source|"};
    identity += TranslatorSourceHash();
    identity += '|';
    identity += CompileFacts;
    return identity;
}

} // namespace Shader
