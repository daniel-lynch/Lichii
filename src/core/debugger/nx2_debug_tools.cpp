// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_server.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/gpu.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_flash_trigger.h"
#include "video_core/renderer_base.h"

namespace Core::Nx2Debug {

using nlohmann::json;
using namespace std::chrono_literals;

/// Lines the log ring keeps for log_tail.
constexpr size_t RING_LINES = 20000;

struct ToolState {
    explicit ToolState(System& system_, std::string title_, u64 program_id_,
                       std::filesystem::path dump_directory_)
        : system{system_}, title{std::move(title_)}, program_id{program_id_},
          dump_directory{std::move(dump_directory_)} {}

    System& system;
    std::string title;
    u64 program_id;
    std::filesystem::path dump_directory;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    /// Names this session's captures apart from earlier ones in the same dump directory.
    s64 session{std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()};
    std::string log_filter{Settings::values.log_filter.GetValue()};
    u64 captures{};
    u64 screenshots{};
    u64 flash_arms{};
    std::atomic<bool> stopping{};
};

namespace {

std::string Hex(u64 value) {
    return fmt::format("{:016x}", value);
}

json HexList(const std::vector<u64>& values) {
    json list = json::array();
    for (const u64 value : values) {
        list.push_back(Hex(value));
    }
    return list;
}

template <typename T>
T Param(const json& params, const char* name, T fallback, T low, T high) {
    const T value = params.value(name, fallback);
    return (std::clamp)(value, low, high);
}

bool IsVulkan() {
    return Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
}

/// Waits for a future in short slices so a stopping server does not wait out the timeout.
template <typename T>
bool WaitReady(const ToolState& state, std::future<T>& future, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(50ms) != std::future_status::ready) {
        if (state.stopping.load(std::memory_order_relaxed) ||
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    return true;
}

void AppendPng(void* context, void* data, int size) {
    auto& png = *static_cast<std::vector<u8>*>(context);
    const u8* const bytes = static_cast<const u8*>(data);
    png.insert(png.end(), bytes, bytes + size);
}

json FlashReport(const VideoCore::Nx2Debug::FlashStatus& flash) {
    json captures = json::array();
    for (const auto& capture : flash.captures) {
        captures.push_back({
            {"directory", capture.directory.generic_string()},
            {"trigger_frame", capture.trigger_frame},
            {"capture_frame", capture.capture_frame},
            {"frame_delay", capture.capture_frame - capture.trigger_frame},
            {"trigger_white_fraction", capture.trigger_white_fraction},
            {"capture_white_fraction", capture.capture_white_fraction
                                           ? json(*capture.capture_white_fraction) : json{}},
            {"complete", capture.complete}, {"error", capture.error}});
    }
    json report = {
        {"armed", flash.armed}, {"reason", flash.reason},
        {"directory", flash.config.directory.generic_string()},
        {"threshold_white_fraction", flash.config.threshold}, {"min_luma", flash.config.min_luma},
        {"samples", flash.samples}, {"dropped_samples", flash.dropped},
        {"cpu_checks", flash.cpu_checks}, {"gpu_checks", flash.gpu_checks},
        {"cpu_mean_ms", flash.cpu_checks ? flash.cpu_total_ms / flash.cpu_checks : 0.0},
        {"cpu_max_ms", flash.cpu_max_ms},
        {"gpu_mean_ms", flash.gpu_checks ? json(flash.gpu_total_ms / flash.gpu_checks) : json{}},
        {"gpu_max_ms", flash.gpu_checks ? json(flash.gpu_max_ms) : json{}},
        {"captures", std::move(captures)},
        {"sample_grid", {64, 36}},
        {"timing_note", "CPU probe submission/analysis and GPU blit/copy timestamps; excludes one-time allocation and full dumps"}};
    if (!flash.config.directory.empty()) {
        std::ofstream file{flash.config.directory / "flash-status.json"};
        file << report.dump(2);
    }
    return report;
}

json CaptureOnFlash(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"flash detection needs the Vulkan renderer"};
    }
    const auto fraction = [&](const char* name) {
        const auto& value = params.at(name);
        if (!value.is_number()) {
            throw std::runtime_error{fmt::format("{} must be a number in [0, 1]", name)};
        }
        const double number = value.get<double>();
        if (!std::isfinite(number) || number < 0 || number > 1) {
            throw std::runtime_error{fmt::format("{} must be in [0, 1]", name)};
        }
        return number;
    };
    const double threshold = fraction("threshold_white_fraction");
    const double min_luma = fraction("min_luma");
    const auto limit = [&](const char* name, u32 fallback, u32 maximum) {
        if (!params.contains(name)) {
            return fallback;
        }
        const auto& value = params.at(name);
        if (!value.is_number_integer() || value < 1 || value > maximum) {
            throw std::runtime_error{fmt::format("{} must be an integer in 1..{}", name, maximum)};
        }
        return value.get<u32>();
    };
    const u32 max_captures = limit("max_captures", 1, 10);
    const u32 timeout = limit("timeout_s", 120, 600);
    const auto directory = state.dump_directory /
                           fmt::format("flash-{}-{:03}", state.session, ++state.flash_arms);
    std::filesystem::create_directories(directory);
    return FlashReport(VideoCore::Nx2Debug::ArmFlash({
        .directory = directory, .threshold = threshold, .min_luma = min_luma,
        .max_captures = max_captures,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeout},
        .buffers = params.value("buffers", false), .sampled = params.value("sampled", false)}));
}

