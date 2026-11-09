// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include "core/arm/dynarmic/oot_coop_chest.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace Core::OotCoop::Chest {
namespace {
std::optional<u32> Key(const Change& change) {
    if (change.source.kind != World::OriginKind::Chest || !World::Valid(change.source)) return {};
    return static_cast<u32>(change.source.key);
}
bool ReceiptShape(const Change& change) {
    return !change.origin.empty() && change.chest_part < Claims::PartLimit &&
           (change.chest_transaction || (!change.chest_part && !change.chest_final));
}
u64 Transaction(const Change& change) { return change.chest_transaction; }
} // namespace

bool DecodeConfig(const nlohmann::json& value, Config& config) {
    if (!value.is_object()) return false;
    Config out;
    const std::map<std::string, u64*> wide{
        {"interaction_pc", &out.interaction_pc}, {"confirm_pc", &out.confirm_pc},
        {"return_pc", &out.return_pc}, {"selector_vtable", &out.selector_vtable},
        {"input_offset", &out.input_offset}, {"reserved_offset", &out.reserved_offset},
        {"current_offset", &out.current_offset}, {"flags_offset", &out.flags_offset},
        {"metadata_offset", &out.metadata_offset}, {"metadata_key_offset", &out.metadata_key_offset}};
    const std::map<std::string, u32*> narrow{
        {"interaction_word", &out.interaction_word}, {"confirm_word", &out.confirm_word},
        {"return_word", &out.return_word}, {"struct_hash", &out.struct_hash},
        {"input_value", &out.input_value}, {"input_pointer_mask", &out.input_pointer_mask}};
    for (const auto& [field, item] : value.items()) {
        if (field == "enforce") {
            if (!item.is_boolean()) return false;
            out.enforce = item.get<bool>();
        } else if (const auto wide_found = wide.find(field); wide_found != wide.end()) {
            const auto number = World::Number(item, std::numeric_limits<u64>::max());
            if (!number) return false;
            *wide_found->second = *number;
        } else if (const auto narrow_found = narrow.find(field); narrow_found != narrow.end()) {
            const auto number = World::Number(item, std::numeric_limits<u32>::max());
            if (!number) return false;
            *narrow_found->second = static_cast<u32>(*number);
        } else if (field == "invalid_child" || field == "transition_mask") {
            const auto number = World::Number(item, std::numeric_limits<u8>::max());
            if (!number) return false;
            (field == "invalid_child" ? out.invalid_child : out.transition_mask) = static_cast<u8>(*number);
        } else if (field == "owner_chain" || field == "link") {
            const size_t size = field == "owner_chain" ? out.owner_chain.size() : out.link.size();
            if (!item.is_array() || item.size() != size) return false;
            for (size_t i = 0; i < size; ++i) {
                const auto number = World::Number(item[i], std::numeric_limits<u64>::max());
                if (!number) return false;
                (field == "owner_chain" ? out.owner_chain[i] : out.link[i]) = *number;
            }
        } else return false;
    }
    for (const auto& [field, target] : wide) {
        if (!value.contains(field)) return false;
        const bool code = field.ends_with("_pc") || field == "selector_vtable";
        if (*target > (code ? u64{0x10000000} : u64{0xFFFF})) return false;
    }
    for (const auto& [field, target] : narrow) {
        if (!value.contains(field)) return false;
    }
    for (const auto field : {"invalid_child", "transition_mask", "owner_chain", "link"})
        if (!value.contains(field)) return false;
    for (u64 offset : out.owner_chain) if (offset > 0xFFFF) return false;
    for (u64 offset : out.link) if (offset > 0xFFFF) return false;
    // A partial enabled profile would make an observation look authoritative.
    if (!out.interaction_pc || !out.confirm_pc || !out.return_pc || !out.interaction_word ||
        !out.confirm_word || !out.return_word || !out.struct_hash || !out.selector_vtable ||
        !out.input_offset || !out.reserved_offset || !out.current_offset || !out.flags_offset ||
        !out.metadata_offset || !out.metadata_key_offset || !out.transition_mask ||
        !std::ranges::all_of(out.owner_chain, [](u64 offset) { return offset != 0; }) ||
        !std::ranges::all_of(out.link, [](u64 offset) { return offset != 0; })) return false;
    config = out;
    return true;
}

EventToken Events::Sample(std::optional<u64> pointer) {
    if (!pointer) return {};
    if (!*pointer) {
        for (const auto& [address, token] : active) {
            if (ended.size() < Limit) ended.push_back(token);
            else overflowed = true;
        }
        active.clear();
        return {};
    }
    if (const auto found = active.find(*pointer); found != active.end()) return found->second;
    if (active.size() >= Limit || generation == std::numeric_limits<u64>::max()) {
        overflowed = true;
        return {};
    }
    if (active.empty()) ++generation;
    const EventToken token{*pointer, generation};
    active.emplace(*pointer, token);
    return token;
}
std::vector<EventToken> Events::TakeEnded() {
    auto result = std::move(ended);
    ended.clear();
    return result;
}

