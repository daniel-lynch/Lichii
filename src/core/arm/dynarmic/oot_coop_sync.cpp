// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

#include <boost/asio.hpp>

#include "common/logging.h"
#include "common/thread.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace Core::OotCoop {
namespace {
using Json = nlohmann::json;
using boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::mutex sessions_mutex;
std::shared_ptr<Session> current_session;

std::string Env(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool Unsigned(const Json& value, u64 maximum) {
    if (value.is_number_unsigned()) {
        return value.get<u64>() <= maximum;
    }
    return value.is_number_integer() && value.get<s64>() >= 0 &&
           static_cast<u64>(value.get<s64>()) <= maximum;
}

bool Signed32(const Json& value) {
    if (value.is_number_unsigned()) {
        return value.get<u64>() <= static_cast<u64>(std::numeric_limits<s32>::max());
    }
    return value.is_number_integer() && value.get<s64>() >= std::numeric_limits<s32>::min() &&
           value.get<s64>() <= std::numeric_limits<s32>::max();
}

bool Identity(const Json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
           });
}

Kind StorageKind(Kind kind) {
    return kind == Kind::PouchAdd ? Kind::PouchTotal : kind;
}

std::string_view Type(Kind kind) {
    switch (kind) {
    case Kind::Bool: return "bool_set";
    case Kind::Int: return "int_set";
    case Kind::Enum: return "enum_set";
    case Kind::Quest: return "quest_step";
    case Kind::Bit: return "bit_set";
    case Kind::PouchAdd: return "pouch_add";
    case Kind::PouchTotal: return "pouch_total";
    }
    return {};
}
} // namespace

Json EncodeChange(const Change& change) {
    Json packet{{"type", Type(change.kind)}};
    if (!change.origin.empty()) {
        packet["origin"] = change.origin;
        packet["seq"] = change.seq;
    }
    if (change.kind == Kind::Quest) {
        packet["challenge"] = change.key;
        packet["step"] = change.step;
        packet["index"] = change.value;
    } else if (change.kind == Kind::Bit) {
        packet["actor"] = change.key;
        packet["value"] = change.value != 0;
    } else {
        packet["hash"] = change.key;
        if (change.kind == Kind::Bool) {
            packet["value"] = change.value != 0;
        } else {
            packet[change.kind == Kind::PouchAdd || change.kind == Kind::PouchTotal ? "count"
                                                                                 : "value"] =
                change.value;
        }
    }
    return packet;
}