json Disarm(ToolState&, const json&) {
    return FlashReport(VideoCore::Nx2Debug::DisarmFlash());
}

json OptionalFrame(const std::optional<u64>& frame) {
    return frame ? json(*frame) : json{};
}

json CountersReport(const VideoCore::Nx2Debug::FlickerCounters& c) {
    return {{"frames", c.frames}, {"judged", c.judged}, {"duplicates", c.duplicates},
            {"gaps", c.gaps}, {"flashes", c.flashes}, {"flicker_frames", c.flicker_frames},
            {"flicker_tiles", c.flicker_tiles}, {"max_jump", c.max_jump},
            {"max_tile_jump", c.max_tile_jump},
            {"last_flash_frame", OptionalFrame(c.last_flash_frame)},
            {"last_flicker_frame", OptionalFrame(c.last_flicker_frame)},
            {"centre_luma_mean", c.frames ? json(c.luma_sum / static_cast<double>(c.frames)) : json{}}};
}

json MonitorReport(const VideoCore::Nx2Debug::MonitorStatus& m) {
    json capture{};
    if (m.capture) {
        capture = {{"directory", m.capture->directory.generic_string()},
                   {"trigger_frame", m.capture->trigger_frame},
                   {"capture_frame", m.capture->capture_frame},
                   {"flash_jump", m.capture->flash_jump},
                   {"complete", m.capture->complete}, {"error", m.capture->error}};
    }
    return {{"enabled", m.enabled},
            {"flash_jump", m.config.thresholds.flash_jump},
            {"tile_jump", m.config.thresholds.tile_jump},
            {"capture_first", m.config.capture_first},
            {"window", CountersReport(m.window)},
            {"total", CountersReport(m.total)},
            {"first_flash_capture", std::move(capture)},
            {"dropped_samples", m.dropped},
            {"cpu_mean_ms", m.cpu_checks ? m.cpu_total_ms / static_cast<double>(m.cpu_checks) : 0.0},
            {"cpu_max_ms", m.cpu_max_ms},
            {"gpu_mean_ms", m.gpu_checks ? json(m.gpu_total_ms / static_cast<double>(m.gpu_checks)) : json{}},
            {"gpu_max_ms", m.gpu_checks ? json(m.gpu_max_ms) : json{}},
            {"sample_grid", {VideoCore::Nx2Debug::ProbeWidth, VideoCore::Nx2Debug::ProbeHeight}},
            {"tiles", {VideoCore::Nx2Debug::TileColumns, VideoCore::Nx2Debug::TileRows}}};
}

