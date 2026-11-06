# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Identity of the shader translator, for the translated-shader cache
# (src/video_core/shader_translation_cache.cpp, docs/shader-translation-cache.md).
#
# The cache keeps translated stages (IR metadata and SPIR-V) on disk. A cached stage may only be
# used by a build whose translator would produce the same output, so the cache file carries an
# identity of the translator. That identity is a SHA-256 over:
#   - the source of the translator: every file under src/shader_recompiler, the video_core files
#     that drive translation or store its result (TRANSLATOR_IDENTITY_FILES), and every in-repo
#     "common/..." header those files reach through #include;
# Nothing about the running process (executable, path, time) enters it, so two executables built
# from the same source with the same facts (the emulator, shader-sweep) agree on it.
#   - the facts of the build that can change the code generated from identical source: compiler
#     and version, target, build type, flags, definitions, and the sirit/SPIRV-Headers packages.
# A rebuild that changes none of these keeps the cache; any change in them starts a new one.
#
# Included from CMake, this file defines lichii_translator_identity(). Run as a script
# (cmake -P), it computes the identity and writes the generated header.

set(TRANSLATOR_IDENTITY_SCHEME 1)
# Directories hashed recursively, every file in them, relative to the source root.
set(TRANSLATOR_IDENTITY_DIRS
    src/shader_recompiler
)
# Single files hashed besides the directories. A missing file fails the build: a rename must
# update this list, or the cache would silently stop covering the renamed file.
set(TRANSLATOR_IDENTITY_FILES
    src/video_core/offline_shader_producer.cpp
    src/video_core/offline_shader_producer.h
    src/video_core/shader_archive_environment.cpp
    src/video_core/shader_archive_environment.h
    src/video_core/shader_environment.cpp
    src/video_core/shader_environment.h
    src/video_core/shader_translation_cache.cpp
    src/video_core/shader_translation_cache.h
    src/video_core/shader_cache.cpp
    src/video_core/shader_cache.h
    src/video_core/renderer_vulkan/vk_pipeline_cache.cpp
    src/video_core/renderer_vulkan/vk_pipeline_cache.h
    CMakeModules/TranslatorIdentity.cmake
)
# Quoted includes with this prefix, resolved under src/, are followed from every hashed file.
set(TRANSLATOR_IDENTITY_INCLUDE_PREFIX "common/")