void Claims::SelectLocal(ChestRecord& record, u64 id) {
    record.selected = true;
    record.claim_order = ++next_order;
    record.attempt = id;
    record.transaction = id;
    record.transaction_selected = true;
}
ChestRecord* Claims::Get(u32 key) {
    if (!key) return nullptr;
    const auto found = records.find(key);
    if (found != records.end()) return &found->second;
    return records.size() < Limit ? &records.try_emplace(key).first->second : nullptr;
}
const ChestRecord* Claims::Find(u32 key) const {
    const auto found = records.find(key);
    return found == records.end() ? nullptr : &found->second;
}
const Attempt* Claims::FindAttempt(u64 id) const {
    const auto found = attempts.find(id);
    return found == attempts.end() ? nullptr : &found->second;
}
u64 Claims::LocalBegin(const Identity& identity, u64 epoch, u64 tick) {
    if (!identity.key || !identity.token.actor || !identity.token.record || identity.token.epoch != epoch)
        return 0;
    if (const auto found = FindActor(identity.token)) {
        return attempts.at(*found).identity.key == identity.key ? *found : 0;
    }
    if (attempts.size() >= Limit || next_attempt == std::numeric_limits<u64>::max()) return 0;
    auto* record = Get(identity.key);
    if (!record) return 0;
    const u64 id = ++next_attempt;
    Attempt attempt;
    attempt.id = id;
    attempt.tick = tick;
    attempt.identity = identity;
    attempts.emplace(id, std::move(attempt));
    actors.emplace(identity.token, id);
    if (!record->selected) SelectLocal(*record, id);
    return id;
}
bool Claims::BindEvent(u64 id, EventToken event) {
    const auto found = attempts.find(id);
    if (found == attempts.end() || found->second.ended || !event || ambiguous.contains(event)) return false;
    auto& attempt = found->second;
    if (attempt.event && *attempt.event != event) return false;
    attempt.event = event;
    const auto [binding, fresh] = events.try_emplace(event, id);
    if (!fresh && binding->second != id) {
        if (ambiguous.size() < Limit) ambiguous.insert(event);
        return false;
    }
    attempt.event = event;
    return true;
}
bool Claims::LocalActive(u32 key) const {
    return std::ranges::any_of(attempts, [key](const auto& entry) {
        return !entry.second.ended && entry.second.identity.key == key;
    });
}
std::optional<u64> Claims::FindActor(ActorToken actor) const {
    const auto found = actors.find(actor);
    return found == actors.end() ? std::nullopt : std::optional{found->second};
}
std::optional<u64> Claims::FindEvent(EventToken event) const {
    if (!event || ambiguous.contains(event)) return {};
    const auto found = events.find(event);
    return found == events.end() ? std::nullopt : std::optional{found->second};
}
std::optional<u64> Claims::Attribute(std::optional<ActorToken> actor, EventToken event) const {
    const auto by_actor = actor ? FindActor(*actor) : std::nullopt;
    const auto by_event = FindEvent(event);
    if (ambiguous.contains(event) || (by_actor && by_event && by_actor != by_event)) return {};
    return by_actor ? by_actor : by_event;
}
GateResult Claims::Gate(u64 id) {
    const auto found = attempts.find(id);
    if (found == attempts.end() || found->second.ended) return {};
    auto& attempt = found->second;
    auto& record = records.at(attempt.identity.key);
    if (!record.selected) SelectLocal(record, id); // an earlier unpaid attempt released it
    if (!record.source.empty() || record.closed || record.outcome == Outcome::Historical ||
        record.attempt != id) return {Decision::Skip};
    if (attempt.unresolved || record.unresolved || attempt.next_part >= PartLimit) return {};
    const u32 part = attempt.next_part++;
    attempt.applying.insert(part);
    return {Decision::Allow, record.transaction, part};
}
bool Claims::Returned(u64 id, u32 part, bool success) {
    const auto found = attempts.find(id);
    if (found == attempts.end() || !found->second.applying.erase(part)) return false;
    auto& record = records.at(found->second.identity.key);
    if (!success) {
        found->second.unresolved = record.unresolved = true;
        record.outcome = Outcome::Unknown;
        return false;
    }
    record.applied.insert(part);
    if (!record.unresolved) record.outcome = Outcome::LocalApplied;
    return true;
}
void Claims::Complete(u32 key) {
    auto found = records.find(key);
    if (found == records.end() || !found->second.source.empty() || !found->second.attempt) return;
    auto& record = found->second;
    record.closed = true;
    if (record.applied.empty() || !attempts.at(record.attempt).applying.empty()) {
        record.unresolved = true;
        record.outcome = Outcome::Unknown;
    } else record.final_part = *record.applied.rbegin();
}
void Claims::End(u64 id) {
    const auto found = attempts.find(id);
    if (found == attempts.end() || found->second.ended) return;
    auto& attempt = found->second;
    attempt.ended = true;
    actors.erase(attempt.identity.token);
    if (attempt.event) {
        events.erase(*attempt.event);
        ambiguous.erase(*attempt.event);
    }
    auto& record = records.at(attempt.identity.key);
    if (record.attempt != id || !record.source.empty()) return;
    if (!record.closed && record.applied.empty() && record.applying.empty() && attempt.applying.empty()) {
        // No benefit and no completion: a canceled or never-confirmed open. No claim was ever
        // announced (there is no claim packet), so releasing it is local: a later opener
        // (this client or a peer) can still be selected and paid.
        record = ChestRecord{};
        attempts.erase(found);
        return;
    }
    Complete(attempt.identity.key);
}
void Claims::EndEvent(EventToken event) {
    // Ambiguous bindings must all end together, never select one by insertion order.
    std::vector<u64> ended;
    for (const auto& [id, attempt] : attempts) if (attempt.event == event) ended.push_back(id);
    for (u64 id : ended) End(id);
    events.erase(event);
    ambiguous.erase(event);
}
std::vector<u64> Claims::Expire(u64 tick, u64 age) {
    // Only an attempt no event ever confirmed expires: a confirmed one lives until its event's
    // observed end, however long the player leaves the item message up.
    std::vector<u64> expired;
    for (const auto& [id, attempt] : attempts)
        if (!attempt.ended && !attempt.event && attempt.applying.empty() && tick >= attempt.tick &&
            tick - attempt.tick >= age) expired.push_back(id);
    for (u64 id : expired) End(id);
    return expired;
}
bool Claims::Active() const {
    return std::ranges::any_of(attempts, [](const auto& entry) { return !entry.second.ended; });
}
std::vector<u32> Claims::MissingBenefits(u64 tick, u64 age) {
    std::vector<u32> missing;
    for (auto& [key, record] : records) {
        if (!record.selected || record.source.empty() || record.closed || record.missing_logged ||
            record.unresolved || !record.applied.empty() || !record.applying.empty()) continue;
        if (!record.pending_since) record.pending_since = tick;
        if (tick >= *record.pending_since && tick - *record.pending_since >= age) {
            record.missing_logged = true;
            missing.push_back(key);
        }
    }
    return missing;
}
void Claims::RemoteClaim(World::Origin key, const std::string& origin, bool baseline) {
    if (key.kind != World::OriginKind::Chest || !World::Valid(key)) return;
    auto* record = Get(static_cast<u32>(key.key));
    if (!record || record->selected || (!baseline && origin.empty())) return;
    record->selected = true;
    record->claim_order = ++next_order;
    if (baseline) {
        record->closed = true;
        record->outcome = Outcome::Historical;
    } else record->source = origin;
}
bool Claims::AcceptRemote(const Change& change) {
    const auto key = Key(change);
    if (!key) return change.source.kind != World::OriginKind::Chest;
    if (!ReceiptShape(change)) return false;
    auto* record = Get(*key);
    if (!record) return false;
    if (!record->selected) RemoteClaim(change.source, change.origin);
    if (record->source == change.origin && !record->closed && !record->transaction_selected) {
        record->transaction = Transaction(change);
        record->transaction_selected = true;
    }
    return CanApply(change);
}
bool Claims::CanApply(const Change& change) const {
    const auto key = Key(change);
    if (!key) return change.source.kind != World::OriginKind::Chest;
    if (!ReceiptShape(change)) return false;
    const auto* record = Find(*key);
    return record && record->selected && record->source == change.origin && !record->closed &&
           !record->unresolved && record->transaction == Transaction(change) &&
           !record->applied.contains(change.chest_part) && !record->applying.contains(change.chest_part) &&
           (!record->final_part || change.chest_part <= *record->final_part);
}
bool Claims::StartRemote(const Change& change) {
    if (!CanApply(change)) return false;
    const auto key = Key(change);
    if (!key) return true;
    auto& record = records.at(*key);
    record.applying.insert(change.chest_part);
    record.outcome = Outcome::RemoteApplying;
    if (change.chest_final || !change.chest_transaction) record.final_part = change.chest_part;
    return true;
}
void Claims::RemoteReturned(const Change& change, bool success) {
    const auto key = Key(change);
    if (!key) return;
    auto found = records.find(*key);
    if (found == records.end()) return;
    auto& record = found->second;
    if (record.source != change.origin || record.transaction != Transaction(change) ||
        !record.applying.erase(change.chest_part)) return;
    if (!success) {
        record.unresolved = true;
        record.outcome = Outcome::Unknown;
        return;
    }
    record.applied.insert(change.chest_part);
    if (!record.unresolved && record.final_part && record.applied.size() == *record.final_part + 1U) {
        record.closed = true;
        record.outcome = Outcome::RemoteApplied;
    } else if (!record.unresolved) record.outcome = Outcome::Pending;
}
std::string_view Claims::Owner(World::Origin key) const {
    if (key.kind != World::OriginKind::Chest || !World::Valid(key)) return "none";
    const auto* record = Find(static_cast<u32>(key.key));
    if (!record || !record->selected) return "none";
    if (record->outcome == Outcome::Historical) return "historical";
    return record->source.empty() ? "us" : std::string_view{record->source};
}
void Claims::Reset() {
    records.clear(); attempts.clear(); actors.clear(); events.clear(); ambiguous.clear();
    // Attempt numbers never repeat within this object, even across a save/session reset.
}
} // namespace Core::OotCoop::Chest