json FlashMonitor(ToolState& state, const json& params) {
    if (!params.value("enable", true)) {
        return MonitorReport(VideoCore::Nx2Debug::StopMonitor());
    }
    if (!IsVulkan()) {
        throw std::runtime_error{"flash monitoring needs the Vulkan renderer"};
    }
    const auto positive = [&](const char* name, double fallback) {
        const double value = params.value(name, fallback);
        if (!std::isfinite(value) || value <= 0 || value > 255) {
            throw std::runtime_error{fmt::format("{} must be in (0, 255] luma", name)};
        }
        return value;
    };
    VideoCore::Nx2Debug::MonitorConfig config{
        .directory = state.dump_directory / fmt::format("flash-monitor-{}", state.session),
        .thresholds = {.flash_jump = positive("flash_jump", 6.0),
                       .tile_jump = positive("tile_jump", 12.0)},
        .capture_first = params.value("capture_first", true),
        .sampled = params.value("sampled", false)};
    return MonitorReport(VideoCore::Nx2Debug::StartMonitor(std::move(config)));
}

json FlashStats(ToolState&, const json& params) {
    return MonitorReport(VideoCore::Nx2Debug::ReadMonitor(params.value("reset", false)));
}

json Status(ToolState& state, const json& params) {
    const auto window = std::chrono::milliseconds{Param<int>(params, "fps_window_ms", 500, 0, 5000)};
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < window &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return {
        {"title", state.title},
        {"program_id", fmt::format("{:016X}", state.program_id)},
        {"build", std::string{Common::g_build_fullname}},
        {"scm_rev", std::string{Common::g_scm_rev}},
        {"switch2", Settings::CurrentProgramIsSwitch2()},
        {"renderer", IsVulkan() ? "vulkan" : "other"},
        {"powered_on", state.system.IsPoweredOn()},
        {"paused", state.system.IsPaused()},
        {"presents", after},
        {"fps", seconds > 0.0 ? static_cast<double>(after - before) / seconds : 0.0},
        {"uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start)
                         .count()},
        {"dump_dir", state.dump_directory.generic_string()},
        {"log_filter", state.log_filter},
        {"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())},
        {"flash", FlashReport(VideoCore::Nx2Debug::ReadFlashStatus())},
    };
}

json Screenshot(ToolState& state, const json& params) {
    auto& renderer = state.system.GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        throw std::runtime_error{"a screenshot is already pending: no frame was presented since"};
    }
    const u32 height = Param<u32>(params, "height", 720, 144, 2160);
    const auto layout = Layout::DefaultFrameLayout(height * 16 / 9, height);
    const auto pixels = std::make_shared<std::vector<u8>>(size_t{layout.width} * layout.height * 4);
    const auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    renderer.RequestScreenshot(
        pixels->data(), [pixels, done](bool invert_y) { done->set_value(invert_y); }, layout);
    const int timeout = Param<int>(params, "timeout_s", 5, 1, 30);
    if (!WaitReady(state, future, std::chrono::seconds{timeout})) {
        throw std::runtime_error{
            fmt::format("no frame was presented within {} s (paused or loading?)", timeout)};
    }
    const bool invert_y = future.get();
    std::vector<u8> rgb(size_t{layout.width} * layout.height * 3);
    for (u32 y = 0; y < layout.height; ++y) {
        const u8* source = pixels->data() + size_t{invert_y ? layout.height - 1 - y : y} * layout.width * 4;
        u8* target = rgb.data() + size_t{y} * layout.width * 3;
        for (u32 x = 0; x < layout.width; ++x, source += 4, target += 3) {
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
        }
    }
    std::vector<u8> png;
    if (!stbi_write_png_to_func(AppendPng, &png, static_cast<int>(layout.width),
                                static_cast<int>(layout.height), 3, rgb.data(),
                                static_cast<int>(layout.width * 3))) {
        throw std::runtime_error{"PNG encoding failed"};
    }
    json result = {{"width", layout.width}, {"height", layout.height}};
    if (params.value("save", false)) {
        std::filesystem::create_directories(state.dump_directory);
        const auto path =
            state.dump_directory / fmt::format("screenshot-{}-{:04}.png", state.session, ++state.screenshots);
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) {
            throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
        }
        result["path"] = path.generic_string();
    }
    if (params.value("inline", true)) {
        result["png_base64"] = Base64(png);
    }
    return result;
}

