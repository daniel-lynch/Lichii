// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/common_types.h"

namespace Core::OotCoop {
struct Change;
namespace Ledger {
inline constexpr size_t RoomLimit = 8;
inline constexpr size_t AppliedLimit = 8192;
using Id = std::pair<std::string, u64>;
struct Entry {
    std::string epoch;
    u64 through{}, used_ms{};
    std::deque<Id> applied;
};
struct Document {
    std::string slot;
    std::map<std::string, Entry> rooms;
    bool corrupt{};
};
Document Read(const std::filesystem::path& path, std::string_view slot);
void Write(const std::filesystem::path& path, std::string_view slot,
           std::string_view room, Entry entry);
void Merge(Document& document, std::string_view room, Entry entry);
std::string Serialize(const Document& document);
std::optional<Entry> Find(const Document& document, std::string_view room, std::string_view epoch);

class Pattern {
public:
    explicit Pattern(std::string_view text);
    std::optional<std::string> Match(std::string_view path) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl;
};
std::optional<Pattern> LoadConfig(const std::filesystem::path& keys, std::string& why);
std::filesystem::path SidecarPath(std::string_view host, std::string_view slot,
                                 const std::filesystem::path& user);
using Watch = std::function<void(std::string_view, std::string_view, bool)>;
void InstallWatch(const void* owner, Watch callback);
void RemoveWatch(const void* owner) noexcept;
bool Watching() noexcept;
void NoteSaveFile(std::string_view virtual_path, std::string_view host_path, bool write) noexcept;

struct HashResult {
    std::string text, digest;
    size_t files{};
    u64 bytes{};
};
// All enabled directories are listed, including those with no hashed trees.
std::vector<std::filesystem::path> EnabledModFolders(const std::filesystem::path& program_load,
                                                   const std::vector<std::string>& disabled);
HashResult ModHash(const std::filesystem::path& keys, const std::filesystem::path& program_load,
                   const std::vector<std::string>& disabled);
std::string Digest(std::string_view bytes);

struct Page {
    std::string epoch;
    u64 floor{}, through{}, after{};
    bool more{};
    std::vector<Change> grants;
};
std::optional<Page> DecodePage(std::string_view text, std::string_view epoch, u64 after);
std::optional<std::pair<std::string, u64>> DecodeCapability(std::string_view text);

enum class Decision { Applied, Queued, Covered, Admit, Policy };
std::string_view DecisionName(Decision decision);
// Owns the save's working IDs; every method locks independently of the transport/guest locks.
class Book {
public:
    Book();
    ~Book();
    void Bind(const std::optional<Entry>& entry);
    void Epoch(std::string_view epoch);
    Decision Admit(const Change& change, bool live_accepted = true);
    void Applied(const Change& change);
    void Dropped(const Change& change);
    void Quarantined();
    void LocalFlag(const Change& change);
    Entry Snapshot() const;
    void Through(u64 revision);
    void Begin(u64 transaction, std::string_view epoch, u64 floor, u64 through);
    void End(u64 transaction);
    void Abort(u64 transaction);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Save watch and binding, separated from game memory and the transport for unit testing.
class Save {
public:
    using Post = std::function<void(std::function<void()>)>;
    Save(Book& book, Pattern pattern, std::filesystem::path user, std::string room, Post post);
    ~Save();
    void Note(std::string_view path, std::string_view host, bool write);
    void Frame(bool in_game);
    void Epoch(std::string_view epoch);
    bool Bound() const;
    bool Enabled() const;
    void Disable();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Ledger
} // namespace Core::OotCoop
