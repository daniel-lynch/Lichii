// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_ledger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <openssl/evp.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include "common/logging.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace Core::OotCoop::Ledger {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
std::atomic<bool> watching{};
std::mutex watch_mutex;
Watch watcher;
const void* watch_owner{};

u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
bool Identifier(std::string_view text) {
    return !text.empty() && text.size() <= 64 && std::ranges::all_of(text, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}
bool Slot(std::string_view text) { return Identifier(text) && text != "." && text != ".."; }
bool U53(const Json& value) {
    return value.is_number_integer() && !value.is_boolean() &&
           (value.is_number_unsigned() || value.get<s64>() >= 0) && value.get<u64>() <= MaxSequence;
}
std::string ReadText(const fs::path& path, size_t limit = 16 * 1024 * 1024) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.string());
    std::string text;
    std::array<char, 65536> block{};
    while (input) {
        input.read(block.data(), block.size());
        text.append(block.data(), static_cast<size_t>(input.gcount()));
        if (text.size() > limit) throw std::runtime_error("file too large: " + path.string());
    }
    if (!input.eof()) throw std::runtime_error("read failed: " + path.string());
    return text;
}
Json Parse(std::string_view text) {
    if (text.empty() || text.size() > 16 * 1024 * 1024) throw std::runtime_error("JSON size limit");
    std::vector<std::set<std::string>> keys;
    bool bad = false;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 16) throw std::runtime_error("JSON nesting limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) bad = true;
        return true;
    };
    auto parsed = Json::parse(text, callback, false);
    if (bad || !parsed.is_object()) throw std::runtime_error("invalid JSON");
    return parsed;
}
void AtomicWrite(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    static std::atomic<u64> serial{};
    const fs::path temporary = path.string() + ".tmp." + std::to_string(Now()) + "." + std::to_string(++serial);
    try {
#ifdef _WIN32
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        out.write(text.data(), text.size());
        out.flush();
        if (!out) throw std::runtime_error("sidecar write failed");
        out.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("sidecar replace failed");
#else
        const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) throw std::runtime_error("sidecar create failed");
        size_t done = 0;
        bool ok = true;
        while (done < text.size()) {
            const auto count = write(fd, text.data() + done, text.size() - done);
            if (count <= 0) { ok = false; break; }
            done += static_cast<size_t>(count);
        }
        (void)fsync(fd); // Best effort: the guest save itself has no corresponding commit barrier.
        if (close(fd) != 0 || !ok) throw std::runtime_error("sidecar write failed");
        fs::rename(temporary, path);
        const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (directory >= 0) { (void)fsync(directory); close(directory); }
#endif
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
std::pair<u64, std::string> Fingerprint(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot hash " + path.string());
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA256 initialization failed");
    std::array<char, 65536> buffer{};
    u64 size = 0;
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = static_cast<size_t>(input.gcount());
        size += count;
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), count) != 1)
            throw std::runtime_error("SHA256 update failed");
    }
    if (!input.eof()) throw std::runtime_error("cannot hash " + path.string());
    std::array<unsigned char, 32> result{};
    unsigned int length{};
    if (EVP_DigestFinal_ex(ctx.get(), result.data(), &length) != 1 || length != result.size())
        throw std::runtime_error("SHA256 final failed");
    std::string hex;
    for (auto byte : result) hex += fmt::format("{:02x}", byte);
    return {size, hex};
}
void Files(const fs::path& directory, std::set<fs::path>& ancestors, std::vector<fs::path>& result) {
    const auto real = fs::canonical(directory);
    if (!ancestors.insert(real).second) throw std::runtime_error("mod directory symlink cycle");
    for (const auto& file : fs::directory_iterator(directory)) {
        if (file.is_directory()) Files(file.path(), ancestors, result);
        else if (file.is_regular_file()) result.push_back(file.path());
        else if (file.is_symlink()) throw std::runtime_error("broken mod symlink");
    }
    ancestors.erase(real);
}
}

void InstallWatch(const void* owner, Watch callback) {
    std::scoped_lock lock{watch_mutex};
    watch_owner = owner;
    watcher = std::move(callback);
    watching.store(true, std::memory_order_release);
}
void RemoveWatch(const void* owner) noexcept {
    std::scoped_lock lock{watch_mutex};
    if (watch_owner != owner) return;
    watching.store(false, std::memory_order_release);
    watcher = {};
    watch_owner = nullptr;
}
bool Watching() noexcept { return watching.load(std::memory_order_acquire); }
void NoteSaveFile(std::string_view path, std::string_view host, bool write) noexcept {
    if (!watching.load(std::memory_order_acquire)) return;
    try {
        std::scoped_lock lock{watch_mutex};
        if (watcher) watcher(path, host, write);
    } catch (...) {
        // A diagnostic observer must never change the filesystem service's result.
    }
}