std::optional<Change> DecodeChange(const Json& packet, bool snapshot) {
    try {
        if (!packet.is_object() || !packet.at("type").is_string()) {
            return std::nullopt;
        }
        Change change;
        const auto type = packet.at("type").get<std::string>();
        bool found = false;
        for (const Kind kind : {Kind::Bool, Kind::Int, Kind::Enum, Kind::Quest, Kind::Bit,
                                Kind::PouchAdd, Kind::PouchTotal}) {
            if (type == Type(kind)) {
                change.kind = kind;
                found = true;
                break;
            }
        }
        if (!found || (!snapshot && change.kind == Kind::PouchTotal)) {
            return std::nullopt;
        }
        if (!snapshot || packet.contains("origin") || packet.contains("seq")) {
            if (!Identity(packet.at("origin")) || !Unsigned(packet.at("seq"), MaxSequence) ||
                packet.at("seq").get<u64>() == 0) {
                return std::nullopt;
            }
            change.origin = packet.at("origin").get<std::string>();
            change.seq = packet.at("seq").get<u64>();
        }
        const char* key = change.kind == Kind::Quest ? "challenge"
                          : change.kind == Kind::Bit ? "actor" : "hash";
        if (!Unsigned(packet.at(key), change.kind == Kind::Bit
                                          ? std::numeric_limits<u64>::max()
                                          : std::numeric_limits<u32>::max())) {
            return std::nullopt;
        }
        change.key = packet.at(key).get<u64>();
        if (WireKind(change.kind)) {
            for (const auto& [field, value] : packet.items()) {
                if (field == "type" || field == key || field == "value") continue;
                if (!snapshot && (field == "origin" || field == "seq")) continue;
                if (!snapshot && field == "rev" && Unsigned(value, MaxSequence) && value != 0) continue;
                return std::nullopt;
            }
        }
        if (change.kind == Kind::Bool || change.kind == Kind::Bit) {
            if (!packet.at("value").is_boolean()) {
                return std::nullopt;
            }
            change.value = packet.at("value").get<bool>() ? 1 : 0;
        } else if (change.kind == Kind::Quest) {
            if (!Unsigned(packet.at("index"), 255) ||
                !Unsigned(packet.at("step"), std::numeric_limits<u32>::max())) {
                return std::nullopt;
            }
            change.value = packet.at("index").get<s64>();
            change.step = packet.at("step").get<u32>();
        } else {
            const bool pouch = change.kind == Kind::PouchAdd || change.kind == Kind::PouchTotal;
            const auto& value = packet.at(pouch ? "count" : "value");
            if (change.kind == Kind::PouchTotal ? !Unsigned(value, MaxSequence) : !Signed32(value)) {
                return std::nullopt;
            }
            change.value = value.get<s64>();
            if (pouch && change.value <= 0) {
                return std::nullopt;
            }
        }
        return change;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::optional<Json> DecodeLine(std::string_view line) {
    if (line.empty() || line.size() > MaxFrame || line.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    // Bound nesting before parsing (JSON strings can contain brackets).
    size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char c : line) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            if (++depth > 16) return std::nullopt;
        } else if (c == ']' || c == '}') {
            if (depth == 0) return std::nullopt;
            --depth;
        }
    }
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key &&
                 !keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
        return true;
    };
    auto packet = Json::parse(line, callback, false);
    if (duplicate || !packet.is_object()) {
        return std::nullopt;
    }
    return packet;
}

bool WireKind(Kind kind) {
    return kind == Kind::Bool || kind == Kind::Bit;
}

std::optional<std::set<Kind>> DecodeAllowClasses(std::string_view text) {
    std::set<Kind> result;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto name = text.substr(0, comma);
        if (name == "bool") result.insert(Kind::Bool);
        else if (name == "bit") result.insert(Kind::Bit);
        else return std::nullopt;
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (text.empty()) return std::nullopt;
    }
    return result;
}

std::optional<u32> FrameLength(std::string_view header) {
    if (header.size() != 4) return std::nullopt;
    u32 length = 0;
    for (const unsigned char byte : header) length = (length << 8) | byte;
    return length > 0 && length <= MaxFrame ? std::optional<u32>{length} : std::nullopt;
}

std::string EncodeFrame(const Json& packet) {
    auto payload = packet.dump();
    if (!packet.is_object() || payload.empty() || payload.size() > MaxFrame) {
        throw std::length_error{"invalid co-op frame"};
    }
    const auto length = static_cast<u32>(payload.size());
    std::string frame(4, '\0');
    for (size_t i = 0; i < 4; ++i) frame[i] = static_cast<char>(length >> (24 - i * 8));
    return frame + payload;
}

