#include "web_service_actions.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "../../client/hooks/vendor_banner/vendor_banner_retire.h"
#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../core/settings/rule_text.h"
#include "../../middleware/web_service/messages/opcode1801.h"
#include "../../middleware/web_service/messages/opcode1820.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode402.h"
#include "../../middleware/web_service/messages/opcode403.h"
#include "../../middleware/web_service/messages/opcode406.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode901/opcode901_codec.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../middleware/web_service/messages/opcode904/opcode904_codec.h"
#include "../../state/account/account_state.h"
#include "../../state/account/pursuit_hold.h"
#include "../../state/build_data/items/item_catalog.h"
#include "../../state/build_data/runtime.h"
#include "../../state/build_data/vendors/vendor_catalog.h"
#include "../../state/record_claims/record_claims.h"
#include "../../state/runtime/runtime.h"
#include "../../state/unlocks/unlocks_runtime.h"

namespace sunrise::server::web_service {

namespace {

/** Socket kind the shader model occupies, which is the only kind a shader swap may target. */
constexpr std::uint8_t kEquippedShaderModelSocketKind = 0;
/** Index stored when no definition resolves. The catalog is u16-indexed, so this cannot be one. */
constexpr std::uint32_t kUnavailableDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();

} // namespace

/** Logs one exact correlated equipment response after its Queuez update is staged. */
void report_equip_response(const middleware::web_service::Message& message,
                           std::int32_t family4Version,
                           std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=equipment stage=response opcode=%u transaction=%u "
                                     "family_version=%d bytes=%zu hex=",
                                     static_cast<unsigned>(message.opcode),
                                     static_cast<unsigned>(message.transactionId),
                                     family4Version,
                                     response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final item-creation status pair and the exact Family-4 revision it promises. */
void report_item_acquisition_response(const middleware::web_service::Message& message,
                                      std::int32_t family4Version,
                                      std::uint64_t acquiredInstanceSoid,
                                      std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(acquiredInstanceSoid),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final profile-stack status pair and the exact Family-4 account revision it promises. */
void report_profile_item_acquisition_response(const middleware::web_service::Message& message,
                                              std::int32_t family4Version,
                                              std::uint32_t definitionHash,
                                              std::int32_t quantity,
                                              std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=profile_acquire stage=response result=ok opcode=%u transaction=%u "
                      "family_version=%d definition_hash=0x%08X quantity=%d bytes=%zu hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      family4Version,
                      definitionHash,
                      quantity,
                      response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final dismantle status pair and the exact Family-4 revision it promises. */
void report_item_dismantle_response(const middleware::web_service::Message& message,
                                    std::int32_t family4Version,
                                    std::uint64_t dismantledInstanceSoid,
                                    std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=dismantle stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(dismantledInstanceSoid),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the exact opcode-903 status pair and the item-instance revision it promises. */
void report_socket_plug_response(const middleware::web_service::Message& message,
                                 std::int32_t family4Version,
                                 std::uint64_t targetInstanceSoid,
                                 std::uint8_t socketLane,
                                 std::uint16_t plugDefinitionIndex,
                                 std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket_plug stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX lane=%u plug_definition=%u bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(targetInstanceSoid),
        static_cast<unsigned>(socketLane),
        static_cast<unsigned>(plugDefinitionIndex),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the exact opcode-801 status pair and subclass item revision it promises. */
void report_subclass_selection_response(const middleware::web_service::Message& message,
                                        std::int32_t family4Version,
                                        const state::PendingSubclassSelection& mutation,
                                        std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=subclass_select stage=response result=ok opcode=%u transaction=%u "
                      "family_version=%d instance=0x%llX entry=%u bytes=%zu hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      family4Version,
                      static_cast<unsigned long long>(mutation.subclassInstanceSoid),
                      static_cast<unsigned>(mutation.requestedEntry),
                      response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    // Upper-case hex, which is the form the rest of the log lines use.
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    for (const std::byte byte : response) {
        if (length + 2 >= line.size()) {
            break;
        }
        const unsigned value = std::to_integer<unsigned>(byte);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** One line carries the picked id and whether the selection moved. */
constexpr std::size_t kSelectLineCapacity = 96;

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way. The
 * Family-4 object move follows this call, and the family-zero pair after it.
 * @param message Parsed select-character request.
 * @param outcome Gets the picked key once the selection has moved in State.
 */
void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode504::Request picked;
    if (!middleware::web_service::messages::opcode504::parse_request(message, picked)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws504 stage=parse result=fail");
        return;
    }
    bool changed = false;
    if (!state::set_selected_character(picked.characterSoid, changed)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws504 stage=select result=unknown");
        return;
    }
    outcome.hasSelectedCharacter = true;
    outcome.selectedCharacterSoid = picked.characterSoid;

    std::array<char, kSelectLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=ws504 stage=select result=ok soid=0x%llX changed=%u",
                                      static_cast<unsigned long long>(picked.characterSoid),
                                      static_cast<unsigned>(changed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reads the shared opcode-403/404 SOID descriptor through its codec. */
[[nodiscard]] bool parse_equipment_instance(const middleware::web_service::Message& message,
                                            std::uint64_t& instanceSoid) noexcept {
    middleware::web_service::messages::opcode403::Request request{};
    const bool parsed =
        middleware::web_service::messages::opcode403::parse_request(message, request);
    instanceSoid = request.instanceSoid;
    return parsed;
}

/** Prepares one opcode-403/404 equipment mutation without publishing State early. */
void mutate_equipment(const middleware::web_service::Message& message,
                      bool unequip,
                      Outcome& outcome) noexcept {
    std::uint64_t requestedInstanceSoid = 0;
    if (!parse_equipment_instance(message, requestedInstanceSoid)) {
        std::array<char, 112> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=equipment stage=parse result=fail opcode=%u "
                                        "payload_bytes=%zu",
                                        static_cast<unsigned>(message.opcode),
                                        message.payload.size());
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingEquipmentSwap mutation;
    const bool prepared = unequip
                              ? state::prepare_equipment_unequip(requestedInstanceSoid, mutation)
                              : state::prepare_equipment_swap(requestedInstanceSoid, mutation);
    if (!prepared) {
        std::array<char, 144> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=equipment stage=prepare result=fail opcode=%u action=%s requested=0x%llX",
            static_cast<unsigned>(message.opcode),
            unequip ? "unequip" : "equip",
            static_cast<unsigned long long>(requestedInstanceSoid));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }
    outcome.mutation = mutation;

    std::array<char, 224> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=equipment stage=prepare result=ok opcode=%u action=%s character=0x%llX "
                      "previous=0x%llX requested=0x%llX native_slot=%u moved_items=%zu",
                      static_cast<unsigned>(message.opcode),
                      unequip ? "unequip" : "equip",
                      static_cast<unsigned long long>(mutation.characterSoid),
                      static_cast<unsigned long long>(mutation.previousInstanceSoid),
                      static_cast<unsigned long long>(mutation.requestedInstanceSoid),
                      static_cast<unsigned>(mutation.nativeEquipmentSlot),
                      mutation.movedItemCount);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one exact selected-character opcode-801 subclass node selection. */
void mutate_subclass_selection(const middleware::web_service::Message& message,
                               Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode801::Request request{};
    if (!middleware::web_service::messages::opcode801::parse_request(message, request)) {
        std::array<char, 128> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=ws801 stage=parse result=fail transaction=%u payload_bytes=%zu",
                          static_cast<unsigned>(message.transactionId),
                          message.payload.size());
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingSubclassSelection mutation{};
    if (!state::prepare_subclass_selection(
            request.subclassInstanceSoid, request.socketEntry, mutation)) {
        std::array<char, 160> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws801 stage=prepare result=fail transaction=%u instance=0x%llX entry=%u",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.subclassInstanceSoid),
            static_cast<unsigned>(request.socketEntry));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws801 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "entry=%u socket_list=%u",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.subclassInstanceSoid),
        static_cast<unsigned>(mutation.requestedEntry),
        static_cast<unsigned>(mutation.socketEntryListIndex));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one exact selected-character opcode-903 socket selection. */
void mutate_socket_plug(const middleware::web_service::Message& message,
                        Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode903::Request request{};
    if (!middleware::web_service::messages::opcode903::parse_request(message, request)
        || !request.hasInstance || request.instanceSoid == 0 || request.hasTargetDefinition
        || !request.hasPlugDefinition
        || request.socketIndex >= state::account::inventory::kPlugCapacity) {
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=parse result=fail transaction=%u payload_bytes=%zu has_instance=%u "
            "instance=0x%llX has_target_definition=%u socket=%u has_plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned>(request.hasInstance),
            static_cast<unsigned long long>(request.instanceSoid),
            static_cast<unsigned>(request.hasTargetDefinition),
            request.socketIndex,
            static_cast<unsigned>(request.hasPlugDefinition));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingSocketPlug mutation{};
    if (!state::prepare_socket_plug(request.instanceSoid,
                                    static_cast<std::uint8_t>(request.socketIndex),
                                    request.plugDefinitionIndex,
                                    mutation)) {
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=prepare result=fail transaction=%u instance=0x%llX lane=%u "
            "plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.instanceSoid),
            request.socketIndex,
            static_cast<unsigned>(request.plugDefinitionIndex));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 240> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws903 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "target_definition=%u target_bucket=%u lane=%u plug_definition=%u plug_bucket=%u "
        "equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(mutation.targetEquipped),
        mutation.itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one character-location opcode-1901 socket selection. */
void mutate_equipped_socket_plug(const middleware::web_service::Message& message,
                                 Outcome& outcome) noexcept {
    namespace opcode1901 = middleware::web_service::messages::opcode1901;
    opcode1901::Request request{};
    const bool parsed = opcode1901::parse_request(message, request);
    // A request prepares at most one State mutation, so a run naming several sockets cannot be
    // applied as the one transaction it has to be. It is understood and declined rather than
    // treated as malformed, and the reply now carries that refusal.
    const opcode1901::Replacement& replacement = request.replacements.front();
    if (!parsed || request.replacementCount != 1
        || replacement.modelSocketKind != kEquippedShaderModelSocketKind
        || replacement.auxiliary != 0
        || replacement.socketIndex >= state::account::inventory::kPlugCapacity
        || request.instanceIdentityToken == 0) {
        std::array<char, 256> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=parse result=fail transaction=%u payload_bytes=%zu replacements=%zu "
            "plug_definition=%u canonical_kind=%u model_kind=%u socket=%u auxiliary=0x%llX "
            "equipment_selector=%llu",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            request.replacementCount,
            static_cast<unsigned>(replacement.plugDefinitionIndex),
            static_cast<unsigned>(replacement.canonicalSocketKind),
            static_cast<unsigned>(replacement.modelSocketKind),
            replacement.socketIndex,
            static_cast<unsigned long long>(replacement.auxiliary),
            static_cast<unsigned long long>(request.equipmentSelector));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    const std::uint64_t identityToken = request.instanceIdentityToken;
    state::PendingSocketPlug mutation{};
    if (!state::prepare_character_selector_socket_plug(
            request.instanceIdentityToken,
            static_cast<std::uint8_t>(replacement.socketIndex),
            replacement.plugDefinitionIndex,
            mutation)) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=prepare result=fail transaction=%u equipment_selector=%llu "
            "identity_token=%llu lane=%u plug_definition=%u canonical_kind=%u model_kind=%u "
            "auxiliary=0x%llX",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.equipmentSelector),
            static_cast<unsigned long long>(identityToken),
            replacement.socketIndex,
            static_cast<unsigned>(replacement.plugDefinitionIndex),
            static_cast<unsigned>(replacement.canonicalSocketKind),
            static_cast<unsigned>(replacement.modelSocketKind),
            static_cast<unsigned long long>(replacement.auxiliary));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 288> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1901 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "equipment_selector=%llu identity_token=%llu target_definition=%u target_bucket=%u "
        "lane=%u plug_definition=%u plug_bucket=%u canonical_kind=%u model_kind=%u "
        "auxiliary=0x%llX",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned long long>(request.equipmentSelector),
        static_cast<unsigned long long>(identityToken),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(replacement.canonicalSocketKind),
        static_cast<unsigned>(replacement.modelSocketKind),
        static_cast<unsigned long long>(replacement.auxiliary));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one complete accumulated item-state value from opcode 406. */
void mutate_item_state(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode406::Request request{};
    if (!middleware::web_service::messages::opcode406::parse_request(message, request)) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws406 stage=parse result=fail transaction=%u payload_bytes=%zu instance=0x%llX "
            "definition=%u flags=0x%X",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned long long>(request.instanceSoid),
            static_cast<unsigned>(request.definitionIndex),
            request.flags);
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    const std::uint64_t instanceSoid = request.instanceSoid;
    const std::uint32_t flags = request.flags;
    state::PendingItemState mutation{};
    if (!state::prepare_item_state(instanceSoid, request.definitionIndex, flags, mutation)) {
        return;
    }
    outcome.mutation = mutation;
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws406 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "definition=%u flags_before=0x%X flags_after=0x%X equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        mutation.beforeFlags,
        mutation.afterFlags,
        mutation.targetEquipped ? 1U : 0U,
        mutation.itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Records strict opcode-402 parsing, identity checks, and State preparation outcomes. */
void report_item_dismantle(const middleware::web_service::Message& message,
                           std::string_view result,
                           std::string_view reason,
                           std::uint64_t instanceSoid,
                           std::uint32_t definitionIndex,
                           std::uint32_t definitionHash,
                           std::uint32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws402 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "instance=0x%llX definition_index=%u definition_hash=0x%08X quantity=%u",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        static_cast<unsigned long long>(instanceSoid),
        definitionIndex,
        definitionHash,
        quantity);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Prepares the exact fixed-width opcode-402 Character-inventory removal request. */
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode402::Request request{};
    if (!middleware::web_service::messages::opcode402::parse_request(message, request)) {
        report_item_dismantle(
            message, "fail", "payload_bits", request.instanceSoid, request.definitionIndex, 0, 0);
        return;
    }
    const std::uint64_t instanceSoid = request.instanceSoid;
    const std::uint16_t definitionIndex = request.definitionIndex;
    // The codec owns the value; this alias keeps the dismantle checks below readable.
    constexpr std::uint32_t kSingleQuantity =
        middleware::web_service::messages::opcode402::kSingleQuantity;

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)) {
        report_item_dismantle(
            message, "fail", "definition", instanceSoid, definitionIndex, 0, kSingleQuantity);
        return;
    }
    state::PendingItemDismantle mutation{};
    if (!state::prepare_item_dismantle(instanceSoid, mutation)) {
        report_item_dismantle(message,
                              "fail",
                              "state",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    if (mutation.dismantledItem.definitionHash != definition.definitionHash
        || mutation.dismantledItem.quantity != static_cast<std::int32_t>(kSingleQuantity)) {
        report_item_dismantle(message,
                              "fail",
                              "identity",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    outcome.mutation = mutation;
    report_item_dismantle(message,
                          "ok",
                          "ready",
                          instanceSoid,
                          definitionIndex,
                          definition.definitionHash,
                          kSingleQuantity);
}

/** Records opcode-1801 Triumphs claim requests and the record row each one names. */
void report_record_claim(const middleware::web_service::Message& message,
                         std::string_view result,
                         std::string_view reason,
                         std::uint32_t recordIndex,
                         std::uint32_t completionFlagIndex,
                         std::uint32_t scoreValue) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1801 stage=claim result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "record_index=%u completion_flag_index=%u score=%u total_score=%u claims=%zu",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        recordIndex,
        completionFlagIndex,
        scoreValue,
        state::record_claims::total_score(),
        state::record_claims::count());
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Records strict opcode-1820 parsing, installed mapping, and State preparation outcomes. */
void report_item_acquisition(const middleware::web_service::Message& message,
                             std::string_view result,
                             std::string_view reason,
                             std::uint32_t collectibleIndex,
                             std::uint32_t itemDefinitionIndex,
                             std::uint32_t definitionHash,
                             std::uint64_t instanceSoid) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1820 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "collectible_index=%u item_definition_index=%u definition_hash=0x%08X instance=0x%llX",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        collectibleIndex,
        itemDefinitionIndex,
        definitionHash,
        static_cast<unsigned long long>(instanceSoid));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/**
 * Writes one purchase line.
 *
 * The opcode is carried rather than hard-coded: 901 and 904 share this line, and a quest acquire
 * reporting itself as `ws901` sends anyone reading the log to the wrong decoder.
 *
 * @param opcode Request opcode the line belongs to, 901 or 904.
 * @param result `ok` or `fail`.
 * @param reason Step that decided it.
 * @param vendorIndex Vendor row the request named.
 * @param saleIndex Sale row the request named.
 * @param itemDefinitionIndex Item resolved, when the row resolved.
 */
void report_purchase(std::uint16_t opcode,
                     const char* result,
                     const char* reason,
                     std::int32_t vendorIndex,
                     std::int32_t saleIndex,
                     std::uint16_t itemDefinitionIndex) noexcept {
    std::array<char, 192> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=ws%u stage=purchase result=%s reason=%s vendor=%d "
                                    "sale=%d item=%u",
                                    static_cast<unsigned>(opcode),
                                    result,
                                    reason,
                                    static_cast<int>(vendorIndex),
                                    static_cast<int>(saleIndex),
                                    static_cast<unsigned>(itemDefinitionIndex));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         std::strcmp(result, "ok") == 0 ? core::log::Level::info
                                                        : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Resolves one vendor sale row to the item it sells and the category it belongs to. */
[[nodiscard]] bool resolve_vendor_row(std::int32_t vendorIndex,
                                      std::int32_t rowIndex,
                                      std::uint16_t& itemDefinitionIndex,
                                      std::int32_t& categoryIndex,
                                      const char*& reason) noexcept;

/**
 * Records a reputation rule that named something this build cannot credit.
 *
 * @param reason What the rule got wrong.
 * @param vendorIndex Vendor the rule names.
 * @param categoryIndex Category the rule names.
 * @param definition Progression definition the rule names.
 * @param amount Amount the rule would have credited.
 */
void report_reputation_refusal(const char* reason,
                               std::int32_t vendorIndex,
                               std::int32_t categoryIndex,
                               std::uint16_t definition,
                               std::int32_t amount) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int used = std::snprintf(line.data(),
                                   line.size(),
                                   "ev=reputation stage=credit result=fail reason=%s vendor=%d "
                                   "category=%d definition=%u amount=%d",
                                   reason,
                                   vendorIndex,
                                   categoryIndex,
                                   static_cast<unsigned>(definition),
                                   amount);
    if (used > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(used)});
    }
}

/**
 * Credits vendor reputation for a currency turn-in, in place of granting the row's item.
 *
 * A reputation turn-in is a sale row whose item is a Dummy placeholder: the purchase is meant to
 * run a side effect while the dummy is only what the window draws. Granting it is what put
 * "Gunsmith Rewards" in the player's inventory. The row is recognised by its vendor and category,
 * because a category is what an interaction points at - Banshee's "Increase Reputation" is
 * interaction 37 with vendorCategoryIndex 7, so its turn-ins are the rows whose +100 reads 7.
 *
 * Configured by `vendor_reputation.txt`, one rule per line:
 *   `<vendorIndex> <categoryIndex> <progressionDefinition> <amount> <stepTotal>`
 * All five fields are required. A short line reads its missing fields as zero rather than reaching
 * into the next line, because a field never crosses a line ending, so one malformed rule stays one
 * malformed rule. `stepTotal` is the definition's step size and is reported only, because the
 * client derives the rank itself by walking the steps against the cumulative lane.
 *
 * This is data-driven because the vendor-to-progression mapping is not derivable yet: Sunrise
 * extracts neither the vendor's factionHash nor progression definition hashes, so the one known
 * pair (Banshee 22 -> definition 55) was established empirically by bisection.
 *
 * @param vendorIndex Vendor the purchase names.
 * @param categoryIndex Category of the purchased row, from sale row +100.
 * @return True when this row was a reputation turn-in and its item must NOT be granted.
 */
[[nodiscard]] bool credit_vendor_reputation(std::int32_t vendorIndex,
                                            std::int32_t categoryIndex) noexcept {
    if (vendorIndex < 0 || categoryIndex < 0) {
        return false;
    }
    static std::array<char, core::rule_text::kRuleTextCapacity> text{};
    if (!core::path::read_artifact_text(L"vendor_reputation.txt", text)) {
        return false;
    }
    core::rule_text::Cursor rules{text.data()};
    while (rules.seek_field()) {
        std::array<std::int32_t, 5> field{};
        for (std::int32_t& value : field) {
            value = rules.read_decimal();
        }
        if (field[0] != vendorIndex || field[1] != categoryIndex) {
            continue;
        }
        const auto definition = static_cast<std::uint16_t>(field[2]);
        const std::int32_t amount = field[3];
        const std::int32_t stepTotal = field[4];
        // The definition names a row of a fixed bank, and it comes from an authored file, so it is
        // checked before it is used to read one. A mistyped rule would otherwise read past the
        // bank and report whatever it found as the player's new standing.
        if (definition >= state::build_data::progressions::kDefinitionCapacity) {
            report_reputation_refusal("definition", vendorIndex, categoryIndex, definition, amount);
            return false;
        }
        // A rank does not roll over on its own. It fills to the boundary and stops there, the
        // vendor offers its faction reward, and only claiming that reward releases whatever was
        // earned past the boundary into the next rank. So lane 0 is clamped to the boundary and
        // the excess is parked in lane 2, which the client publishes but does not read.
        const state::unlocks::ProgressionLanes lanes =
            state::unlocks::get().characterProgressions[definition];
        const std::int32_t before = lanes[0];
        // Lane 0 is cumulative: the client walks the progression definition's step totals
        // against it to derive the rank, so it is never reset and never clamped. Lanes 1 and 2
        // produce no visible effect and are left alone.
        state::unlocks::ProgressionLanes next = lanes;
        next[0] = before + amount;
        if (!state::unlocks::set_progression(
                state::build_data::progressions::Scope::character, definition, next)) {
            // Saying the credit landed when the write refused it would send the player looking for
            // reputation that was never stored.
            report_reputation_refusal("write", vendorIndex, categoryIndex, definition, amount);
            return false;
        }
        std::array<char, core::log::kLineCapacity> line{};
        const int used = std::snprintf(
            line.data(),
            line.size(),
            "ev=reputation stage=credit vendor=%d category=%d definition=%u amount=%d shown=%d "
            "step=%d",
            vendorIndex,
            categoryIndex,
            static_cast<unsigned>(definition),
            amount,
            next[0],
            stepTotal);
        if (used > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(used)});
        }
        return true;
    }
    return false;
}