struct Pattern::Impl { std::regex expression; };
Pattern::Pattern(std::string_view text)
    : impl{std::make_shared<Impl>(Impl{std::regex{std::string{text}, std::regex::ECMAScript}})} {
    if (text.empty() || text.size() > 4096 || impl->expression.mark_count() != 1)
        throw std::runtime_error("save_file must have exactly one capture group");
}
std::optional<std::string> Pattern::Match(std::string_view path) const {
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_match(path.begin(), path.end(), match, impl->expression)) return {};
    const auto slot = match[1].str();
    return Slot(slot) ? std::optional{slot} : std::nullopt;
}
std::optional<Pattern> LoadConfig(const fs::path& keys, std::string& why) {
    try {
        if (keys.empty()) throw std::runtime_error("no key file");
        auto value = Parse(ReadText(keys.parent_path() / "coop-ledger.json", 16384));
        if (value.size() != 2 || !value.contains("version") || !U53(value["version"]) ||
            value["version"] != 1 || !value.contains("save_file") || !value["save_file"].is_string())
            throw std::runtime_error("invalid ledger config");
        return Pattern{value["save_file"].get<std::string>()};
    } catch (const std::exception& error) { why = error.what(); return {}; }
}
fs::path SidecarPath(std::string_view host, std::string_view slot, const fs::path& user) {
    if (!Slot(slot)) throw std::runtime_error("invalid save slot");
    const fs::path path{host};
    std::error_code error;
    if (path.is_absolute() && fs::is_directory(path.parent_path(), error))
        return path.parent_path() / "coop-ledger.json";
    return user / "oot-coop-ledger" / (std::string{slot} + ".json");
}
Document Read(const fs::path& path, std::string_view slot) {
    Document document{std::string{slot}};
    try {
        if (!fs::exists(fs::symlink_status(path))) return document;
        auto value = Parse(ReadText(path));
        if (value.size() != 3 || !U53(value.at("format")) || value.at("format") != 1 ||
            value.at("slot") != slot || !value.at("rooms").is_object() || value.at("rooms").size() > RoomLimit)
            throw std::runtime_error("invalid sidecar document");
        for (const auto& [room, v] : value.at("rooms").items()) {
            const auto slash = room.find('/');
            if (slash == std::string::npos || !Identifier(room.substr(0, slash)) || !Identifier(room.substr(slash + 1)) ||
                v.size() != 4 || !v.at("epoch").is_string() || !Identifier(v.at("epoch").get<std::string>()) ||
                !U53(v.at("through")) || !U53(v.at("used_ms")) || !v.at("applied").is_array() ||
                v.at("applied").size() > AppliedLimit) throw std::runtime_error("invalid sidecar entry");
            Entry entry{v.at("epoch").get<std::string>(), v.at("through").get<u64>(), v.at("used_ms").get<u64>()};
            std::set<Id> unique;
            for (const auto& id : v.at("applied")) {
                if (!id.is_array() || id.size() != 2 || !id[0].is_string() || !Identifier(id[0].get<std::string>()) ||
                    !U53(id[1]) || id[1] == 0) throw std::runtime_error("invalid applied ID");
                Id parsed{id[0].get<std::string>(), id[1].get<u64>()};
                if (!unique.insert(parsed).second) throw std::runtime_error("duplicate applied ID");
                entry.applied.push_back(std::move(parsed));
            }
            document.rooms.emplace(room, std::move(entry));
        }
    } catch (const std::exception& error) {
        document.rooms.clear();
        document.corrupt = true;
        LOG_WARNING(Core_ARM, "OoT co-op: ledger sidecar unreadable {}: {} (kept until save-write)", path.string(), error.what());
    }
    return document;
}
void Merge(Document& document, std::string_view room, Entry entry) {
    while (entry.applied.size() > AppliedLimit) entry.applied.pop_front();
    document.rooms[std::string{room}] = std::move(entry);
    while (document.rooms.size() > RoomLimit) {
        auto oldest = document.rooms.end();
        for (auto it = document.rooms.begin(); it != document.rooms.end(); ++it) {
            if (it->first == room) continue;
            if (oldest == document.rooms.end() || it->second.used_ms < oldest->second.used_ms) oldest = it;
        }
        document.rooms.erase(oldest);
    }
}
std::optional<Entry> Find(const Document& document, std::string_view room, std::string_view epoch) {
    auto it = document.rooms.find(std::string{room});
    if (it == document.rooms.end() || it->second.epoch != epoch) return {};
    return it->second;
}
std::string Serialize(const Document& document) {
    Json rooms = Json::object();
    for (const auto& [room, entry] : document.rooms)
        rooms[room] = {{"epoch", entry.epoch}, {"through", entry.through}, {"used_ms", entry.used_ms}, {"applied", entry.applied}};
    return Json{{"format", 1}, {"slot", document.slot}, {"rooms", rooms}}.dump();
}
void Write(const fs::path& path, std::string_view slot, std::string_view room, Entry entry) {
    auto document = Read(path, slot);
    if (document.corrupt) {
        auto bad = fs::path{path.string() + ".bad"};
        if (fs::exists(bad)) bad += "." + std::to_string(Now());
        fs::copy_file(path, bad); // If evidence cannot be kept, do not replace the original.
    }
    Merge(document, room, std::move(entry));
    AtomicWrite(path, Serialize(document));
}
std::string Digest(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    unsigned int size{};
    if (!EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) || size != digest.size())
        throw std::runtime_error("SHA256 failed");
    std::string hex;
    for (auto byte : digest) hex += fmt::format("{:02x}", byte);
    return hex;
}
HashResult ModHash(const fs::path& keys, const fs::path& program_load) {
    HashResult result;
    auto [size, hash] = Fingerprint(keys);
    result.text = fmt::format("oot-coop-mod-hash 1\nkeys {} {}\n", size, hash);
    result.bytes = size;
    for (const auto* name : {"coop-ux.json", "coop-anim.json", "coop-equip.json", "coop-ledger.json"}) {
        const auto path = keys.parent_path() / name;
        if (!fs::exists(fs::symlink_status(path))) result.text += fmt::format("sidecar {} absent\n", name);
        else {
            auto [bytes, sha] = Fingerprint(path);
            result.bytes += bytes;
            result.text += fmt::format("sidecar {} {} {}\n", name, bytes, sha);
        }
    }
    struct Record { std::string path, hash; u64 size; };
    std::vector<Record> records;
    if (fs::exists(program_load)) for (const auto& mod : fs::directory_iterator(program_load)) {
        if (!mod.is_directory()) continue;
        for (const auto* tree : {"romfs", "exefs"}) {
            const auto root = mod.path() / tree;
            if (!fs::exists(fs::symlink_status(root))) continue;
            std::set<fs::path> ancestors;
            std::vector<fs::path> files;
            Files(root, ancestors, files);
            for (const auto& file : files) {
                const auto utf8 = file.lexically_relative(mod.path()).generic_u8string();
                std::string relative{utf8.begin(), utf8.end()};
                if (relative.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("newline in mod path");
                auto [bytes, sha] = Fingerprint(file);
                result.bytes += bytes;
                records.push_back({relative, sha, bytes});
            }
        }
    }
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        // std::string's char_traits compares bytewise, including non-ASCII UTF-8 paths.
        return std::tie(a.path, a.hash) < std::tie(b.path, b.hash);
    });
    for (const auto& record : records) result.text += fmt::format("mod {} {} {}\n", record.path, record.size, record.hash);
    result.files = records.size();
    result.digest = Digest(result.text);
    return result;
}

