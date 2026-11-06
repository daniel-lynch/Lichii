# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Tests CMakeModules/TranslatorIdentity.cmake on a small fake source tree:
#   cmake -DMODULE=<repo>/CMakeModules/TranslatorIdentity.cmake -DWORK_DIR=<scratch dir> -P this
# The identity must be stable when nothing changes, change with any translator file, a header the
# translator includes, a new translator file or a build fact, and stay put when unrelated code
# changes.

foreach (var MODULE WORK_DIR)
    if (NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set")
    endif()
endforeach()

set(root "${WORK_DIR}/tree")
set(out "${WORK_DIR}/out")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${root}/CMakeModules" "${out}")
file(COPY "${MODULE}" DESTINATION "${root}/CMakeModules")
set(module "${root}/CMakeModules/TranslatorIdentity.cmake")

include("${module}") # for TRANSLATOR_IDENTITY_FILES
foreach (file IN LISTS TRANSLATOR_IDENTITY_FILES)
    if (NOT file MATCHES "^CMakeModules/")
        file(WRITE "${root}/${file}" "// ${file}\n")
    endif()
endforeach()
file(WRITE "${root}/src/shader_recompiler/frontend/translate.cpp"
     "#include \"common/bit_field.h\"\n// translate\n")
file(WRITE "${root}/src/shader_recompiler/backend/emit.cpp" "// emit\n")
file(WRITE "${root}/src/common/bit_field.h" "#include \"common/common_types.h\"\n// bit field\n")
file(WRITE "${root}/src/common/common_types.h" "// types\n")
file(WRITE "${root}/src/common/unrelated.h" "// not included by the translator\n")
file(WRITE "${root}/src/core/core.cpp" "// emulator core\n")
file(WRITE "${root}/src/yuzu/main.cpp" "// frontend\n")
file(WRITE "${out}/facts" "fact compiler=GNU 15.2.1\n")

set(failures 0)

function(identity out_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${root}" "-DFACTS_FILE=${out}/facts"
                "-DOUTPUT_HEADER=${out}/translator_identity.h" "-DMANIFEST=${out}/manifest"
                -P "${module}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "TranslatorIdentity.cmake failed:\n${output}")
    endif()
    file(READ "${out}/translator_identity.h" header)
    if (NOT header MATCHES "TranslatorSourceHash\\[\\] = \"([0-9a-f]+)\"")
        message(FATAL_ERROR "No hash in the generated header:\n${header}")
    endif()
    set(hash "${CMAKE_MATCH_1}")
    string(LENGTH "${hash}" length)
    if (NOT length EQUAL 64)
        message(FATAL_ERROR "Hash ${hash} is not 64 hexadecimal digits")
    endif()
    if (NOT header MATCHES "TranslatorSourceFileCount = ([0-9]+)u")
        message(FATAL_ERROR "No file count in the generated header:\n${header}")
    endif()
    set(${out_var} "${hash}" PARENT_SCOPE)
    set(${out_var}_FILES "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

macro(expect what condition)
    cmake_language(EVAL CODE "
        if (${condition})
            set(passed TRUE)
        else()
            set(passed FALSE)
        endif()")
    if (passed)
        message(STATUS "PASS: ${what}")
    else()
        message(SEND_ERROR "FAIL: ${what}")
        math(EXPR failures "${failures} + 1")
    endif()
endmacro()

identity(base)
identity(again)
expect("stable across two runs on the same tree" "base STREQUAL again")
# 9 listed files + 2 translator files + 2 included common headers.
expect("hashes the listed files, the translator and its common headers (${base_FILES})"
       "base_FILES EQUAL 13")

file(APPEND "${root}/src/core/core.cpp" "// a comment\n")
file(APPEND "${root}/src/yuzu/main.cpp" "// a comment\n")
file(APPEND "${root}/src/common/unrelated.h" "// a comment\n")
identity(unrelated)
expect("unchanged by edits outside the translator" "unrelated STREQUAL base")

file(READ "${root}/src/shader_recompiler/frontend/translate.cpp" saved)
file(APPEND "${root}/src/shader_recompiler/frontend/translate.cpp" "// a comment\n")
identity(translator)
expect("changed by a comment in src/shader_recompiler" "NOT translator STREQUAL base")
file(WRITE "${root}/src/shader_recompiler/frontend/translate.cpp" "${saved}")
identity(restored)
expect("back to the original when the edit is undone" "restored STREQUAL base")

file(READ "${root}/src/common/common_types.h" saved)
file(APPEND "${root}/src/common/common_types.h" "// a comment\n")
identity(included)
expect("changed by a common header the translator includes indirectly"
       "NOT included STREQUAL base")
file(WRITE "${root}/src/common/common_types.h" "${saved}")

file(READ "${root}/src/video_core/shader_environment.cpp" saved)
file(APPEND "${root}/src/video_core/shader_environment.cpp" "// a comment\n")
identity(listed)
expect("changed by a listed video_core file" "NOT listed STREQUAL base")
file(WRITE "${root}/src/video_core/shader_environment.cpp" "${saved}")

file(WRITE "${root}/src/shader_recompiler/new_dir/new_pass.h" "// a new pass\n")
identity(added)
expect("changed by a new file in a new directory under src/shader_recompiler"
       "NOT added STREQUAL base")
file(REMOVE_RECURSE "${root}/src/shader_recompiler/new_dir")

file(RENAME "${root}/src/shader_recompiler/backend/emit.cpp"
     "${root}/src/shader_recompiler/backend/emit2.cpp")
identity(renamed)
expect("changed by a renamed translator file" "NOT renamed STREQUAL base")
file(RENAME "${root}/src/shader_recompiler/backend/emit2.cpp"
     "${root}/src/shader_recompiler/backend/emit.cpp")

file(WRITE "${out}/facts" "fact compiler=GNU 15.2.2\n")
identity(compiler)
expect("changed by a build fact (compiler version)" "NOT compiler STREQUAL base")
file(WRITE "${out}/facts" "fact compiler=GNU 15.2.1\n")

identity(final)
expect("back to the original at the end" "final STREQUAL base")

file(REMOVE "${root}/src/video_core/shader_cache.h")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${root}" "-DFACTS_FILE=${out}/facts"
            "-DOUTPUT_HEADER=${out}/translator_identity.h" "-DMANIFEST=${out}/manifest"
            -P "${module}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
expect("fails when a listed file is missing" "NOT result EQUAL 0")

if (failures GREATER 0)
    message(FATAL_ERROR "${failures} translator identity checks failed")
endif()
message(STATUS "All translator identity checks passed (base ${base})")