/**
 * Grants one item, given the collectible that owns it and its definition index.
 *
 * Split out of `acquire_item` so a vendor purchase reaches the same grant instead of growing a
 * second acquisition path. The acquisition state is keyed by collectible, so a caller has to arrive
 * with one; `find_collectible_for_item` is how a purchase gets there.
 *
 * @param message Request being answered, for the log line.
 * @param collectibleIndex Collectible that owns the item.
 * @param itemDefinitionIndex Item to grant.
 * @param outcome Receives the prepared mutation on success.
 */
void grant_item_definition(const middleware::web_service::Message& message,
                           std::uint16_t collectibleIndex,
                           std::uint16_t itemDefinitionIndex,
                           Outcome& outcome) noexcept {
    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, definition)) {
        report_item_acquisition(
            message, "fail", "item_definition", collectibleIndex, itemDefinitionIndex, 0, 0);
        return;
    }
    // The same rule the client's vendor-row gate applies, so a row that is still offered can never
    // be one this grant would refuse.
    if (state::account::holds_pursuit(itemDefinitionIndex)) {
        report_item_acquisition(message,
                                "fail",
                                "already_held",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        report_item_acquisition(message,
                                "fail",
                                "item_detail_or_bucket",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            report_item_acquisition(message,
                                    "fail",
                                    "profile_item_instanced",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        state::PendingProfileItemAcquisition mutation{};
        if (!state::prepare_profile_item_acquisition(
                collectibleIndex, definition.definitionHash, mutation)) {
            report_item_acquisition(message,
                                    "fail",
                                    "profile_state",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        outcome.mutation = mutation;
        report_item_acquisition(message,
                                "ok",
                                "profile_ready",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }
    if (bucket.arraySelector != bucket_domain::ArraySelector::character) {
        report_item_acquisition(message,
                                "fail",
                                "unsupported_inventory_array",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    state::PendingItemAcquisition mutation{};
    if (!state::prepare_item_acquisition(collectibleIndex, definition.definitionHash, mutation)) {
        report_item_acquisition(message,
                                "fail",
                                "state",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }
    outcome.mutation = mutation;
    report_item_acquisition(message,
                            "ok",
                            "ready",
                            collectibleIndex,
                            itemDefinitionIndex,
                            definition.definitionHash,
                            mutation.acquiredInstanceSoid);
}

/** Decodes one opcode-1801 Triumphs claim and reports the record it names. */
void claim_record(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    // Deliberately no mutation. The shared reply path answers an action that prepares nothing with
    // the refusal status, so preparing a placeholder here would turn a silently-accepted claim into
    // an explicitly rejected one. Attach the transition here once claimed state is identified.
    namespace records = state::build_data::records;
    middleware::web_service::messages::opcode1801::Request request{};
    if (!middleware::web_service::messages::opcode1801::parse_request(message, request)) {
        report_record_claim(message, "fail", "payload_bits", 0, records::kUnavailableFlagIndex, 0);
        return;
    }
    records::Definition definition{};
    if (!state::build_data::find_record_definition(request.recordIndex, definition)) {
        report_record_claim(message,
                            "fail",
                            "record_definition",
                            request.recordIndex,
                            records::kUnavailableFlagIndex,
                            0);
        return;
    }
    if (definition.completionFlagIndex == records::kUnavailableFlagIndex) {
        // The record carries no completion flag, or its slot has no row in the account bank.
        report_record_claim(message,
                            "fail",
                            "no_completion_flag",
                            request.recordIndex,
                            records::kUnavailableFlagIndex,
                            definition.scoreValue);
        return;
    }
    if (!state::record_claims::claim(definition.completionFlagIndex, definition.scoreValue)) {
        report_record_claim(message,
                            "fail",
                            "flag_index_range",
                            request.recordIndex,
                            definition.completionFlagIndex,
                            definition.scoreValue);
        return;
    }
    // The claim is already in the store, so the account image only has to be sent again.
    outcome.hasRecordClaim = true;
    report_record_claim(message,
                        "ok",
                        "claimed",
                        request.recordIndex,
                        definition.completionFlagIndex,
                        definition.scoreValue);
}

/**
 * Finds the collectible that owns one item definition.
 *
 * A vendor sale row names an item, never a collectible, while the acquisition state is keyed by
 * collectible - so a purchase has to find one, or say plainly that there is none.
 *
 * Plenty of what a vendor sells has none at all: bounties, tokens and quest steps are in no
 * collection. Those are granted by hash instead, under `kNoCollectibleIndex`, which is why a caller
 * passes that sentinel on rather than inventing a key or refusing the grant.
 *
 * @param itemDefinitionIndex Item to look up.
 * @param collectibleIndex Receives the owning collectible row, and is left untouched when none
 *        does, so a caller that seeded it with `kNoCollectibleIndex` still holds that sentinel.
 * @return True when a collectible names this item.
 */
[[nodiscard]] bool find_collectible_for_item(std::uint16_t itemDefinitionIndex,
                                             std::uint16_t& collectibleIndex) noexcept {
    namespace collectibles = state::build_data::collectibles;
    static std::array<collectibles::Definition, collectibles::kDefinitionCapacity> rows{};
    std::size_t count = 0;
    if (!collectibles::snapshot(rows, count)) {
        return false;
    }
    for (std::size_t row = 0; row < count; ++row) {
        if (rows[row].itemDefinitionIndex == itemDefinitionIndex) {
            collectibleIndex = rows[row].collectibleIndex;
            return true;
        }
    }
    return false;
}

/** Prepares the exact three-byte opcode-1820 Collections item request. */
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode1820::Request request{};
    if (!middleware::web_service::messages::opcode1820::parse_request(message, request)) {
        report_item_acquisition(message,
                                "fail",
                                "payload_bits",
                                kUnavailableDefinitionIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }
    const std::uint16_t collectibleIndex = request.collectibleIndex;
    std::uint16_t itemDefinitionIndex = 0;
    if (!state::build_data::find_collectible_item_definition_index(collectibleIndex,
                                                                   itemDefinitionIndex)) {
        report_item_acquisition(message,
                                "fail",
                                "collectible_definition",
                                collectibleIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }
    grant_item_definition(message, collectibleIndex, itemDefinitionIndex, outcome);
}

/**
 * Resolves one vendor row to the item it sells.
 *
 * Shared by the purchase (901) and the quest acquire (904), which name a row the same way, so the
 * two cannot drift apart.
 *
 * @param vendorIndex Vendor table row.
 * @param rowIndex Sale row within that vendor.
 * @param itemDefinitionIndex Receives the item the row sells.
 * @param reason Receives the step that failed, when one does.
 * @return True when the row resolved.
 */
[[nodiscard]] bool resolve_vendor_row(std::int32_t vendorIndex,
                                      std::int32_t rowIndex,
                                      std::uint16_t& itemDefinitionIndex,
                                      std::int32_t& categoryIndex,
                                      const char*& reason) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    if (vendorIndex < 0 || rowIndex < 0) {
        reason = "negative_index";
        return false;
    }
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (!vendor_domain::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
        || !vendor_domain::find(entry.definitionHash, definition)) {
        reason = "vendor";
        return false;
    }
    static std::array<vendor_domain::SaleRow, vendor_domain::kSaleRowCapacity> rows{};
    std::size_t count = 0;
    if (!vendor_domain::sale_rows(definition, rows, count)
        || static_cast<std::size_t>(rowIndex) >= count) {
        reason = "sale_row";
        return false;
    }
    itemDefinitionIndex = rows[static_cast<std::size_t>(rowIndex)].itemIndex;
    // Sale row +100. The catalog still calls this `installedIndex`, but it is the vendor
    // categoryIndex - established by correlating 3,304 rows against the manifest.
    categoryIndex = rows[static_cast<std::size_t>(rowIndex)].installedIndex;
    return true;
}

/** Pursuit rows written out when a vendor is asked what it actually sells. */
constexpr std::size_t kPursuitListCap = 64;

/**
 * Lists the sale rows of one vendor whose item is a pursuit.
 *
 * A pursuit is what lands in the Quests tab, and the grant path for one is already proven: a row
 * resolves to an item, the item goes to the character inventory, and the tab shows it. What is not
 * known for a given vendor is *which* of its rows are pursuits at all. Amanda declares 220 sale
 * rows, and a tile that carries no row is not one of them.
 *
 * The classification is the shared rule, so this list and the duplicate check can never disagree
 * about what counts as a quest.
 *
 * @param vendorIndex Vendor to list.
 */
void report_pursuit_rows(std::int32_t vendorIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    namespace detail_domain = state::build_data::items::details;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (vendorIndex < 0
        || !vendor_domain::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
        || !vendor_domain::find(entry.definitionHash, definition)) {
        return;
    }
    static std::array<vendor_domain::SaleRow, vendor_domain::kSaleRowCapacity> rows{};
    std::size_t count = 0;
    if (!vendor_domain::sale_rows(definition, rows, count)) {
        return;
    }
    // One item repeats across dozens of rows - Amanda declares 38 consecutive rows of a single
    // placeholder - so listing rows rather than items buries everything interesting under filler.
    static std::array<std::uint16_t, kPursuitListCap> seen{};
    std::size_t listed = 0;
    std::size_t pursuits = 0;
    for (std::size_t row = 0; row < count; ++row) {
        const std::uint16_t itemIndex = rows[row].itemIndex;
        detail_domain::Definition detail{};
        if (!state::build_data::find_configured_item_detail(itemIndex, detail)
            || detail.equipmentSlot.has_value() || detail.maxStackSize > 1) {
            continue;
        }
        ++pursuits;
        bool duplicate = false;
        for (std::size_t index = 0; index < listed; ++index) {
            duplicate = duplicate || seen[index] == itemIndex;
        }
        if (duplicate || listed >= kPursuitListCap) {
            continue;
        }
        seen[listed] = itemIndex;
        ++listed;
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=vendor stage=pursuit vendor=%d sale=%zu item=%u "
                                          "hash=0x%08X bucket=%u",
                                          static_cast<int>(vendorIndex),
                                          row,
                                          static_cast<unsigned>(itemIndex),
                                          detail.definitionHash,
                                          static_cast<unsigned>(detail.bucketId));
        if (written > 0) {
            // One line per distinct row, so this is the detail behind the summary rather than
            // something worth putting in front of everything else that reports at info.
            core::log::write(core::log::Channel::server,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    std::array<char, core::log::kLineCapacity> summary{};
    const int written = std::snprintf(summary.data(),
                                      summary.size(),
                                      "ev=vendor stage=pursuits vendor=%d sale_rows=%zu "
                                      "pursuits=%zu distinct_listed=%zu",
                                      static_cast<int>(vendorIndex),
                                      count,
                                      pursuits,
                                      listed);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {summary.data(), static_cast<std::size_t>(written)});
    }
}

/** An installed row names its item by definition hash at this offset. */
constexpr std::size_t kInstalledRowHashOffset = 0;
/** FNV-1's basis, which this engine also uses as its absent-hash sentinel. */
constexpr std::uint32_t kAbsentNameHash = 0x811C9DC5U;

/**
 * Resolves the item behind a 904 that names no sale row.
 *
 * Amanda Holliday's Legacy Content tiles send `slot=1, row=-1`, where every working request names a
 * real sale row. So those tiles are not sale rows, and the slot is the only thing identifying them.
 * The old codec turned that -1 into 65535 and refused it as an out-of-range row, which was the same
 * refusal by luck rather than by reading the request.
 *
 * Measured on the Red War tile: vendor 27 declares 220 sale rows but **22 installed rows and 22
 * third rows**, and the slot is 1. The two parallel counts are what say the slot indexes them
 * rather than the sale array. That installed row reads
 * `7FC947A6 00000000 00000000 C59D1C81 FFFFFFFF FFFFFFFF` - a hash at `+0`, then the FNV-1 absent
 * sentinel at `+12`, then two -1s. So `+0` names the item by **definition hash**, where a sale row
 * names its item by index at `+70`.
 *
 * The resolution is logged whether or not it succeeds, because the grant that follows is only as
 * good as the hash, and a wrong item that commits cleanly is harder to spot than a refusal.
 *
 * @param vendorIndex Vendor the request named.
 * @param slotIndex The 16-bit field, which is all the request carries.
 * @param itemDefinitionIndex Receives the item, or the unavailable sentinel.
 * @return True when the row's hash resolved to an installed item definition.
 */
[[nodiscard]] bool resolve_rowless_quest(std::int32_t vendorIndex,
                                         std::int32_t slotIndex,
                                         std::uint16_t& itemDefinitionIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    itemDefinitionIndex = kUnavailableDefinitionIndex;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (vendorIndex < 0 || slotIndex < 0
        || !vendor_domain::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
        || !vendor_domain::find(entry.definitionHash, definition)) {
        return false;
    }
    static std::array<vendor_domain::InstalledRow, vendor_domain::kInstalledRowCapacity> rows{};
    std::size_t count = 0;
    if (!vendor_domain::installed_rows(definition, rows, count)
        || static_cast<std::size_t>(slotIndex) >= count) {
        return false;
    }
    const auto& raw = rows[static_cast<std::size_t>(slotIndex)].raw;
    std::uint32_t definitionHash = 0;
    std::memcpy(&definitionHash, raw.data() + kInstalledRowHashOffset, sizeof definitionHash);

    state::build_data::items::Definition item{};
    const bool resolved = definitionHash != kAbsentNameHash
                          && state::build_data::find_item_definition_hash(definitionHash, item);
    if (resolved) {
        itemDefinitionIndex = item.definitionIndex;
    }
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=ws904 stage=rowless vendor=%d slot=%d installed=%u sale=%u "
                                "third=%u hash=0x%08X item=%u resolved=%u hex=",
                                static_cast<int>(vendorIndex),
                                static_cast<int>(slotIndex),
                                static_cast<unsigned>(definition.installedCount),
                                static_cast<unsigned>(definition.saleCount),
                                static_cast<unsigned>(definition.thirdCount),
                                definitionHash,
                                static_cast<unsigned>(itemDefinitionIndex),
                                resolved ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        std::size_t length = static_cast<std::size_t>(written);
        const auto* const bytes = reinterpret_cast<const std::byte*>(raw.data());
        (void)core::log::append_hex(line, length, {bytes, raw.size()});
        if (length != 0) {
            core::log::write(core::log::Channel::server,
                             resolved ? core::log::Level::info : core::log::Level::warn,
                             {line.data(), length});
        }
    }
    return resolved;
}

/** What one resolved vendor row turned out to be, once it was settled. */
enum class RowOutcome : std::uint8_t {
    /** The row was a reputation turn-in. */
    reputation,
    /** The row granted the item it names. */
    granted,
};

/**
 * Settles one resolved vendor row, in the order a row's behaviours are tried.
 *
 * Both vendor opcodes end here. A row is one of two things, and which it is cannot be read off the
 * row itself - each is recognised by an authored rule keyed to the vendor, so they are tried in
 * turn and the first that claims the row owns it. Keeping that order in one place is what stops the
 * two opcodes drifting: a behaviour taught to one and not the other is the shape of bug this path
 * has already produced more than once.
 *
 * @param message Request being answered.
 * @param opcode Opcode to report under.
 * @param vendorIndex Vendor the request names.
 * @param rowIndex Sale row the request names.
 * @param categoryIndex Category of that row, from sale row +100.
 * @param itemDefinitionIndex Item the row names.
 * @param outcome Receives whatever mutation the row prepared.
 * @return What the row turned out to be.
 */
RowOutcome settle_vendor_row(const middleware::web_service::Message& message,
                             std::uint16_t opcode,
                             std::int32_t vendorIndex,
                             std::int32_t rowIndex,
                             std::int32_t categoryIndex,
                             std::uint16_t itemDefinitionIndex,
                             Outcome& outcome) noexcept {
    if (credit_vendor_reputation(vendorIndex, categoryIndex)) {
        report_purchase(opcode, "ok", "reputation", vendorIndex, rowIndex, itemDefinitionIndex);
        // Granting nothing means preparing no transaction, so nothing would re-encode the
        // character and the credit would sit in server state unseen. This asks for the peer's own
        // account graph to be resent.
        outcome.requiresSelfResync = true;
        return RowOutcome::reputation;
    }
    const std::uint16_t granted = itemDefinitionIndex;
    std::uint16_t collectibleIndex = state::build_data::collectibles::kNoCollectibleIndex;
    const bool collected = find_collectible_for_item(granted, collectibleIndex);
    report_purchase(opcode,
                    "ok",
                    collected ? "resolved" : "resolved_no_collectible",
                    vendorIndex,
                    rowIndex,
                    granted);
    grant_item_definition(message, collectibleIndex, granted, outcome);
    return RowOutcome::granted;
}

/**
 * Prepares one opcode-904 quest acquire.
 *
 * A quest names a vendor row exactly as a purchase does, and the item behind it is granted through
 * the same path, so a quest lands in the inventory the way a bounty now does.
 */
void acquire_quest(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace quest = middleware::web_service::messages::opcode904;
    quest::Request request{};
    if (!quest::parse_request(message, request)) {
        report_purchase(quest::kOpcode, "fail", "payload", -1, -1, kUnavailableDefinitionIndex);
        return;
    }
    // The 16-bit slot field is where the click landed, and indexing sale rows with it granted
    // armour mods. The 32-bit field is the real row, and the slot is kept only for the case where
    // the body carried no such field.
    const std::int32_t row = request.hasSaleIndex ? request.saleIndex : request.slotIndex;
    std::uint16_t itemDefinitionIndex = 0;
    const char* reason = "unknown";
    // A row of -1 is the client saying this tile is not a sale row at all, rather than a row that
    // failed to resolve, so it takes the installed array instead. Falling back to the slot as a
    // sale row would grant whatever sits there, which is the wrong-item bug that made quests hand
    // out armour mods.
    const bool rowless = row < 0;
    // A rowless 904 is an interaction reply rather than a purchase, and the rank-up reward tile is
    // one: its reply names no sale row, so the slot field is the interaction it answered.
    std::int32_t questCategoryIndex = -1;
    const bool located =
        rowless ? resolve_rowless_quest(request.vendorIndex, request.slotIndex, itemDefinitionIndex)
                : resolve_vendor_row(
                      request.vendorIndex, row, itemDefinitionIndex, questCategoryIndex, reason);
    if (!located) {
        report_purchase(quest::kOpcode,
                        "fail",
                        rowless ? "rowless_unresolved" : reason,
                        request.vendorIndex,
                        row,
                        kUnavailableDefinitionIndex);
        // A tile that names no row grants nothing, so say what this vendor does offer that would
        // land in the Quests tab. That is the difference between "this click is broken" and "this
        // click was never a quest".
        if (rowless) {
            report_pursuit_rows(request.vendorIndex);
        }
        return;
    }
    // The reputation turn-in arrives on opcode 904, not 901: it is the quest-acquire path that
    // carries it, along with every other behaviour a row can hold.
    const RowOutcome settled = settle_vendor_row(message,
                                                 quest::kOpcode,
                                                 request.vendorIndex,
                                                 row,
                                                 questCategoryIndex,
                                                 itemDefinitionIndex,
                                                 outcome);
    // Only a row that handed over the quest it offered answers the banner. A reputation credit leaves
    // the banner's own question unanswered, so it stays up.
    if (settled != RowOutcome::granted) {
        return;
    }
    // The banner that offered this quest has now been answered, and nothing else tells the client
    // so: its picker keeps choosing the same interaction for as long as the quest is offerable.
    // Retiring it here, exactly where the shipped game appends its own entry, lets the next
    // interaction take the screen.
    if (request.vendorIndex >= 0
        && request.vendorIndex
               < static_cast<std::int32_t>(client::hooks::vendor_banner::kVendorCapacity)) {
        (void)client::hooks::vendor_banner::suppress_current(
            static_cast<std::uint16_t>(request.vendorIndex));
    }
}

/**
 * Prepares one opcode-901 vendor purchase, for any Tower vendor.
 *
 * The request names a vendor row and a sale row. The sale row names an item-definition index, which
 * is the same thing a Collections pull resolves its collectible to, so this resolves the row and
 * hands over to the very same grant.
 *
 * Cost is deliberately not charged: the sale row's cost-bearing fields are still role-open, and the
 * domain header warns against naming one a cost without its mutation reader.
 */
void purchase_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace purchase = middleware::web_service::messages::opcode901;
    purchase::Request request{};
    if (!purchase::parse_request(message, request)) {
        report_purchase(purchase::kOpcode, "fail", "payload", -1, -1, kUnavailableDefinitionIndex);
        return;
    }
    std::uint16_t itemDefinitionIndex = 0;
    const char* reason = "unknown";
    std::int32_t categoryIndex = -1;
    if (!resolve_vendor_row(
            request.vendorIndex, request.saleIndex, itemDefinitionIndex, categoryIndex, reason)) {
        report_purchase(purchase::kOpcode,
                        "fail",
                        reason,
                        request.vendorIndex,
                        request.saleIndex,
                        kUnavailableDefinitionIndex);
        return;
    }
    // Bounties, quest steps and tokens carry no collectible. The acquisition takes the sentinel
    // rather than a made-up row, and both prepare and commit skip the collectible steps for it.
    // A reputation turn-in sells a Dummy placeholder; the purchase is meant to credit the vendor
    // progression, not put that placeholder in the inventory. Crossing a step total is what hands
    // out the rank-up reward, because offline the client renders the bar and grants nothing.
    //
    (void)settle_vendor_row(message,
                            purchase::kOpcode,
                            request.vendorIndex,
                            request.saleIndex,
                            categoryIndex,
                            itemDefinitionIndex,
                            outcome);
}

} // namespace sunrise::server::web_service