std::optional<Endpoint> ParseEndpoint(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    auto host = text.substr(0, colon);
    const auto port_text = text.substr(colon + 1);
    if (host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    } else if (host.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    unsigned port = 0;
    const auto [end, error] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (host.empty() || host.find_first_of(" \r\n\t/") != std::string_view::npos ||
        port_text.empty() || error != std::errc{} || end != port_text.data() + port_text.size() ||
        port == 0 || port > 65535) {
        return std::nullopt;
    }
    return Endpoint{std::string{host}, static_cast<u16>(port)};
}

bool Gate(std::string_view endpoint, u64 program_id, std::string_view build_id) {
    return ParseEndpoint(endpoint).has_value() && program_id == OotGameDataTrace::ProgramId &&
           build_id == OotGameDataTrace::BuildId;
}

bool Enabled() {
    static const bool enabled = ParseEndpoint(Env("EDEN_NX2_OOT_COOP")).has_value();
    return enabled;
}

Json EncodeHello(const Hello& hello) {
    return {{"type", "hello"}, {"version", ProtocolVersion}, {"build_id", hello.build_id},
            {"seed_hash", hello.seed_hash}, {"policy_id", hello.policy_id}, {"room", hello.room},
            {"team", hello.team},
            {"client_id", hello.client_id}, {"name", hello.name}};
}

bool MatchesHello(const Json& packet, const Hello& hello) {
    try {
        return packet.at("type") == "hello_ack" &&
               Unsigned(packet.at("version"), ProtocolVersion) &&
               packet.at("version") == ProtocolVersion && packet.at("build_id") == hello.build_id &&
               packet.at("seed_hash") == hello.seed_hash && packet.at("policy_id") == hello.policy_id &&
               packet.at("client_id") == hello.client_id;
    } catch (const Json::exception&) {
        return false;
    }
}

std::optional<std::set<u64>> DecodeDenyList(const Json& packet) {
    if (!packet.is_array() || packet.size() > StateLimit) return std::nullopt;
    std::set<u64> denied;
    for (const auto& key : packet) {
        if (!Unsigned(key, std::numeric_limits<u64>::max())) return std::nullopt;
        denied.insert(key.get<u64>());
    }
    return denied;
}

bool State::Merge(const Change& change, bool deduplicate) {
    if (deduplicate) {
        if (change.origin.empty() || change.seq == 0 || change.seq > MaxSequence) return false;
        if (!seen.contains(change.origin) && seen.size() >= StateLimit) return false;
        auto& high = seen[change.origin];
        if (change.seq <= high) return false;
        high = change.seq;
    }
    if (denied.contains(change.key)) return false;
    const auto key = std::pair{StorageKind(change.kind), change.key};
    const auto it = values.find(key);
    if (it == values.end() && values.size() >= StateLimit) return false;
    const s64 old = it == values.end() ? 0 : it->second.value;
    Change next = change;
    switch (change.kind) {
    case Kind::Bool:
    case Kind::Bit:
        if (change.value != 1 || old == 1) return false;
        break;
    case Kind::Quest:
        if (change.value < 0 || change.value > 255 ||
            (it != values.end() && change.value <= old)) return false;
        break;
    case Kind::Int:
    case Kind::Enum:
        if (change.value < std::numeric_limits<s32>::min() ||
            change.value > std::numeric_limits<s32>::max() ||
            (it != values.end() && change.value == old)) return false;
        break;
    case Kind::PouchAdd:
        if (change.value <= 0 || change.value > std::numeric_limits<s32>::max() ||
            old > static_cast<s64>(MaxSequence) - change.value) return false;
        next.kind = Kind::PouchTotal;
        next.value += old;
        break;
    case Kind::PouchTotal:
        if (change.value <= old || change.value > static_cast<s64>(MaxSequence)) return false;
        break;
    }
    values[key] = std::move(next);
    return true;
}

bool State::AdoptSeen(const HighWater& high_water) {
    size_t added = 0;
    for (const auto& [origin, seq] : high_water) {
        if (!Identity(origin) || seq > MaxSequence) return false;
        added += seen.contains(origin) ? 0 : 1;
    }
    if (seen.size() + added > StateLimit) return false;
    for (const auto& [origin, seq] : high_water) {
        auto& current = seen[origin];
        current = std::max(current, seq);
    }
    return true;
}

std::optional<Change> State::Find(Kind kind, u64 key) const {
    const auto it = values.find({StorageKind(kind), key});
    return it == values.end() ? std::nullopt : std::optional<Change>{it->second};
}

std::vector<Change> State::Snapshot() const {
    std::vector<Change> result;
    for (const auto& [key, value] : values) {
        auto change = value;
        change.origin.clear();
        change.seq = 0;
        result.push_back(std::move(change));
    }
    return result;
}

bool ShouldApply(const Change& change, const State& observed) {
    const auto local = observed.Find(change.kind, change.key);
    if (change.kind == Kind::Bool || change.kind == Kind::Bit) {
        return change.value == 1 && (!local || local->value == 0);
    }
    if (change.kind == Kind::Quest) {
        return change.value >= 0 && change.value <= 255 && (!local || change.value > local->value);
    }
    if (change.kind == Kind::Int || change.kind == Kind::Enum) {
        return !local || local->value != change.value;
    }
    // Pouch totals require a save baseline/application receipt, not a comparison to inventory.
    return change.kind == Kind::PouchAdd && change.value > 0;
}

thread_local u32 IncomingScope::depth{};
IncomingScope::IncomingScope() { ++depth; }
IncomingScope::~IncomingScope() { --depth; }
bool IncomingScope::SuppressSend() { return depth != 0; }

ApplyStatus GuestWriter::Apply(const Change& change) const {
    const auto it = std::find_if(Sites.begin(), Sites.end(), [&](const Site& site) {
        return site.kind == change.kind;
    });
    if (it == Sites.end() || it->offset == 0) return ApplyStatus::NotAppliedNoSite;
    if constexpr (!GuestWritesVerified) {
        return ApplyStatus::NotAppliedUnverified;
    }
    return strategy == Strategy::GuestCall ? InjectCall(change) : WriteStorage(change);
}

ApplyStatus GuestWriter::InjectCall(const Change&) const {
    // TODO full CPU context + guest thread ownership, setter ABI, x0..x7, sentinel LR, restore.
    return ApplyStatus::NotAppliedNoSite;
}

ApplyStatus GuestWriter::WriteStorage(const Change&) const {
    // TODO storage layout, manager lifetime, synchronization, notifications and save dirty bits.
    return ApplyStatus::NotAppliedNoSite;
}

struct Session::Impl {
    struct Connection;
    Impl(Endpoint endpoint_, Hello hello_, std::set<u64> denied_, std::set<Kind> allowed_)
        : endpoint{std::move(endpoint_)}, hello{std::move(hello_)}, denied{std::move(denied_)},
          local{denied}, remote{denied}, allowed_classes{std::move(allowed_)}, retry{io} {}
    ~Impl() { Stop(); }
    void Start();
    void Stop();
    void Connect();
    bool Receive(const Json& packet);

    Endpoint endpoint;
    Hello hello;
    std::set<u64> denied;
    std::mutex mutex;
    State local;
    State remote;
    std::set<Kind> allowed_classes;
    std::deque<Change> outgoing;
    std::deque<std::variant<Change, HighWater>> pending;
    u64 next_seq{1};
    bool logged_no_site{};
    bool logged_diagnostic{};
    bool logged_class{};
    bool logged_overflow{};
    std::atomic<bool> stopped{};
    boost::asio::io_context io;
    boost::asio::steady_timer retry;
    std::shared_ptr<Connection> connection;
    std::thread thread;
};

struct Session::Impl::Connection : std::enable_shared_from_this<Connection> {
    explicit Connection(Impl& owner_) : owner{owner_}, resolver{owner.io}, socket{owner.io},
                                        timer{owner.io} {}

    void Start() {
        Tick();
        resolver.async_resolve(owner.endpoint.host, std::to_string(owner.endpoint.port),
            [self = shared_from_this()](auto error, tcp::resolver::results_type endpoints) {
                if (self->failed) return;
                if (error) return self->Fail();
                boost::asio::async_connect(self->socket, endpoints,
                    [self](auto connect_error, const tcp::endpoint&) {
                        if (self->failed) return;
                        if (connect_error) return self->Fail();
                        self->Send(EncodeHello(self->owner.hello));
                        self->Read();
                    });
            });
    }

    void Fail() {
        if (std::exchange(failed, true)) return;
        resolver.cancel();
        boost::system::error_code ignored;
        socket.close(ignored);
        timer.cancel();
        owner.retry.expires_after(2s);
        owner.retry.async_wait([self = shared_from_this()](auto error) {
            if (!error && !self->owner.stopped) self->owner.Connect();
        });
    }

    void Tick() {
        timer.expires_after(50ms);
        timer.async_wait([self = shared_from_this()](auto error) {
            if (error || self->failed) return;
            const auto now = Clock::now();
            if (now - self->last_receive > (self->ready ? 90s : 10s)) return self->Fail();
            if (self->ready) {
                std::vector<Change> changes;
                {
                    std::scoped_lock lock{self->owner.mutex};
                    for (const auto& change : self->owner.outgoing) {
                        if (change.seq > self->sent_seq && changes.size() < 64) {
                            changes.push_back(change);
                        }
                    }
                }
                for (const auto& change : changes) {
                    self->Send(EncodeChange(change));
                    self->sent_seq = change.seq;
                }
                if (now - self->last_heartbeat >= 30s) {
                    self->Send({{"type", "heartbeat"}});
                    self->last_heartbeat = now;
                }
            }
            self->Tick();
        });
    }

    void Send(const Json& packet) {
        if (failed) return;
        if (output.size() >= QueueLimit) return Fail();
        const bool writing = !output.empty();
        output.push_back(EncodeFrame(packet));
        if (!writing) Write();
    }

    void Write() {
        boost::asio::async_write(socket, boost::asio::buffer(output.front()),
            [self = shared_from_this()](auto error, size_t) {
                if (self->failed) return;
                if (error) return self->Fail();
                self->output.pop_front();
                if (!self->output.empty()) self->Write();
            });
    }

    void Read() {
        boost::asio::async_read(socket, boost::asio::buffer(header),
            [self = shared_from_this()](auto error, size_t) {
                if (self->failed) return;
                if (error) return self->Fail();
                const auto length = FrameLength({self->header.data(), self->header.size()});
                if (!length) return self->Fail();
                self->input.resize(*length);
                self->ReadPayload();
            });
    }

    void ReadPayload() {
        boost::asio::async_read(socket, boost::asio::buffer(input),
            [self = shared_from_this()](auto error, size_t) {
                if (self->failed) return;
                if (error) return self->Fail();
                const auto packet = DecodeLine(self->input);
                if (!packet) return self->Fail();
                try {
                    self->last_receive = Clock::now();
                    const auto type = packet->at("type").get<std::string>();
                    if (!self->handshaken) {
                        if (!MatchesHello(*packet, self->owner.hello)) {
                            LOG_WARNING(Core_ARM, "OoT co-op: handshake refused or mismatched");
                            return self->Fail();
                        }
                        self->handshaken = true;
                    } else if (type == "heartbeat") {
                        self->Send({{"type", "heartbeat_ack"}});
                    } else if (type == "heartbeat_ack" || type == "client_state") {
                        // Presence has no guest representation in this draft.
                    } else if (type == "error") {
                        LOG_WARNING(Core_ARM, "OoT co-op: relay rejected a packet");
                        return self->Fail();
                    } else {
                        if (!self->ready && type != "team_state") return self->Fail();
                        if (!self->owner.Receive(*packet)) return self->Fail();
                        if (!self->ready) {
                            self->ready = true;
                            self->Send({{"type", "client_state"},
                                        {"state", {{"apply_enabled", false}}}});
                            LOG_INFO(Core_ARM, "OoT co-op: connected; guest application disabled");
                        }
                    }
                } catch (const std::exception&) {
                    return self->Fail();
                }
                self->Read();
            });
    }

    Impl& owner;
    tcp::resolver resolver;
    tcp::socket socket;
    boost::asio::steady_timer timer;
    Clock::time_point last_receive{Clock::now()};
    Clock::time_point last_heartbeat{Clock::now()};
    bool failed{};
    bool handshaken{};
    bool ready{};
    u64 sent_seq{};
    std::array<char, 4> header{};
    std::string input;
    std::deque<std::string> output;
};

void Session::Impl::Start() {
    Connect();
    thread = std::thread{[this] {
        Common::SetCurrentThreadName("OotCoop");
        try {
            io.run();
        } catch (const std::exception& error) {
            stopped = true;
            LOG_WARNING(Core_ARM, "OoT co-op stopped: {}", error.what());
        }
    }};
}

void Session::Impl::Stop() {
    stopped = true;
    io.stop();
    if (thread.joinable()) thread.join();
    if (connection) {
        boost::system::error_code ignored;
        connection->resolver.cancel();
        connection->socket.close(ignored);
        connection->timer.cancel();
    }
    retry.cancel();
}

void Session::Impl::Connect() {
    if (stopped) return;
    connection = std::make_shared<Connection>(*this);
    connection->Start();
}

bool Session::Impl::Receive(const Json& packet) {
    const auto type = packet.at("type").get<std::string>();
    if (type == "ack") {
        if (!Unsigned(packet.at("seq"), MaxSequence) || packet.at("seq") == 0) return false;
        const u64 seq = packet.at("seq").get<u64>();
        std::scoped_lock lock{mutex};
        if (seq >= next_seq) return false;
        while (!outgoing.empty() && outgoing.front().seq <= seq) outgoing.pop_front();
        return true;
    }
    std::vector<Change> changes;
    HighWater snapshot_seen;
    if (type == "team_state") {
        if (!Unsigned(packet.at("revision"), MaxSequence) || !packet.at("changes").is_array() ||
            !packet.at("replay").is_array() || !packet.at("seen").is_object() ||
            packet.at("changes").size() > StateLimit || packet.at("replay").size() > 512 ||
            packet.at("seen").size() > StateLimit) return false;
        State checkpoint{denied};
        for (const auto& [origin, value] : packet.at("seen").items()) {
            if (!Identity(origin) || !Unsigned(value, MaxSequence)) return false;
            snapshot_seen.emplace(origin, value.get<u64>());
        }
        for (const auto& value : packet.at("changes")) {
            const auto change = DecodeChange(value, true);
            if (!change || !WireKind(change->kind)) return false;
            checkpoint.Merge(*change, false);
        }
        u64 revision = packet.at("revision").get<u64>();
        for (const auto& value : packet.at("replay")) {
            const auto change = DecodeChange(value);
            if (!change || !WireKind(change->kind) || !Unsigned(value.at("rev"), MaxSequence) ||
                value.at("rev").get<u64>() != ++revision) return false;
            checkpoint.Merge(*change);
        }
        changes = checkpoint.Snapshot();
    } else {
        const auto change = DecodeChange(packet);
        if (!change || !WireKind(change->kind) || !Unsigned(packet.at("rev"), MaxSequence) ||
            packet.at("rev") == 0) return false;
        changes.push_back(*change);
    }
    std::scoped_lock lock{mutex};
    if (pending.size() + changes.size() + (type == "team_state" ? 1 : 0) > QueueLimit) {
        if (!logged_overflow) {
            LOG_WARNING(Core_ARM, "OoT co-op: pending queue full; reconnect for a fresh snapshot");
            logged_overflow = true;
        }
        return false;
    }
    for (auto& change : changes) pending.emplace_back(std::move(change));
    if (type == "team_state") pending.emplace_back(std::move(snapshot_seen));
    return true;
}

Session::Session(const Kernel::KProcess* owner_, u64 base_, Endpoint endpoint, Hello hello,
                 std::set<u64> denied, bool played_events_, std::set<Kind> allowed_classes)
    : owner{owner_}, base{base_}, played_events{played_events_},
      impl{std::make_unique<Impl>(std::move(endpoint), std::move(hello), std::move(denied),
                                   std::move(allowed_classes))} {
    impl->Start();
}
Session::~Session() = default;
void Session::Stop() { impl->Stop(); }

void Session::Observe(Change change) {
    if (impl->stopped || IncomingScope::SuppressSend() || !WireKind(change.kind)) return;
    std::scoped_lock lock{impl->mutex};
    if (impl->outgoing.size() >= QueueLimit || impl->next_seq > MaxSequence) {
        if (!impl->logged_overflow) {
            LOG_WARNING(Core_ARM, "OoT co-op: outgoing queue full; observation skipped");
            impl->logged_overflow = true;
        }
        return;
    }
    if (!impl->local.Merge(change, false)) return;
    change.origin = impl->hello.client_id;
    change.seq = impl->next_seq++;
    impl->outgoing.push_back(std::move(change));
}

void Session::ObserveDiagnostic() {
    std::scoped_lock lock{impl->mutex};
    if (!impl->logged_diagnostic) {
        LOG_INFO(Core_ARM, "OoT co-op: observation only; setter request is not a grant receipt");
        impl->logged_diagnostic = true;
    }
}

void Session::Drain() {
    if (impl->stopped || IncomingScope::SuppressSend()) return;
    // Only called from the guest breakpoint callback; the I/O thread never accesses guest state.
    std::scoped_lock lock{impl->mutex};
    const GuestWriter writer;
    for (size_t count = 0; count < 64 && !impl->pending.empty(); ++count) {
        auto entry = std::move(impl->pending.front());
        impl->pending.pop_front();
        if (const auto* seen = std::get_if<HighWater>(&entry)) {
            if (!impl->remote.AdoptSeen(*seen)) {
                LOG_WARNING(Core_ARM, "OoT co-op stopped: snapshot sender capacity exceeded");
                impl->stopped = true;
                impl->io.stop();
            }
            continue;
        }
        const auto& change = std::get<Change>(entry);
        if (!impl->remote.Merge(change, !change.origin.empty())) continue;
        // TODO read authoritative guest values here before enabling either writer. Observations
        // alone cannot account for pre-session saves, local resets or rejected setter requests.
        if (change.kind != Kind::PouchTotal && !ShouldApply(change, impl->local)) continue;
        if (!impl->allowed_classes.contains(change.kind)) {
            if (!impl->logged_class) {
                LOG_INFO(Core_ARM, "OoT co-op: not applied: grant class disabled");
                impl->logged_class = true;
            }
            continue;
        }
        IncomingScope incoming;
        const auto status = writer.Apply(change);
        if (status != ApplyStatus::Applied && !impl->logged_no_site) {
            LOG_INFO(Core_ARM, "OoT co-op: not applied: no site (guest writes disabled)");
            impl->logged_no_site = true;
        }
    }
}

void BeginSession() {
    std::shared_ptr<Session> old;
    {
        std::scoped_lock lock{sessions_mutex};
        old = std::exchange(current_session, nullptr);
    }
    if (old) old->Stop();
}

void RegisterMain(const Kernel::KProcess* owner, u64 program_id, u64 base,
                  std::string_view build_id) {
    BeginSession();
    const auto endpoint_text = Env("EDEN_NX2_OOT_COOP");
    if (!Gate(endpoint_text, program_id, build_id) || base == 0) {
        if (!endpoint_text.empty()) LOG_INFO(Loader, "OoT co-op off: endpoint/title/build gate");
        return;
    }
    try {
        std::set<u64> denied;
        const auto path = Env("EDEN_NX2_OOT_COOP_DENY_LIST");
        if (!path.empty()) {
            std::ifstream in{std::filesystem::path{path}};
            if (!in) throw std::runtime_error{"cannot read deny-list"};
            std::string text;
            char c;
            while (in.get(c) && text.size() < MaxFrame) text.push_back(c);
            const auto parsed = DecodeLine("{\"deny\":" + text + "}");
            const auto list = parsed ? DecodeDenyList(parsed->at("deny")) : std::nullopt;
            if (!list) throw std::runtime_error{"invalid deny-list"};
            denied = *list;
        }
        const auto allowed = DecodeAllowClasses(Env("EDEN_NX2_OOT_COOP_ALLOW_CLASSES"));
        if (!allowed) throw std::runtime_error{"invalid grant class allow-list"};
        Hello hello;
        hello.seed_hash = Env("EDEN_NX2_OOT_COOP_SEED", "0");
        hello.policy_id = Env("EDEN_NX2_OOT_COOP_POLICY", "or-v2");
        hello.room = Env("EDEN_NX2_OOT_COOP_ROOM", "default");
        hello.team = Env("EDEN_NX2_OOT_COOP_TEAM", "default");
        hello.name = Env("EDEN_NX2_OOT_COOP_NAME", "Player");
        std::random_device random;
        hello.client_id = fmt::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
        auto session = std::make_shared<Session>(owner, base, *ParseEndpoint(endpoint_text),
            std::move(hello), std::move(denied), false, *allowed);
        std::scoped_lock lock{sessions_mutex};
        current_session = std::move(session);
    } catch (const std::exception& error) {
        LOG_WARNING(Loader, "OoT co-op off: {}", error.what());
    }
}

std::shared_ptr<Session> FindSession(const Kernel::KProcess* owner) {
    std::scoped_lock lock{sessions_mutex};
    return current_session && current_session->owner == owner ? current_session : nullptr;
}

} // namespace Core::OotCoop