std::optional<std::pair<std::string, u64>> DecodeCapability(std::string_view text) {
    try {
        const auto value = Parse(text);
        if (value.size() != 3 || !U53(value.at("v")) || value.at("v") != 1 ||
            !value.at("epoch").is_string() || !Identifier(value.at("epoch").get<std::string>()) ||
            !U53(value.at("floor"))) return {};
        return std::pair{value.at("epoch").get<std::string>(), value.at("floor").get<u64>()};
    } catch (const std::exception&) { return {}; }
}
std::optional<Page> DecodePage(std::string_view text, std::string_view epoch, u64 after) {
    try {
        if (text.size() > MaxFrame) return {};
        const auto value = Parse(text);
        if (value.size() != 7 || value.at("type") != "catchup" || value.at("epoch") != epoch ||
            !U53(value.at("floor")) || !U53(value.at("through")) || !U53(value.at("after")) ||
            value.at("after") != after || !value.at("more").is_boolean() ||
            !value.at("grants").is_array() || value.at("grants").size() > 256) return {};
        Page page{std::string{epoch}, value.at("floor"), value.at("through"), after, value.at("more")};
        if (page.floor > page.through) return {};
        u64 previous = after;
        for (const auto& grant : value.at("grants")) {
            auto change = DecodeChange(grant);
            if (!change || !EventKind(change->kind) || !U53(grant.at("rev"))) return {};
            const auto rev = grant.at("rev").get<u64>();
            if (!rev || rev <= previous || rev > page.through || rev <= page.floor) return {};
            previous = rev;
            change->catchup = true;
            page.grants.push_back(std::move(*change));
        }
        if (page.more && (page.grants.empty() || previous >= page.through)) return {};
        return page;
    } catch (const std::exception&) { return {}; }
}