json LogTail(ToolState&, const json& params) {
    const u64 after = params.value("after", u64{0});
    const size_t limit = Param<size_t>(params, "limit", 200, 1, 5000);
    const std::string contains = params.value("contains", std::string{});
    const std::vector<Common::Log::RingLine> lines = Common::Log::ReadRing(after);
    std::vector<const Common::Log::RingLine*> matched;
    for (const auto& line : lines) {
        if (contains.empty() || line.text.find(contains) != std::string::npos) {
            matched.push_back(&line);
        }
    }
    // From the start the newest lines; after a sequence number the oldest ones, to page forward.
    const bool more = matched.size() > limit;
    if (more) {
        if (after == 0) {
            matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(limit));
        } else {
            matched.resize(limit);
        }
    }
    json text = json::array();
    for (const auto* line : matched) {
        text.push_back(line->text);
    }
    const u64 next = more && after != 0 ? matched.back()->sequence
                     : lines.empty()    ? after
                                        : lines.back().sequence;
    return {{"lines", std::move(text)}, {"next", next}, {"more", more}};
}

json LogFilter(ToolState& state, const json& params) {
    const std::string filter = params.at("filter").get<std::string>();
    Common::Log::Filter parsed;
    parsed.ParseFilterString(filter);
    Common::Log::SetGlobalFilter(parsed);
    json result = {{"previous", state.log_filter}, {"filter", filter}};
    state.log_filter = filter;
    return result;
}

/// One frame of a capture: its frame and summary lines, entry counts and the first `keep`
/// entries.
json ReadCaptureManifest(const std::filesystem::path& directory, size_t keep) {
    json entries = json::array();
    json kinds = json::object();
    json frame;
    json summary;
    size_t total = 0;
    std::ifstream manifest{directory / "manifest.jsonl"};
    for (std::string line; std::getline(manifest, line);) {
        json entry = json::parse(line, nullptr, false);
        if (entry.is_discarded()) {
            continue;
        }
        const std::string kind = entry.value("kind", std::string{});
        if (kind == "frame") {
            frame = std::move(entry);
        } else if (kind == "summary") {
            summary = std::move(entry);
        } else {
            kinds[kind] = kinds.value(kind, 0) + 1;
            if (total++ < keep) {
                entries.push_back(std::move(entry));
            }
        }
    }
    return {
        {"directory", directory.generic_string()},
        {"frame", std::move(frame)},
        {"summary", std::move(summary)},
        {"kinds", std::move(kinds)},
        {"entries_total", total},
        {"entries", std::move(entries)},
        {"index_command",
         fmt::format("python3 -I tools/nx2/frame_dump_index.py \"{}\"", directory.generic_string())},
    };
}

json CaptureFrame(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"frame capture needs the Vulkan renderer"};
    }
    const u64 id = ++state.captures;
    const auto directory = state.dump_directory / fmt::format("capture-{}-{:03}", state.session, id);
    std::filesystem::create_directories(directory);
    const bool sampled = params.value("sampled", false);
    const int timeout = Param<int>(params, "timeout_s", 120, 1, 600);
    // A burst of consecutive frames, as EDEN_NX2_FRAME_DUMP_COUNT asks of the env trigger.
    u32 default_count = 1;
    if (const char* const value = std::getenv("EDEN_NX2_FRAME_DUMP_COUNT")) {
        default_count = static_cast<u32>(std::clamp(std::strtol(value, nullptr, 10), 1L, 8L));
    }
    const u32 count = Param<u32>(params, "count", default_count, 1, 8);
    VideoCore::Nx2Debug::RequestCapture(
        {.id = id, .directory = directory, .sampled = sampled, .count = count});
    const auto finished = VideoCore::Nx2Debug::WaitCapture(id, std::chrono::seconds{timeout});
    if (!finished) {
        VideoCore::Nx2Debug::CancelCapture(id);
        throw std::runtime_error{fmt::format(
            "the capture did not finish within {} s (paused or loading?); if it started, its "
            "files are still being written to {}",
            timeout, directory.generic_string())};
    }
    if (!finished->error.empty()) {
        throw std::runtime_error{finished->error};
    }
    const size_t keep = Param<size_t>(params, "manifest_lines", 200, 0, 5000);
    if (count == 1) {
        json result = ReadCaptureManifest(directory, keep);
        result["sampled"] = sampled;
        return result;
    }
    // A burst: one manifest per frame-<k>/; `manifest_lines` applies to each frame.
    json frames = json::array();
    for (u32 index = 1; index <= count; ++index) {
        frames.push_back(ReadCaptureManifest(directory / fmt::format("frame-{}", index), keep));
    }
    return {
        {"directory", directory.generic_string()},
        {"sampled", sampled},
        {"count", count},
        {"frames", std::move(frames)},
    };
}