if (CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    # Script mode: -DSOURCE_ROOT= -DFACTS_FILE= -DOUTPUT_HEADER= -DMANIFEST= [-DSTAMP=]
    foreach (var SOURCE_ROOT FACTS_FILE OUTPUT_HEADER MANIFEST)
        if (NOT DEFINED ${var})
            message(FATAL_ERROR "TranslatorIdentity.cmake: ${var} is not set")
        endif()
    endforeach()

    set(files)
    foreach (dir IN LISTS TRANSLATOR_IDENTITY_DIRS)
        if (NOT IS_DIRECTORY "${SOURCE_ROOT}/${dir}")
            message(FATAL_ERROR "Translator identity: ${dir} does not exist (update CMakeModules/TranslatorIdentity.cmake)")
        endif()
        file(GLOB_RECURSE found LIST_DIRECTORIES false RELATIVE "${SOURCE_ROOT}"
             "${SOURCE_ROOT}/${dir}/*")
        list(APPEND files ${found})
    endforeach()
    foreach (file IN LISTS TRANSLATOR_IDENTITY_FILES)
        if (NOT EXISTS "${SOURCE_ROOT}/${file}")
            message(FATAL_ERROR "Translator identity: ${file} does not exist (update CMakeModules/TranslatorIdentity.cmake)")
        endif()
        list(APPEND files "${file}")
    endforeach()
    # Python caches and editor files are not source.
    list(FILTER files EXCLUDE REGEX "(/__pycache__/|\\.pyc$|~$|\\.swp$|/\\.)")

    # Follow quoted includes into src/common, transitively.
    set(pending ${files})
    set(seen ${files})
    while (pending)
        list(POP_FRONT pending file)
        file(STRINGS "${SOURCE_ROOT}/${file}" includes
             REGEX "^[ \t]*#[ \t]*include[ \t]*\"${TRANSLATOR_IDENTITY_INCLUDE_PREFIX}")
        foreach (line IN LISTS includes)
            if (line MATCHES "\"(${TRANSLATOR_IDENTITY_INCLUDE_PREFIX}[^\"]+)\"")
                set(header "src/${CMAKE_MATCH_1}")
                if (EXISTS "${SOURCE_ROOT}/${header}" AND NOT header IN_LIST seen)
                    list(APPEND seen "${header}")
                    list(APPEND pending "${header}")
                    list(APPEND files "${header}")
                endif()
            endif()
        endforeach()
    endwhile()
    list(REMOVE_DUPLICATES files)
    list(SORT files)
    list(LENGTH files file_count)

    set(manifest "scheme ${TRANSLATOR_IDENTITY_SCHEME}\n")
    foreach (file IN LISTS files)
        file(SHA256 "${SOURCE_ROOT}/${file}" file_hash)
        string(APPEND manifest "file ${file} ${file_hash}\n")
    endforeach()
    file(READ "${FACTS_FILE}" facts)
    string(APPEND manifest "${facts}")
    string(SHA256 identity "${manifest}")

    # Written only when changed, so a rebuild with the same identity recompiles nothing.
    set(header_text "// Generated by CMakeModules/TranslatorIdentity.cmake. Do not edit.
#pragma once

namespace Shader::Generated {
// SHA-256 of the translator's source and of the build facts that can change its output.
inline constexpr char TranslatorSourceHash[] = \"${identity}\";
// Number of source files in that hash.
inline constexpr unsigned TranslatorSourceFileCount = ${file_count}u;
} // namespace Shader::Generated
")
    foreach (pair "OUTPUT_HEADER;header_text" "MANIFEST;manifest")
        list(GET pair 0 path_var)
        list(GET pair 1 text_var)
        set(old "")
        if (EXISTS "${${path_var}}")
            file(READ "${${path_var}}" old)
        endif()
        if (NOT old STREQUAL "${${text_var}}")
            file(WRITE "${${path_var}}" "${${text_var}}")
        endif()
    endforeach()
    if (DEFINED STAMP)
        file(TOUCH "${STAMP}")
    endif()
    message(STATUS "Translator identity: ${identity} (${file_count} files)")
    return()
endif()

# lichii_translator_identity(TARGET <target>)
# Generates shader_recompiler/translator_source_hash.h for TARGET, the translator library, at the
# end of its CMakeLists.txt. The emulator and the offline tools (shader-sweep) link that library,
# so builds of the same source with the same facts agree on the identity.
function(lichii_translator_identity)
    cmake_parse_arguments(ARG "" "TARGET" "" ${ARGN})
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(out_dir "${CMAKE_BINARY_DIR}/translator_identity")
    set(header "${out_dir}/shader_recompiler/translator_source_hash.h")
    set(stamp "${out_dir}/translator_identity.stamp")
    set(manifest "${out_dir}/translator_identity.manifest")
    set(facts_file "${out_dir}/translator_identity.facts")

    # Build facts that can change the code generated from identical source. translator_identity.cpp
    # adds its own predefined macros (NDEBUG, __OPTIMIZE__, ISA flags), which also tell the
    # configurations of a multi-config generator apart.
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    set(facts "")
    macro(_translator_identity_fact name)
        string(REPLACE "\n" " " _fact_value "${ARGN}")
        string(APPEND facts "fact ${name}=${_fact_value}\n")
    endmacro()
    _translator_identity_fact(system "${CMAKE_SYSTEM_NAME}" "${CMAKE_SYSTEM_PROCESSOR}")
    _translator_identity_fact(compiler "${CMAKE_CXX_COMPILER_ID}" "${CMAKE_CXX_COMPILER_VERSION}"
             "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" "${CMAKE_CXX_SIMULATE_ID}")
    _translator_identity_fact(cxx_standard "${CMAKE_CXX_STANDARD}")
    _translator_identity_fact(cxx_flags "${CMAKE_CXX_FLAGS}")
    if (multi_config)
        _translator_identity_fact(build_type "multi:${CMAKE_CONFIGURATION_TYPES}")
        foreach (config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${config}" upper)
            _translator_identity_fact("cxx_flags_${upper}" "${CMAKE_CXX_FLAGS_${upper}}")
        endforeach()
    else()
        string(TOUPPER "${CMAKE_BUILD_TYPE}" upper)
        _translator_identity_fact(build_type "${CMAKE_BUILD_TYPE}")
        _translator_identity_fact(cxx_flags_config "${CMAKE_CXX_FLAGS_${upper}}")
    endif()
    _translator_identity_fact(ipo "${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
    get_target_property(recompiler_dir ${ARG_TARGET} SOURCE_DIR)
    foreach (property COMPILE_OPTIONS COMPILE_DEFINITIONS)
        get_directory_property(value DIRECTORY "${recompiler_dir}" ${property})
        _translator_identity_fact("dir_${property}" "${value}")
    endforeach()
    foreach (target ${ARG_TARGET})
        foreach (property COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FEATURES CXX_STANDARD
                          INTERPROCEDURAL_OPTIMIZATION)
            get_target_property(value ${target} ${property})
            if (value STREQUAL "value-NOTFOUND")
                set(value "")
            endif()
            _translator_identity_fact("${target}_${property}" "${value}")
        endforeach()
    endforeach()
    # The SPIR-V assembler and its grammar, as registered by CPMUtil (name, url, version or hash).
    get_property(names GLOBAL PROPERTY CPM_PACKAGE_NAMES)
    get_property(urls GLOBAL PROPERTY CPM_PACKAGE_URLS)
    get_property(shas GLOBAL PROPERTY CPM_PACKAGE_SHAS)
    set(index 0)
    foreach (name IN LISTS names)
        string(TOLOWER "${name}" lower)
        if (lower MATCHES "^(sirit|spirv-headers)$")
            set(url "")
            set(sha "")
            list(LENGTH urls url_count)
            list(LENGTH shas sha_count)
            if (index LESS url_count)
                list(GET urls ${index} url)
            endif()
            if (index LESS sha_count)
                list(GET shas ${index} sha)
            endif()
            _translator_identity_fact("package_${lower}" "${url}" "${sha}")
        endif()
        math(EXPR index "${index} + 1")
    endforeach()
    # Rewritten only when changed: a new timestamp would re-run the hash on every configure.
    set(old_facts "")
    if (EXISTS "${facts_file}")
        file(READ "${facts_file}" old_facts)
    endif()
    if (NOT old_facts STREQUAL facts)
        file(WRITE "${facts_file}" "${facts}")
    endif()

    # Every file that can enter the hash is a dependency, so editing one re-runs the script. The
    # script itself globs again and follows includes, so it never works from a stale list;
    # CONFIGURE_DEPENDS re-runs CMake when a file is added or removed, to refresh these dependencies.
    string(REGEX REPLACE "/$" "" include_dir "src/${TRANSLATOR_IDENTITY_INCLUDE_PREFIX}")
    set(relative ${TRANSLATOR_IDENTITY_FILES})
    foreach (dir IN LISTS TRANSLATOR_IDENTITY_DIRS include_dir)
        file(GLOB_RECURSE found CONFIGURE_DEPENDS LIST_DIRECTORIES false RELATIVE "${root}"
             "${root}/${dir}/*")
        list(APPEND relative ${found})
    endforeach()
    list(FILTER relative EXCLUDE REGEX "(/__pycache__/|\\.pyc$|~$|\\.swp$|/\\.)")
    list(TRANSFORM relative PREPEND "${root}/" OUTPUT_VARIABLE depends)

    add_custom_command(
        OUTPUT "${stamp}"
        BYPRODUCTS "${header}" "${manifest}"
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${root}" "-DFACTS_FILE=${facts_file}"
                "-DOUTPUT_HEADER=${header}" "-DMANIFEST=${manifest}" "-DSTAMP=${stamp}"
                -P "${root}/CMakeModules/TranslatorIdentity.cmake"
        DEPENDS ${depends} "${facts_file}"
        COMMENT "Hashing the shader translator's source"
        VERBATIM)
    add_custom_target(translator_identity DEPENDS "${stamp}")
    set_target_properties(translator_identity PROPERTIES FOLDER "Generated")
    add_dependencies(${ARG_TARGET} translator_identity)
    target_sources(${ARG_TARGET} PRIVATE "${header}")
    target_include_directories(${ARG_TARGET} PRIVATE "${out_dir}")
endfunction()