std::string_view DecisionName(Decision decision) {
    switch (decision) {
    case Decision::Applied: return "already-applied";
    case Decision::Queued: return "queued";
    case Decision::Covered: return "covered";
    case Decision::Admit: return "queued";
    case Decision::Policy: return "policy";
    }
    return "policy";
}
struct Book::Impl {
    mutable std::mutex mutex;
    Entry entry;
    std::set<Id> applied, queued;
    std::set<std::pair<Kind, u64>> local_flags;
    u64 lines{};
    struct Receipt {
        std::string epoch;
        u64 floor{}, through{}, grants{}, queued{}, applied{}, already{}, covered{}, inflight{}, dropped{};
        bool ended{};
        std::set<Id> waiting;
    };
    std::map<u64, Receipt> receipts;
    void Complete() {
        std::erase_if(receipts, [](const auto& pair) {
            const auto& r = pair.second;
            if (!r.ended || !r.waiting.empty()) return false;
            LOG_INFO(Core_ARM, "OoT co-op: catchup: {} grants, {} applied, {} already had{}",
                     r.grants, r.applied, r.already + r.covered + r.inflight,
                     r.applied < r.queued ? fmt::format(" ({} queued grants not applied)", r.queued - r.applied) : "");
            return true;
        });
    }
    void Insert(const Id& id) {
        if (!applied.insert(id).second) return;
        entry.applied.push_back(id);
        while (entry.applied.size() > AppliedLimit) {
            applied.erase(entry.applied.front());
            entry.applied.pop_front();
        }
    }
};
Book::Book() : impl{std::make_unique<Impl>()} {}
Book::~Book() = default;
void Book::Epoch(std::string_view epoch) {
    std::scoped_lock lock{impl->mutex};
    if (!impl->entry.epoch.empty() && impl->entry.epoch != epoch) {
        impl->entry = {};
        impl->applied.clear();
        impl->queued.clear();
        impl->receipts.clear();
    }
    impl->entry.epoch = epoch;
}
void Book::Bind(const std::optional<Entry>& entry) {
    std::scoped_lock lock{impl->mutex};
    if (!entry || entry->epoch != impl->entry.epoch) return;
    // Local outgoing IDs observed before the bind still belong to this running save.
    const auto local = impl->entry.applied;
    impl->entry = *entry;
    impl->entry.applied.clear();
    impl->applied.clear();
    for (const auto& id : entry->applied) impl->Insert(id);
    for (const auto& id : local) impl->Insert(id);
}
Decision Book::Admit(const Change& change, bool live_accepted) {
    std::scoped_lock lock{impl->mutex};
    const Id id{change.origin, change.seq};
    Decision decision = Decision::Admit;
    if (!change.catchup && !live_accepted) decision = Decision::Policy;
    else if (impl->applied.contains(id)) decision = Decision::Applied;
    else if (impl->queued.contains(id)) decision = Decision::Queued;
    else if (change.catchup) {
        bool covered = false;
        if (change.source.kind == World::OriginKind::Chest)
            covered = impl->local_flags.contains({Kind::Bool, change.source.key});
        else if (change.source.kind == World::OriginKind::Bit || change.source.kind == World::OriginKind::Token) {
            const auto flags = World::Flags(change.source);
            covered = std::ranges::all_of(flags, [&](u64 key) { return impl->local_flags.contains({Kind::Bit, key}); });
        }
        if (covered) decision = Decision::Covered;
    }
    if (decision == Decision::Admit) impl->queued.insert(id);
    if (change.catchup) {
        if (impl->lines++ < 256)
            LOG_INFO(Core_ARM, "OoT co-op: catchup grant sender={} seq={} class={} name={} decision={}",
                     change.origin.substr(0, 8), change.seq,
                     change.kind == Kind::Item ? "item" : change.kind == Kind::Rupee ? "rupee" : "key",
                     change.name.empty() ? "-" : change.name, DecisionName(decision));
        auto it = impl->receipts.find(change.catchup_transaction);
        if (it != impl->receipts.end()) {
            auto& r = it->second;
            ++r.grants;
            switch (decision) {
            case Decision::Applied: ++r.already; break;
            case Decision::Queued: ++r.inflight; break;
            case Decision::Covered: ++r.covered; break;
            case Decision::Admit: ++r.queued; r.waiting.insert(id); break;
            case Decision::Policy: ++r.dropped; break;
            }
        }
    }
    return decision;
}
void Book::Applied(const Change& change) {
    std::scoped_lock lock{impl->mutex};
    const Id id{change.origin, change.seq};
    if (id.first.empty() || !id.second) return;
    impl->Insert(id);
    impl->queued.erase(id);
    for (auto& [number, r] : impl->receipts) if (r.waiting.erase(id)) ++r.applied;
    impl->Complete();
}
void Book::Dropped(const Change& change) {
    std::scoped_lock lock{impl->mutex};
    const Id id{change.origin, change.seq};
    impl->queued.erase(id);
    for (auto& [number, r] : impl->receipts) if (r.waiting.erase(id)) ++r.dropped;
    if (change.catchup && impl->lines++ < 256)
        LOG_INFO(Core_ARM, "OoT co-op: catchup grant sender={} seq={} class={} name={} decision=policy",
                 change.origin.substr(0, 8), change.seq,
                 change.kind == Kind::Item ? "item" : change.kind == Kind::Rupee ? "rupee" : "key",
                 change.name.empty() ? "-" : change.name);
    impl->Complete();
}
void Book::Quarantined() {
    std::scoped_lock lock{impl->mutex};
    for (auto& [number, r] : impl->receipts) {
        r.dropped += r.waiting.size();
        r.waiting.clear();
    }
    // Keep ambiguous queued IDs poisoned for this process; quarantine must never retry a call.
    impl->Complete();
}
void Book::LocalFlag(const Change& change) {
    if ((change.kind != Kind::Bool && change.kind != Kind::Bit) || change.value != 1) return;
    std::scoped_lock lock{impl->mutex};
    impl->local_flags.emplace(change.kind, change.key);
}
Entry Book::Snapshot() const {
    std::scoped_lock lock{impl->mutex};
    auto result = impl->entry;
    result.used_ms = Now();
    return result;
}
void Book::Through(u64 revision) {
    std::scoped_lock lock{impl->mutex};
    impl->entry.through = std::max(impl->entry.through, revision);
}
void Book::Begin(u64 transaction, std::string_view epoch, u64 floor, u64 through) {
    std::scoped_lock lock{impl->mutex};
    if (!impl->receipts.contains(transaction) && impl->receipts.size() >= 32) {
        LOG_WARNING(Core_ARM, "OoT co-op: catchup receipt tracking full; oldest unfinished receipt abandoned");
        impl->receipts.erase(impl->receipts.begin());
    }
    auto& r = impl->receipts[transaction];
    r.epoch = epoch;
    r.floor = floor;
    r.through = through;
}
void Book::End(u64 transaction) {
    std::scoped_lock lock{impl->mutex};
    auto it = impl->receipts.find(transaction);
    if (it == impl->receipts.end()) return;
    auto& r = it->second;
    r.ended = true;
    LOG_INFO(Core_ARM, "OoT co-op: catchup received epoch={} floor={} through={} grants={} queued={} already-applied={} covered={} in-flight={}",
             r.epoch, r.floor, r.through, r.grants, r.queued, r.already, r.covered, r.inflight);
    impl->Complete();
}
void Book::Abort(u64 transaction) {
    std::scoped_lock lock{impl->mutex};
    impl->receipts.erase(transaction);
}