json RejectShaders(ToolState&, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"runtime rejection needs the Vulkan renderer"};
    }
    std::vector<u64> hashes;
    for (const json& hash : params.value("hashes", json::array())) {
        hashes.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    const bool reject = params.value("reject", true);
    const u64 generation = VideoCore::Nx2Debug::SetRejected(hashes, reject);
    const int timeout = Param<int>(params, "timeout_s", 5, 0, 60);
    const bool applied = hashes.empty() ||
                         VideoCore::Nx2Debug::WaitRejectApplied(generation, std::chrono::seconds{timeout});
    json result = {{"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())}, {"applied", applied}};
    if (!applied) {
        result["note"] = "no frame was presented yet; the change applies at the next frame end";
    }
    return result;
}

Handlers MakeHandlers(const std::shared_ptr<ToolState>& state) {
    const auto handler = [&state](json (*tool)(ToolState&, const json&)) -> Handler {
        return [state, tool](const json& params) { return tool(*state, params); };
    };
    return {
        {"status", handler(Status)},
        {"screenshot", handler(Screenshot)},
        {"log_tail", handler(LogTail)},
        {"log_filter", handler(LogFilter)},
        {"capture_frame", handler(CaptureFrame)},
        {"reject_shaders", handler(RejectShaders)},
        {"capture_on_flash", handler(CaptureOnFlash)},
        {"disarm", handler(Disarm)},
        {"flash_monitor", handler(FlashMonitor)},
        {"flash_stats", handler(FlashStats)},
    };
}

} // Anonymous namespace

u64 ParseShaderHash(std::string_view text) {
    const std::string_view digits =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    u64 value = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (digits.size() != 16 || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format(
            "bad hash \"{}\": want all 16 hex digits of a native program key (shader dump file "
            "name) or shader hash (frame dump \"shaders\"); a shorter one would match nothing",
            text)};
    }
    return value;
}

Service::Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                 std::string dump_directory)
    : state{std::make_shared<ToolState>(system, std::move(title), program_id,
                                        std::move(dump_directory))} {
    VideoCore::Nx2Debug::BeginSession();
    Common::Log::SetRingCapacity(RING_LINES);
    try {
        server = std::make_unique<Server>(endpoint, MakeHandlers(state));
    } catch (...) {
        VideoCore::Nx2Debug::EndSession();
        throw;
    }
    LOG_INFO(Debug, "Debug server listening on {}:{}, dump dir {}", endpoint.host, server->Port(),
             state->dump_directory.generic_string());
}

Service::~Service() {
    state->stopping.store(true, std::memory_order_relaxed);
    VideoCore::Nx2Debug::EndSession();
    server.reset();
}

std::unique_ptr<Service> MaybeStartService(System& system, std::string title, u64 program_id) {
    const char* const text = std::getenv("EDEN_NX2_DEBUG_SERVER");
    if (text == nullptr || *text == '\0') {
        return nullptr;
    }
    std::string error;
    const auto endpoint = ParseEndpoint(text, error);
    if (!endpoint) {
        LOG_ERROR(Debug, "EDEN_NX2_DEBUG_SERVER={}: {}; the debug server is off", text, error);
        return nullptr;
    }
    const char* const directory = std::getenv("EDEN_NX2_DEBUG_DIR");
    std::string dump_directory = directory != nullptr && *directory != '\0'
        ? std::string{directory}
        : (Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "debug-server").generic_string();
    try {
        return std::make_unique<Service>(system, std::move(title), program_id, *endpoint,
                                         std::move(dump_directory));
    } catch (const std::exception& failure) {
        LOG_ERROR(Debug, "Debug server cannot listen on {}:{}: {}", endpoint->host, endpoint->port,
                  failure.what());
        return nullptr;
    }
}

} // namespace Core::Nx2Debug
