// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>

#include "core/arm/dynarmic/oot_coop_ghost.h"

namespace Core::OotCoop {
namespace {
using Json = nlohmann::json;

bool Origin(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '.' || c == '-';
           });
}

std::optional<float> Finite(const Json& value, double limit) {
    if (!value.is_number() || value.is_boolean()) return {};
    const double v = value.get<double>();
    if (!std::isfinite(v) || std::fabs(v) > limit) return {};
    return static_cast<float>(v);
}
} // namespace

bool BancName(std::string_view name) {
    return name.size() <= 63 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_';
           });
}

bool AnimName(std::string_view name) {
    return name == "idle" || name == "walk" || name == "run";
}

Json EncodePose(const Pose& pose) {
    // Two decimals of a metre / degree is plenty for a statue and keeps the frame small.
    const auto round = [](float v) { return std::round(static_cast<double>(v) * 100.0) / 100.0; };
    Json packet = {{"type", "pose"},           {"t", pose.t},
            {"banc", pose.banc},        {"child", pose.child},
            {"loading", pose.loading},  {"pos", Json::array({round(pose.pos[0]), round(pose.pos[1]), round(pose.pos[2])})},
            {"yaw", round(pose.yaw)},   {"anim", pose.anim},
            {"anim_seq", pose.anim_seq}, {"rate", round(pose.rate)},
            {"phase", std::min(0.99, round(pose.phase))}};
    if (pose.animation) Anim::EncodeState(packet, *pose.animation);
    if (pose.equipment) {
        packet["eq32"] = pose.equipment->word;
        packet["eqseq"] = pose.equipment->sequence;
    }
    return packet;
}

std::optional<Pose> DecodePose(const Json& packet, std::string* origin) {
    try {
        if (!packet.is_object() || packet.at("type") != "pose" ||
            packet.dump().size() > PoseFrameLimit) return {};
        const bool forwarded = origin != nullptr;
        for (const auto& [field, value] : packet.items()) {
            if (field == "type" || field == "t" || field == "banc" || field == "child" ||
                field == "loading" || field == "pos" || field == "yaw" || field == "anim" ||
                field == "anim_seq" || field == "rate" || field == "phase" ||
                field == "av" || field == "aid" || field == "af" || field == "ar" || field == "aseq" ||
                field == "eq32" || field == "eqseq")
                continue;
            if (forwarded && field == "origin") continue;
            return {};
        }
        if (forwarded) {
            if (!Origin(packet.at("origin"))) return {};
            *origin = packet.at("origin").get<std::string>();
        }
        Pose pose;
        const auto& t = packet.at("t");
        if (!t.is_number_unsigned() && !(t.is_number_integer() && t.get<s64>() >= 0)) return {};
        if (t.get<u64>() > 0xFFFFFFFFULL) return {};
        pose.t = static_cast<u32>(t.get<u64>());
        if (!packet.at("banc").is_string() || !packet.at("child").is_boolean() ||
            !packet.at("loading").is_boolean())
            return {};
        pose.banc = packet.at("banc").get<std::string>();
        if (!BancName(pose.banc)) return {};
        pose.child = packet.at("child").get<bool>();
        pose.loading = packet.at("loading").get<bool>();
        if (packet.contains("eq32") || packet.contains("eqseq")) {
            pose.equipment = Equip::DecodePacket(packet, pose.child);
            if (!pose.equipment) return {};
        }
        const auto& pos = packet.at("pos");
        if (!pos.is_array() || pos.size() != 3) return {};
        for (size_t i = 0; i < 3; ++i) {
            const auto v = Finite(pos[i], PoseCoordinateLimit);
            if (!v) return {};
            pose.pos[i] = *v;
        }
        const auto yaw = Finite(packet.at("yaw"), 360.0);
        if (!yaw) return {};
        pose.yaw = *yaw;
        const auto& seq = packet.at("anim_seq");
        if (!packet.at("anim").is_string() || !AnimName(packet.at("anim").get<std::string>()) ||
            !(seq.is_number_unsigned() || (seq.is_number_integer() && seq.get<s64>() >= 0)) ||
            seq.get<u64>() > 0xFFFFFFFFULL)
            return {};
        pose.anim = packet.at("anim").get<std::string>();
        pose.anim_seq = static_cast<u32>(seq.get<u64>());
        const auto rate = Finite(packet.at("rate"), 10.0);
        const auto phase = Finite(packet.at("phase"), 1.0);
        if (!rate || *rate < 0 || !phase || *phase < 0 || *phase >= 1.0f) return {};
        pose.rate = *rate;
        pose.phase = *phase;
        if (packet.contains("av") || packet.contains("aid") || packet.contains("af") ||
            packet.contains("ar") || packet.contains("aseq")) {
            pose.animation = Anim::DecodeState(packet);
            if (!pose.animation) return {};
        }
        return pose;
    } catch (const Json::exception&) {
        return {};
    }
}

Pose Blend(const Pose& a, const Pose& b, float f) {
    f = std::clamp(f, 0.0f, 1.0f);
    Pose out = f < 1.0f ? a : b;
    for (size_t i = 0; i < 3; ++i) out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * f;
    float d = std::fmod(b.yaw - a.yaw, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    float yaw = a.yaw + d * f;
    if (yaw > 180.0f) yaw -= 360.0f;
    if (yaw < -180.0f) yaw += 360.0f;
    out.yaw = yaw;
    return out;
}

} // namespace Core::OotCoop