struct Save::Impl {
    Book& book;
    Pattern pattern;
    fs::path user;
    std::string room;
    Post post;
    mutable std::mutex mutex;
    std::string last_slot, bound_slot, epoch;
    fs::path last_path, bound_path;
    std::map<std::string, fs::path> paths; // slot -> sidecar path seen with a host path (renames carry none)
    bool enabled{true}, bound{};
    u32 frames{}, lines{};
    Impl(Book& b, Pattern p, fs::path u, std::string r, Post submit)
        : book{b}, pattern{std::move(p)}, user{std::move(u)}, room{std::move(r)}, post{std::move(submit)} {}
};
Save::Save(Book& book, Pattern pattern, fs::path user, std::string room, Post post)
    : impl{std::make_unique<Impl>(book, std::move(pattern), std::move(user), std::move(room), std::move(post))} {
    InstallWatch(this, [this](auto path, auto host, bool write) { Note(path, host, write); });
}
Save::~Save() { RemoveWatch(this); }
void Save::Note(std::string_view path, std::string_view host, bool write) {
    std::scoped_lock lock{impl->mutex};
    if (!impl->enabled) return;
    auto slot = impl->pattern.Match(path);
    if (!slot) {
        // EDEN_NX2_OOT_COOP_LEDGER_TRACE=1: the first 64 unmatched opens, to write the private pattern.
        static const bool trace = [] { const char* v = std::getenv("EDEN_NX2_OOT_COOP_LEDGER_TRACE"); return v && *v == '1'; }();
        if (trace && impl->lines++ < 64)
            LOG_INFO(Core_ARM, "OoT co-op: ledger file op={} unmatched path={} host={}",
                     host.empty() ? "rename" : write ? "write" : "read", path, host);
        return;
    }
    if (impl->lines++ < 64)
        LOG_INFO(Core_ARM, "OoT co-op: ledger save file op={} slot={} path={} host={}",
                 host.empty() ? "rename" : write ? "write" : "read", *slot, path, host);
    auto sidecar = SidecarPath(host, *slot, impl->user);
    if (!host.empty()) impl->paths[*slot] = sidecar;
    else if (const auto known = impl->paths.find(*slot); known != impl->paths.end()) sidecar = known->second;
    if (!write) {
        if (impl->bound && impl->bound_slot != *slot) {
            impl->enabled = false;
            LOG_WARNING(Core_ARM, "OoT co-op: ledger: another save loaded (slot {}); catch-up off until restart", *slot);
        } else if (!impl->bound) {
            impl->last_slot = *slot;
            impl->last_path = sidecar;
        }
        return;
    }
    if (!impl->bound) return; // Loads may open read-write; that is not a save receipt.
    if (impl->bound_slot.empty()) { impl->bound_slot = *slot; impl->bound_path = sidecar; }
    auto entry = impl->book.Snapshot();
    if (entry.epoch.empty()) return; // No negotiated campaign yet.
    impl->post([sidecar, slot = *slot, room = impl->room, entry = std::move(entry)] {
        try {
            Write(sidecar, slot, room, entry);
            LOG_INFO(Core_ARM, "OoT co-op: ledger sidecar written slot={} applied={} path={}", slot, entry.applied.size(), sidecar.string());
        } catch (const std::exception& error) {
            LOG_WARNING(Core_ARM, "OoT co-op: ledger sidecar write failed {}: {}", sidecar.string(), error.what());
        }
    });
}
void Save::Frame(bool in_game) {
    std::scoped_lock lock{impl->mutex};
    if (!impl->enabled || impl->bound) return;
    impl->frames = in_game ? impl->frames + 1 : 0;
    if (impl->frames < 60) return;
    impl->bound_slot = std::exchange(impl->last_slot, {});
    impl->bound_path = std::exchange(impl->last_path, {});
    if (!impl->bound_path.empty() && !impl->epoch.empty())
        impl->book.Bind(Find(Read(impl->bound_path, impl->bound_slot), impl->room, impl->epoch));
    impl->bound = true;
    LOG_INFO(Core_ARM, "OoT co-op: ledger save bound slot={} applied={} sidecar={}",
             impl->bound_slot.empty() ? "new" : impl->bound_slot, impl->book.Snapshot().applied.size(),
             impl->bound_path.empty() ? "none" : impl->bound_path.string());
}
void Save::Epoch(std::string_view epoch) {
    std::scoped_lock lock{impl->mutex};
    if (impl->epoch == epoch) return;
    impl->epoch = epoch;
    impl->book.Epoch(epoch);
    if (impl->bound && !impl->bound_path.empty())
        impl->book.Bind(Find(Read(impl->bound_path, impl->bound_slot), impl->room, epoch));
}
bool Save::Bound() const { std::scoped_lock lock{impl->mutex}; return impl->bound; }
bool Save::Enabled() const { std::scoped_lock lock{impl->mutex}; return impl->enabled; }
void Save::Disable() { std::scoped_lock lock{impl->mutex}; impl->enabled = false; }
} // namespace Core::OotCoop::Ledger
