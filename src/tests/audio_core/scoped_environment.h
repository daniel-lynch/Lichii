// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace AudioTests {
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name_, const char* value) : name{name_} {
        if (const auto* old = std::getenv(name)) {
            previous = old;
        }
        Set(value);
    }
    ~ScopedEnvironment() {
        Set(previous ? previous->c_str() : nullptr);
    }
private:
    void Set(const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
#endif
    }
    const char* name;
    std::optional<std::string> previous;
};
} // namespace AudioTests
