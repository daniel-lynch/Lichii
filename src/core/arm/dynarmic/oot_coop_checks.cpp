// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <tuple>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/arm/dynarmic/oot_coop_checks.h"

namespace Core::OotCoop::Checks {
namespace {
constexpr size_t NarrowWidth = 36, DenseWidth = 52;
constexpr size_t NarrowRows = 10, DenseRows = 22;
constexpr u64 PollMs = 1000, EvaluateMs = 500;
constexpr size_t FileLimit = 8 * 1024 * 1024;

std::string Ascii(std::string_view s, size_t max) {
    std::string out;
    for (const unsigned char c : s) {
        if (out.size() == max) break;
        out += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::optional<u64> Hex(const nlohmann::json& value, size_t digits) {
    if (!value.is_string()) return {};
    const auto& s = value.get_ref<const std::string&>();
    if (s.size() < 3 || s.size() > 2 + digits || !s.starts_with("0x")) return {};
    u64 key{};
    const auto [end, ec] = std::from_chars(s.data() + 2, s.data() + s.size(), key, 16);
    if (ec != std::errc{} || end != s.data() + s.size() || !key) return {};
    return key;
}

std::optional<Flag> DecodeFlag(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 1) return {};
    if (value.contains("bool")) {
        const auto key = Hex(value["bool"], 8);
        return key ? std::optional{Flag{FlagKind::Bool, *key}} : std::nullopt;
    }
    if (value.contains("bit")) {
        const auto key = Hex(value["bit"], 16);
        return key ? std::optional{Flag{FlagKind::Bit, *key}} : std::nullopt;
    }
    return {};
}

std::optional<Check> DecodeCheck(const nlohmann::json& v) {
    if (!v.is_object()) return {};
    const auto text = [&](const char* key) -> std::optional<std::string> {
        if (!v.contains(key) || !v[key].is_string()) return {};
        return v[key].get<std::string>();
    };
    const auto id = text("id");
    const auto name = text("name");
    if (!id || id->empty() || id->size() > 128 || !name || name->empty()) return {};
    if (!v.contains("bancs") || !v["bancs"].is_array() || v["bancs"].empty() || v["bancs"].size() > BancLimit)
        return {};
    Check c;
    c.id = *id;
    c.name = Ascii(*name, NameLimit);
    c.area = Ascii(text("area").value_or(std::string{}), NameLimit);
    for (const auto& banc : v["bancs"]) {
        if (!banc.is_string() || banc.get_ref<const std::string&>().empty() ||
            banc.get_ref<const std::string&>().size() > 128) return {};
        c.bancs.push_back(banc.get<std::string>());
    }
    if (v.contains("shuffled")) {
        if (!v["shuffled"].is_boolean()) return {};
        c.shuffled = v["shuffled"].get<bool>();
    }
    if (v.contains("flag")) {
        const auto flag = DecodeFlag(v["flag"]);
        if (!flag) return {};
        c.flag = *flag;
    }
    return c;
}

std::string ReadText(const std::filesystem::path& path, size_t limit) {
    std::ifstream input{path, std::ios::binary};
    if (!input) return {};
    std::string text;
    text.resize(limit + 1);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(input.gcount()));
    return text.size() > limit ? std::string{} : text;
}

bool Off(std::string_view v) { return v == "0" || v == "off" || v == "false" || v == "no"; }
bool On(std::string_view v) { return v.empty() || v == "1" || v == "on" || v == "true" || v == "yes"; }

struct Global {
    std::mutex mutex;
    std::shared_ptr<const Panel> panel;
    u64 generation{};
};
Global& Shared() {
    static Global g;
    return g;
}
} // namespace

bool Check::In(std::string_view banc) const {
    return std::find(bancs.begin(), bancs.end(), banc) != bancs.end();
}

std::optional<Sidecar> Decode(const nlohmann::json& d, std::string* why) {
    const auto fail = [&](std::string_view reason) -> std::optional<Sidecar> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (!d.is_object()) return fail("not an object");
    if (!d.contains("schema") || d["schema"] != 1) return fail("schema is not 1");
    if (!d.contains("checks") || !d["checks"].is_array()) return fail("no checks array");
    if (d["checks"].size() > CheckLimit) return fail("too many checks");
    Sidecar s;
    if (d.contains("seed") && d["seed"].is_string()) s.seed = Ascii(d["seed"].get<std::string>(), 64);
    if (d.contains("file_hash") && d["file_hash"].is_string()) s.file_hash = Ascii(d["file_hash"].get<std::string>(), 64);
    if (d.contains("areas") && d["areas"].is_object())
        for (const auto& [banc, name] : d["areas"].items())
            if (banc.size() <= 128 && name.is_string() && s.areas.size() < 1024)
                s.areas[banc] = Ascii(name.get<std::string>(), NameLimit);
    size_t skipped = 0;
    std::map<std::string, bool, std::less<>> ids;
    for (const auto& value : d["checks"]) {
        auto check = DecodeCheck(value);
        if (!check || ids.contains(check->id)) {
            ++skipped;
            continue;
        }
        ids[check->id] = true;
        s.checks.push_back(std::move(*check));
    }
    if (s.checks.empty()) return fail("no valid checks");
    if (why) *why = skipped ? fmt::format("{} bad checks skipped", skipped) : std::string{};
    return s;
}

bool DecodeSpoiler(const nlohmann::json& d, Sidecar& s, std::string* why) {
    const auto fail = [&](std::string_view reason) {
        if (why) *why = reason;
        return false;
    };
    if (!d.is_object() || !d.contains("schema") || d["schema"] != 1) return fail("schema is not 1");
    if (!d.contains("file_hash") || !d["file_hash"].is_string() || s.file_hash.empty() ||
        d["file_hash"].get<std::string>() != s.file_hash) return fail("file_hash does not match the checks file");
    if (!d.contains("items") || !d["items"].is_object()) return fail("no items object");
    std::map<std::string, std::string, std::less<>> items;
    for (const auto& check : s.checks) {
        const auto it = d["items"].find(check.id);
        if (it != d["items"].end() && it->is_string()) items[check.id] = Ascii(it->get<std::string>(), NameLimit);
    }
    s.items = std::move(items);
    return true;
}

Options ParseOptions(std::string_view text) {
    Options o;
    text = text.substr(0, 256);
    size_t at = 0;
    while (at < text.size()) {
        const size_t start = text.find_first_not_of(" \t\r\n,", at);
        if (start == std::string_view::npos) break;
        const size_t end = std::min(text.find_first_of(" \t\r\n,", start), text.size());
        const auto word = text.substr(start, end - start);
        if (word == "all") o.all = true;
        else if (word == "spoiler") o.spoiler = true;
        else if (word == "vanilla") o.vanilla = true;
        else if (word == "dense") o.dense = o.all = true;
        at = end;
    }
    return o;
}

Model::Model(Sidecar sidecar_) : sidecar{std::move(sidecar_)}, done(sidecar.checks.size(), false) {}

State Model::StateOf(size_t index) const {
    if (index >= sidecar.checks.size()) return State::Untracked;
    if (!sidecar.checks[index].Tracked()) return State::Untracked;
    return done[index] ? State::Done : State::Open;
}

void Model::Set(size_t index, std::optional<bool> value) {
    if (index < done.size() && value && sidecar.checks[index].Tracked()) done[index] = *value;
}

Counts Model::Here(std::string_view banc, bool vanilla) const {
    Counts c;
    for (size_t i = 0; i < sidecar.checks.size(); ++i) {
        const auto& check = sidecar.checks[i];
        if ((!vanilla && !check.shuffled) || !check.In(banc)) continue;
        if (!check.Tracked()) ++c.untracked;
        else {
            ++c.tracked;
            c.done += done[i];
        }
    }
    return c;
}

Counts Model::Total(bool vanilla) const {
    Counts c;
    for (size_t i = 0; i < sidecar.checks.size(); ++i) {
        const auto& check = sidecar.checks[i];
        if (!vanilla && !check.shuffled) continue;
        if (!check.Tracked()) ++c.untracked;
        else {
            ++c.tracked;
            c.done += done[i];
        }
    }
    return c;
}

std::string Model::AreaName(std::string_view banc) const {
    if (banc.empty()) return {};
    if (const auto it = sidecar.areas.find(banc); it != sidecar.areas.end()) return it->second;
    for (const auto& check : sidecar.checks)
        if (check.In(banc) && !check.area.empty()) return check.area;
    return {};
}

std::vector<Row> Model::Rows(std::string_view banc, bool loading, const Options& o) const {
    const size_t width = o.dense ? DenseWidth : NarrowWidth;
    const size_t budget = o.dense ? DenseRows : NarrowRows;
    const bool list_done = o.all || o.dense;
    const auto total = Total(o.vanilla);
    const auto here = Here(banc, o.vanilla);
    std::string area = loading ? std::string{"Loading"} : AreaName(banc);
    if (area.empty()) area = banc.empty() ? std::string{"Checks"} : Ascii(banc, NameLimit);
    const auto suffix = loading || banc.empty()
        ? fmt::format("  total {}/{}", total.done, total.tracked)
        : fmt::format("  {}/{}  total {}/{}", here.done, here.tracked, total.done, total.tracked);
    const size_t room = width > suffix.size() + 4 ? width - suffix.size() : 4;
    std::vector<Row> out{{Ascii(area, room) + suffix, Style::Title}};
    if (loading || banc.empty()) return out;
    std::vector<size_t> open, untracked, closed;
    for (size_t i = 0; i < sidecar.checks.size(); ++i) {
        const auto& check = sidecar.checks[i];
        if ((!o.vanilla && !check.shuffled) || !check.In(banc)) continue;
        const auto state = StateOf(i);
        (state == State::Open ? open : state == State::Untracked ? untracked : closed).push_back(i);
    }
    const auto by_name = [&](size_t a, size_t b) {
        return std::tie(sidecar.checks[a].name, sidecar.checks[a].id) < std::tie(sidecar.checks[b].name, sidecar.checks[b].id);
    };
    for (auto* group : {&open, &untracked, &closed}) std::sort(group->begin(), group->end(), by_name);
    if (open.empty() && untracked.empty() && closed.empty()) {
        out.push_back({"No checks in this area", Style::Note});
        if (o.dense) Elsewhere(out, banc, area, budget - 1, o);
        return out;
    }
    std::vector<std::pair<size_t, Style>> listed;
    for (const auto i : open) listed.emplace_back(i, Style::Open);
    for (const auto i : untracked) listed.emplace_back(i, Style::Untracked);
    if (list_done) for (const auto i : closed) listed.emplace_back(i, Style::Done);
    const bool use_items = o.spoiler && !sidecar.items.empty();
    const size_t shown = listed.size() > budget ? budget - 1 : listed.size();
    for (size_t n = 0; n < shown; ++n) {
        const auto [i, style] = listed[n];
        const auto& check = sidecar.checks[i];
        const char* mark = style == Style::Open ? "[ ] " : style == Style::Done ? "[x] " : "[?] ";
        std::string name = check.name;
        std::string text;
        const auto item = use_items ? sidecar.items.find(check.id) : sidecar.items.end();
        if (use_items && item != sidecar.items.end()) {
            const size_t fixed = 4 + 3 + item->second.size();
            const size_t keep = width > fixed + 12 ? width - fixed : 12;
            text = fmt::format("{}{} - {}", mark, Ascii(name, keep), item->second);
        } else {
            text = mark + name;
        }
        out.push_back({Ascii(text, width), style});
    }
    if (shown < listed.size()) out.push_back({fmt::format("  +{} more", listed.size() - shown), Style::Note});
    else if (!list_done && open.empty() && untracked.empty())
        out.push_back({fmt::format("All {} done here", closed.size()), Style::Note});
    else if (o.dense) Elsewhere(out, banc, area, budget - shown, o);
    return out;
}

void Model::Elsewhere(std::vector<Row>& out, std::string_view banc, std::string_view here, size_t room,
                      const Options& o) const {
    // Open checks of the other areas, by area name, each under an area row with its counts. The
    // current area stays on top; an area that shares its name (the other age's banc) is left out.
    std::map<std::string, std::vector<size_t>, std::less<>> areas;
    std::map<std::string, Counts, std::less<>> counts;
    for (size_t i = 0; i < sidecar.checks.size(); ++i) {
        const auto& check = sidecar.checks[i];
        if ((!o.vanilla && !check.shuffled) || check.In(banc) || !check.Tracked()) continue;
        std::string name = check.area.empty() ? Ascii(check.bancs.empty() ? std::string{} : check.bancs[0], NameLimit)
                                              : check.area;
        if (name.empty() || name == here) continue;
        auto& c = counts[name];
        ++c.tracked;
        c.done += done[i];
        if (!done[i]) areas[name].push_back(i);
    }
    std::vector<Row> rows;
    size_t total = 0;
    for (auto& [name, list] : areas) {
        std::sort(list.begin(), list.end(), [&](size_t a, size_t b) {
            return std::tie(sidecar.checks[a].name, sidecar.checks[a].id) <
                   std::tie(sidecar.checks[b].name, sidecar.checks[b].id);
        });
        const auto& c = counts[name];
        const auto suffix = fmt::format("  {}/{}", c.done, c.tracked);
        const size_t width = DenseWidth > suffix.size() + 4 ? DenseWidth - suffix.size() : 4;
        rows.push_back({Ascii(name, width) + suffix, Style::Title});
        for (const auto i : list) rows.push_back({Ascii("[ ] " + sidecar.checks[i].name, DenseWidth), Style::Open});
        total += list.size();
    }
    if (rows.empty() || room < 2) return;
    if (rows.size() <= room) {
        out.insert(out.end(), rows.begin(), rows.end());
        return;
    }
    size_t take = room - 1; // the last row says how many are left
    if (rows[take - 1].style == Style::Title) --take;  // never end on an area row
    size_t listed = 0;
    for (size_t n = 0; n < take; ++n) listed += rows[n].style == Style::Open;
    out.insert(out.end(), rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(take));
    out.push_back({fmt::format("  +{} more in other areas", total - listed), Style::Note});
}

void Publish(std::vector<Row> rows, bool dense) {
    auto& g = Shared();
    std::scoped_lock lock{g.mutex};
    if (rows.empty()) {
        g.panel.reset();
        return;
    }
    if (g.panel && g.panel->rows == rows && g.panel->dense == dense) return;
    g.panel = std::make_shared<const Panel>(Panel{std::move(rows), dense, ++g.generation});
}

std::shared_ptr<const Panel> Current() {
    auto& g = Shared();
    std::scoped_lock lock{g.mutex};
    return g.panel;
}

void Reset() {
    auto& g = Shared();
    std::scoped_lock lock{g.mutex};
    g.panel.reset();
}

std::string TriggerPath(const std::function<std::string(std::string_view)>& env) {
    if (auto path = env("EDEN_NX2_OOT_COOP_CHECKS_TRIGGER"); !path.empty()) return path;
    const auto keys = env("EDEN_NX2_OOT_COOP_KEYS");
    return keys.empty() ? std::string{} : (std::filesystem::path{keys}.parent_path() / "coop-checks.show").string();
}

Window::~Window() {
    if (published) Publish({}, false);
}

void Window::Initialize() {
    initialized = true;
    const char* setting = std::getenv("EDEN_NX2_OOT_COOP_CHECKS");
    const std::string_view value = setting ? setting : "";
    const char* keys = std::getenv("EDEN_NX2_OOT_COOP_KEYS");
    const std::filesystem::path dir = keys && *keys ? std::filesystem::path{keys}.parent_path() : std::filesystem::path{};
    if (Off(value)) {
        LOG_INFO(Core_ARM, "OoT co-op: checks overlay off (EDEN_NX2_OOT_COOP_CHECKS={})", value);
        return;
    }
    sidecar_path = On(value) ? (dir.empty() ? std::string{} : (dir / "coop-checks.json").string()) : std::string{value};
    std::error_code error;
    if (sidecar_path.empty() || !std::filesystem::is_regular_file(sidecar_path, error)) {
        LOG_INFO(Core_ARM, "OoT co-op: checks overlay off (no sidecar{}{})", sidecar_path.empty() ? "" : " at ",
                 sidecar_path);
        return;
    }
    enabled = true;
    const char* window = std::getenv("EDEN_NX2_OOT_COOP_CHECKS_WINDOW");
    mode = window && (std::string_view{window} == "on" || std::string_view{window} == "off") ? window : "trigger";
    trigger = TriggerPath([](std::string_view name) {
        const char* v = std::getenv(std::string{name}.c_str());
        return std::string{v ? v : ""};
    });
    LOG_INFO(Core_ARM, "OoT co-op: checks overlay mode {} (trigger {})", mode, trigger.empty() ? "-" : trigger);
}

void Window::Load() {
    loaded = true;
    const auto document = nlohmann::json::parse(ReadText(sidecar_path, FileLimit), nullptr, false);
    std::string why;
    auto sidecar = Decode(document, &why);
    if (!sidecar) {
        enabled = false;
        LOG_WARNING(Core_ARM, "OoT co-op: checks sidecar rejected ({}); checks overlay off", why);
        return;
    }
    const auto spoiler_path = std::filesystem::path{sidecar_path}.parent_path() / "coop-checks-spoiler.json";
    std::error_code error;
    std::string spoiler_why = "absent";
    if (std::filesystem::is_regular_file(spoiler_path, error)) {
        const auto spoiler = nlohmann::json::parse(ReadText(spoiler_path, FileLimit), nullptr, false);
        if (DecodeSpoiler(spoiler, *sidecar, &spoiler_why)) spoiler_why = fmt::format("{} items", sidecar->items.size());
    }
    size_t tracked = 0;
    for (const auto& check : sidecar->checks) tracked += check.Tracked();
    LOG_INFO(Core_ARM, "OoT co-op: checks sidecar loaded: seed {} ({}), {} checks, {} tracked{}; spoiler {}",
             sidecar->seed.empty() ? "-" : sidecar->seed, sidecar->file_hash.empty() ? "-" : sidecar->file_hash,
             sidecar->checks.size(), tracked, why.empty() ? "" : fmt::format(" ({})", why), spoiler_why);
    model = std::make_unique<Model>(std::move(*sidecar));
}

void Window::Frame(const Reader& read, std::string_view banc, bool loading, u64 now) {
    if (!initialized) Initialize();
    if (!enabled) return;
    if (!polled || now - *polled >= PollMs) {
        polled = now;
        std::error_code error;
        const bool visible = mode == "on" ||
            (mode == "trigger" && !trigger.empty() && std::filesystem::exists(trigger, error));
        Options next = options;
        if (visible && mode == "trigger") next = ParseOptions(ReadText(trigger, 256 * 4));
        if (visible && !loaded) Load();
        if (!enabled || !model) return;
        if (visible != shown || next != options) {
            if (visible != shown)
                LOG_INFO(Core_ARM, "OoT co-op: checks overlay {}{}{}{}{}", visible ? "show" : "hide",
                         next.all ? " all" : "", next.spoiler ? " spoiler" : "", next.vanilla ? " vanilla" : "",
                         next.dense ? " dense" : "");
            shown = visible;
            options = next;
            evaluated.reset();
            if (!shown) {
                rows.clear();
                if (std::exchange(published, false)) Publish({}, false);
            }
        }
    }
    if (!shown || !model || (evaluated && now - *evaluated < EvaluateMs)) return;
    evaluated = now;
    if (!loading && !banc.empty()) model->Evaluate(read);
    auto next = model->Rows(banc, loading, options);
    if (next != rows || !published) {
        rows = std::move(next);
        Publish(rows, options.dense);
        published = true;
    }
}

} // namespace Core::OotCoop::Checks
