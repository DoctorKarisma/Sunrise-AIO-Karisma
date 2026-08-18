#include "weapon_editor_panel.h"

#include "server/bap/runtime.h"
#include "state/account/account_state.h"
#include "state/build_data/items/socket_plugs/socket_plug_catalog.h"
#include "state/build_data/runtime.h"
#include "state/runtime/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <imgui.h>

#include "../../../../resources/resource.h"
#include "client/hooks/graphics/textures/graphics_texture_upload.h"

#ifdef interface
#undef interface
#endif

namespace sunrise::server::ui::weapon_editor {
namespace {

namespace inventory = state::account::inventory;
namespace item_details = state::build_data::items::details;
namespace socket_plugs = state::build_data::items::socket_plugs;
namespace gfx_textures = client::hooks::graphics::textures;

constexpr std::array<inventory::EquipmentSlot, 3> kWeaponSlots{
    inventory::EquipmentSlot::kinetic,
    inventory::EquipmentSlot::energy,
    inventory::EquipmentSlot::heavy,
};
constexpr std::array<const char*, 3> kWeaponSlotNames{"KINETIC", "ENERGY", "POWER"};
constexpr std::size_t kMaximumVisibleWeaponsPerSlot = 10;
constexpr std::size_t kWeaponColumns = 5;
constexpr float kInventoryGap = 7.0F;
constexpr float kSocketGap = 7.0F;
constexpr float kPlugCardHeight = 68.0F;
constexpr float kPlugIconSize = 48.0F;
constexpr float kWeaponCatalogCardHeight = 70.0F;

struct ResourceBytes {
    const std::byte* data{};
    std::size_t size{};
};

struct WeaponRow {
    inventory::Item item{};
    state::build_data::items::Definition definition{};
    item_details::Definition detail{};
    std::string_view name{};
    bool equipped{};
};

struct PlugChoice {
    state::build_data::items::Definition definition{};
    std::string_view name{};
};

struct WeaponChoice {
    state::build_data::items::Definition definition{};
    item_details::Definition detail{};
    std::string_view name{};
};

enum class PlugFilter : std::uint8_t {
    compatible,
    all,
};

enum class BrowserMode : std::uint8_t {
    plugs,
    weapons,
};

enum class SocketKind : std::uint8_t {
    hidden,
    perk,
    ornament,
    mod,
    catalyst,
    shader,
    tracker,
    masterwork,
};

std::array<std::vector<WeaponRow>, 3> g_slotWeapons{};
std::array<std::size_t, 3> g_selectedWeaponBySlot{};
std::size_t g_activeSlot{};

std::vector<PlugChoice> g_plugs{};
std::vector<std::size_t> g_visiblePlugs{};
std::size_t g_selectedPlug{};
std::uint8_t g_selectedLane{};
PlugFilter g_filter{PlugFilter::compatible};
std::array<char, 64> g_search{};
std::array<char, 16> g_hashInput{};

std::vector<WeaponChoice> g_weaponCatalog{};
std::vector<std::size_t> g_visibleWeaponCatalog{};
std::size_t g_selectedReplacement{};
std::array<char, 64> g_weaponSearch{};
BrowserMode g_browserMode{BrowserMode::plugs};
float g_uiScale{1.0F};
std::array<char, 192> g_message{};

std::uint16_t g_cachedItemDefinition{0xFFFFU};
std::uint8_t g_cachedLane{0xFFU};
PlugFilter g_cachedFilter{PlugFilter::compatible};
std::size_t g_cachedDefinitionCount{};
std::uint16_t g_cachedWeaponCatalogSource{0xFFFFU};
std::size_t g_cachedWeaponCatalogCount{};

ResourceBytes g_nameResource{};
bool g_nameResourceResolved{};

constexpr std::array<std::byte, 8> kNameMagic{
    static_cast<std::byte>('S'), static_cast<std::byte>('W'),
    static_cast<std::byte>('N'), static_cast<std::byte>('A'),
    static_cast<std::byte>('M'), static_cast<std::byte>('E'),
    static_cast<std::byte>('0'), static_cast<std::byte>('1'),
};
constexpr std::size_t kNameHeaderSize = 12;
constexpr std::size_t kNameEntrySize = 12;

[[nodiscard]] float scaled(float value) noexcept {
    return value * g_uiScale;
}

[[nodiscard]] ImU32 ui_color(ImU32 color) noexcept {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}

[[nodiscard]] HMODULE owning_module() noexcept {
    HMODULE module = nullptr;
    (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&owning_module),
                             &module);
    return module;
}

[[nodiscard]] bool resource_bytes(int identifier, ResourceBytes& output) noexcept {
    const HMODULE module = owning_module();
    const HRSRC resource = module != nullptr
                               ? FindResourceW(module, MAKEINTRESOURCEW(identifier), RT_RCDATA)
                               : nullptr;
    if (resource == nullptr) {
        return false;
    }
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (size == 0 || bytes == nullptr) {
        return false;
    }
    output.data = static_cast<const std::byte*>(bytes);
    output.size = static_cast<std::size_t>(size);
    return true;
}

[[nodiscard]] std::uint32_t read_u32_le(const std::byte* bytes) noexcept {
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(raw[0])
           | (static_cast<std::uint32_t>(raw[1]) << 8U)
           | (static_cast<std::uint32_t>(raw[2]) << 16U)
           | (static_cast<std::uint32_t>(raw[3]) << 24U);
}

[[nodiscard]] std::uint16_t read_u16_le(const std::byte* bytes) noexcept {
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint16_t>(raw[0])
           | static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw[1]) << 8U);
}

[[nodiscard]] const ResourceBytes& name_resource() noexcept {
    if (!g_nameResourceResolved) {
        g_nameResourceResolved = true;
        ResourceBytes candidate{};
        if (resource_bytes(IDR_ITEM_NAMES, candidate) && candidate.size >= kNameHeaderSize
            && std::memcmp(candidate.data, kNameMagic.data(), kNameMagic.size()) == 0) {
            g_nameResource = candidate;
        }
    }
    return g_nameResource;
}

[[nodiscard]] std::string_view display_name(std::uint32_t hash) noexcept {
    const ResourceBytes& resource = name_resource();
    if (resource.data == nullptr || resource.size < kNameHeaderSize) {
        return {};
    }
    const std::size_t count = static_cast<std::size_t>(read_u32_le(resource.data + 8));
    if (count > (resource.size - kNameHeaderSize) / kNameEntrySize) {
        return {};
    }
    const std::byte* index = resource.data + kNameHeaderSize;
    std::size_t low = 0;
    std::size_t high = count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const std::byte* entry = index + middle * kNameEntrySize;
        const std::uint32_t candidate = read_u32_le(entry);
        if (candidate < hash) {
            low = middle + 1;
            continue;
        }
        if (candidate > hash) {
            high = middle;
            continue;
        }
        const std::size_t offset = static_cast<std::size_t>(read_u32_le(entry + 4));
        const std::size_t length = static_cast<std::size_t>(read_u16_le(entry + 8));
        if (offset > resource.size || length > resource.size - offset) {
            return {};
        }
        return {reinterpret_cast<const char*>(resource.data + offset), length};
    }
    return {};
}

[[nodiscard]] std::size_t selected_character_index(const state::AccountState& account) noexcept {
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            return index;
        }
    }
    return account.characterCount;
}

[[nodiscard]] bool resolve_item(const inventory::Item& item,
                                state::build_data::items::Definition& definition,
                                item_details::Definition& detail) noexcept {
    return item.instanceSoid != 0
           && state::build_data::find_item_definition_hash(item.definitionHash, definition)
           && definition.definitionHash == item.definitionHash
           && state::build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex
           && detail.definitionHash == definition.definitionHash
           && detail.ordinarySocketState == item_details::OrdinarySocketState::present
           && detail.ordinarySocketCount != 0
           && detail.ordinarySocketCount <= inventory::kPlugCapacity;
}

[[nodiscard]] bool same_instance(const inventory::Item& left,
                                 const inventory::Item& right) noexcept {
    return left.instanceSoid != 0 && left.instanceSoid == right.instanceSoid;
}

void append_weapon(std::vector<WeaponRow>& rows, const inventory::Item& item, bool equipped) {
    if (rows.size() >= kMaximumVisibleWeaponsPerSlot) {
        return;
    }
    WeaponRow row{};
    if (!resolve_item(item, row.definition, row.detail)) {
        return;
    }
    row.item = item;
    row.name = display_name(item.definitionHash);
    row.equipped = equipped;
    rows.push_back(row);
}

void rebuild_weapons(const state::CharacterState& character) {
    std::array<std::uint8_t, 3> bucketIds{};
    std::array<bool, 3> bucketKnown{};
    std::array<inventory::Item, 3> equippedItems{};
    std::array<bool, 3> equippedKnown{};

    for (auto& slot : g_slotWeapons) {
        slot.clear();
    }

    for (std::size_t slot = 0; slot < kWeaponSlots.size(); ++slot) {
        const std::size_t semantic = static_cast<std::size_t>(kWeaponSlots[slot]);
        if (semantic >= character.equipment.slots.size()
            || !character.equipment.slots[semantic].has_value()) {
            continue;
        }
        const inventory::Item& item = *character.equipment.slots[semantic];
        state::build_data::items::Definition definition{};
        item_details::Definition detail{};
        if (!resolve_item(item, definition, detail)) {
            continue;
        }
        bucketIds[slot] = definition.bucketId;
        bucketKnown[slot] = true;
        equippedItems[slot] = item;
        equippedKnown[slot] = true;
        append_weapon(g_slotWeapons[slot], item, true);
    }

    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        const inventory::Item& item = character.inventory.values[index];
        state::build_data::items::Definition definition{};
        item_details::Definition detail{};
        if (!resolve_item(item, definition, detail)) {
            continue;
        }
        for (std::size_t slot = 0; slot < g_slotWeapons.size(); ++slot) {
            if (!bucketKnown[slot] || definition.bucketId != bucketIds[slot]
                || (equippedKnown[slot] && same_instance(item, equippedItems[slot]))) {
                continue;
            }
            append_weapon(g_slotWeapons[slot], item, false);
            break;
        }
    }

    for (std::size_t slot = 0; slot < g_slotWeapons.size(); ++slot) {
        if (g_selectedWeaponBySlot[slot] >= g_slotWeapons[slot].size()) {
            g_selectedWeaponBySlot[slot] = 0;
        }
    }
    if (g_activeSlot >= g_slotWeapons.size()) {
        g_activeSlot = 0;
    }
}

[[nodiscard]] WeaponRow* selected_weapon() noexcept {
    if (g_activeSlot >= g_slotWeapons.size()) {
        return nullptr;
    }
    auto& rows = g_slotWeapons[g_activeSlot];
    if (rows.empty()) {
        return nullptr;
    }
    const std::size_t selected = (std::min)(g_selectedWeaponBySlot[g_activeSlot], rows.size() - 1);
    return &rows[selected];
}

[[nodiscard]] std::optional<std::uint32_t>
current_plug_hash(const WeaponRow& weapon, std::uint8_t lane) noexcept {
    if (lane >= weapon.detail.ordinarySocketCount) {
        return std::nullopt;
    }
    if (weapon.item.sockets.policy == inventory::SocketPolicy::authored) {
        if (lane >= weapon.item.sockets.plugCount || !weapon.item.sockets.plugs[lane].has_value()) {
            return std::nullopt;
        }
        return *weapon.item.sockets.plugs[lane];
    }
    const std::uint16_t plugIndex = weapon.detail.initialPlugIndices[lane];
    if (plugIndex == item_details::kUnavailableItemIndex) {
        return std::nullopt;
    }
    state::build_data::items::Definition plug{};
    if (!state::build_data::find_item_definition_index(plugIndex, plug)) {
        return std::nullopt;
    }
    return plug.definitionHash;
}

[[nodiscard]] bool text_ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size()
           && text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

[[nodiscard]] SocketKind socket_kind(const WeaponRow& weapon, std::uint8_t lane) noexcept {
    if (lane >= weapon.detail.ordinarySocketCount
        || weapon.detail.socketTypes[lane] == item_details::kUnavailableSocketType) {
        return SocketKind::hidden;
    }

    std::string_view name{};
    const std::uint16_t initialIndex = weapon.detail.initialPlugIndices[lane];
    state::build_data::items::Definition initialPlug{};
    if (initialIndex != item_details::kUnavailableItemIndex
        && state::build_data::find_item_definition_index(initialIndex, initialPlug)) {
        name = display_name(initialPlug.definitionHash);
    }
    if (name == "Default Shader") {
        return SocketKind::shader;
    }
    if (name == "Default Ornament" || text_ends_with(name, " Ornament")) {
        return SocketKind::ornament;
    }
    if (name == "Tracker Disabled" || name.find("Kill Tracker") != std::string_view::npos
        || name == "Crucible Tracker") {
        return SocketKind::tracker;
    }
    if (name == "Empty Catalyst Socket" || text_ends_with(name, " Catalyst")) {
        return SocketKind::catalyst;
    }
    if (name.find("Masterwork") != std::string_view::npos) {
        return SocketKind::masterwork;
    }
    if (name.find("Mod Socket") != std::string_view::npos) {
        return SocketKind::mod;
    }

    const std::uint16_t type = weapon.detail.socketTypes[lane];
    if (type == 180 || type == 182) {
        return SocketKind::shader;
    }
    if (type == 518 || type == 49 || type == 50) {
        return SocketKind::tracker;
    }
    if (type == 68 || type == 69 || (type >= 687 && type <= 703 && type != 693)) {
        return SocketKind::mod;
    }
    if (type == 483 || (type >= 185 && type <= 191) || type == 193
        || (type >= 197 && type <= 206) || type == 216 || (type >= 218 && type <= 220)) {
        return SocketKind::masterwork;
    }
    if ((type >= 421 && type <= 450) || (type >= 562 && type <= 582)
        || (type >= 639 && type <= 642) || (type >= 680 && type <= 685)
        || (type >= 714 && type <= 716) || (type >= 725 && type <= 727)
        || (type >= 736 && type <= 738)) {
        return SocketKind::catalyst;
    }
    return SocketKind::perk;
}

[[nodiscard]] const char* socket_kind_label(SocketKind kind) noexcept {
    switch (kind) {
        case SocketKind::ornament: return "ORNAMENT";
        case SocketKind::mod: return "MOD";
        case SocketKind::catalyst: return "CATALYST";
        case SocketKind::shader: return "SHADER";
        case SocketKind::tracker: return "TRACKER";
        case SocketKind::masterwork: return "MASTERWORK";
        default: return "PERK";
    }
}

[[nodiscard]] bool editable_lane(const WeaponRow& weapon, std::uint8_t lane) noexcept {
    return socket_kind(weapon, lane) != SocketKind::hidden;
}

[[nodiscard]] std::uint8_t first_editable_lane(const WeaponRow& weapon) noexcept {
    for (std::uint8_t lane = 0; lane < weapon.detail.ordinarySocketCount; ++lane) {
        if (editable_lane(weapon, lane)) {
            return lane;
        }
    }
    return 0;
}

void invalidate_plug_cache() noexcept {
    g_cachedItemDefinition = 0xFFFFU;
    g_cachedLane = 0xFFU;
    g_cachedDefinitionCount = 0;
    g_plugs.clear();
    g_visiblePlugs.clear();
    g_selectedPlug = 0;
}

void invalidate_weapon_catalog() noexcept {
    g_cachedWeaponCatalogSource = 0xFFFFU;
    g_cachedWeaponCatalogCount = 0;
    g_weaponCatalog.clear();
    g_visibleWeaponCatalog.clear();
    g_selectedReplacement = 0;
}

void reset_selection_for_weapon(const WeaponRow& weapon) noexcept {
    g_selectedLane = first_editable_lane(weapon);
    g_message[0] = '\0';
    g_browserMode = BrowserMode::plugs;
    invalidate_plug_cache();
    invalidate_weapon_catalog();
}

void rebuild_plugs(const WeaponRow& weapon) {
    const std::size_t definitionCount = state::build_data::item_definition_count();
    if (g_cachedItemDefinition == weapon.definition.definitionIndex
        && g_cachedLane == g_selectedLane && g_cachedFilter == g_filter
        && g_cachedDefinitionCount == definitionCount) {
        return;
    }

    g_plugs.clear();
    g_selectedPlug = 0;

    const std::optional<std::uint32_t> current = current_plug_hash(weapon, g_selectedLane);
    for (std::size_t index = 0; index < definitionCount && index <= 0xFFFFU; ++index) {
        const auto definitionIndex = static_cast<std::uint16_t>(index);
        if (!socket_plugs::contains(definitionIndex)) {
            continue;
        }
        if (g_filter == PlugFilter::compatible
            && !state::build_data::is_socket_plug_allowed(
                weapon.definition.definitionIndex, g_selectedLane, definitionIndex)) {
            continue;
        }
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_index(definitionIndex, definition)
            || definition.definitionHash == inventory::kNoDefinitionHash) {
            continue;
        }
        PlugChoice choice{};
        choice.definition = definition;
        choice.name = display_name(definition.definitionHash);
        if (current.has_value() && definition.definitionHash == *current) {
            g_selectedPlug = g_plugs.size();
        }
        g_plugs.push_back(choice);
    }

    g_cachedItemDefinition = weapon.definition.definitionIndex;
    g_cachedLane = g_selectedLane;
    g_cachedFilter = g_filter;
    g_cachedDefinitionCount = definitionCount;
}

[[nodiscard]] bool contains_ascii_case_insensitive(std::string_view text,
                                                    std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (text.size() < needle.size()) {
        return false;
    }
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            const auto left = static_cast<unsigned char>(text[start + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool parse_hex(std::string_view text, std::uint32_t& hash) noexcept {
    hash = 0;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), hash, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool plug_matches_search(const PlugChoice& plug) noexcept {
    const std::string_view needle{g_search.data(), std::strlen(g_search.data())};
    if (needle.empty()) {
        return true;
    }
    if (!plug.name.empty() && contains_ascii_case_insensitive(plug.name, needle)) {
        return true;
    }
    std::array<char, 48> technical{};
    (void)std::snprintf(technical.data(),
                        technical.size(),
                        "%08X %u",
                        static_cast<unsigned>(plug.definition.definitionHash),
                        static_cast<unsigned>(plug.definition.definitionIndex));
    return contains_ascii_case_insensitive(technical.data(), needle);
}

void rebuild_visible_plugs() {
    g_visiblePlugs.clear();
    g_visiblePlugs.reserve(g_plugs.size());
    for (std::size_t index = 0; index < g_plugs.size(); ++index) {
        if (plug_matches_search(g_plugs[index])) {
            g_visiblePlugs.push_back(index);
        }
    }
}

void select_hash(const WeaponRow& weapon) noexcept {
    std::uint32_t hash = 0;
    state::build_data::items::Definition definition{};
    const std::string_view input{g_hashInput.data(), std::strlen(g_hashInput.data())};
    if (!parse_hex(input, hash)
        || !state::build_data::find_item_definition_hash(hash, definition)
        || !socket_plugs::contains(definition.definitionIndex)) {
        (void)std::snprintf(g_message.data(), g_message.size(), "Plug hash was not found.");
        return;
    }
    if (g_filter == PlugFilter::compatible
        && !state::build_data::is_socket_plug_allowed(
            weapon.definition.definitionIndex, g_selectedLane, definition.definitionIndex)) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "That plug is not compatible. Switch to All plugs.");
        return;
    }
    rebuild_plugs(weapon);
    for (std::size_t index = 0; index < g_plugs.size(); ++index) {
        if (g_plugs[index].definition.definitionIndex == definition.definitionIndex) {
            g_selectedPlug = index;
            (void)std::snprintf(g_message.data(),
                                g_message.size(),
                                "Selected 0x%08X.",
                                static_cast<unsigned>(hash));
            return;
        }
    }
    (void)std::snprintf(g_message.data(), g_message.size(), "Plug is outside this filter.");
}

void apply_selected(const WeaponRow& weapon) noexcept {
    if (g_selectedPlug >= g_plugs.size() || g_selectedLane >= weapon.detail.ordinarySocketCount) {
        return;
    }
    const auto& plug = g_plugs[g_selectedPlug].definition;
    state::PendingSocketPlug mutation{};
    if (!state::prepare_socket_plug_unrestricted(
            weapon.item.instanceSoid, g_selectedLane, plug.definitionIndex, mutation)) {
        (void)std::snprintf(g_message.data(), g_message.size(), "Apply failed while preparing.");
        return;
    }
    if (!state::commit_socket_plug(mutation)) {
        (void)std::snprintf(g_message.data(), g_message.size(), "Apply failed: state changed.");
        return;
    }
    const bool refreshQueued = server::bap::request_account_resync();
    (void)std::snprintf(g_message.data(),
                        g_message.size(),
                        refreshQueued ? "Applied. Live refresh queued."
                                      : "Applied. No active Family-4 session yet.");
    invalidate_plug_cache();
}

void rebuild_weapon_catalog(const WeaponRow& weapon) {
    const std::size_t definitionCount = state::build_data::item_definition_count();
    if (g_cachedWeaponCatalogSource == weapon.definition.definitionIndex
        && g_cachedWeaponCatalogCount == definitionCount) {
        return;
    }

    g_weaponCatalog.clear();
    g_selectedReplacement = 0;
    for (std::size_t index = 0; index < definitionCount && index <= 0xFFFFU; ++index) {
        state::build_data::items::Definition definition{};
        item_details::Definition detail{};
        if (!state::build_data::find_item_definition_index(static_cast<std::uint16_t>(index), definition)
            || definition.definitionHash == inventory::kNoDefinitionHash
            || definition.bucketId != weapon.definition.bucketId
            || !state::build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.bucketId != definition.bucketId
            || detail.equipmentSlot != weapon.detail.equipmentSlot
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::instanced
            || detail.ordinarySocketState != item_details::OrdinarySocketState::present
            || detail.ordinarySocketCount == 0
            || detail.ordinarySocketCount > inventory::kPlugCapacity) {
            continue;
        }
        const std::string_view name = display_name(definition.definitionHash);
        if (name.empty()) {
            continue;
        }
        if (definition.definitionHash == weapon.item.definitionHash) {
            g_selectedReplacement = g_weaponCatalog.size();
        }
        g_weaponCatalog.push_back(WeaponChoice{definition, detail, name});
    }

    g_cachedWeaponCatalogSource = weapon.definition.definitionIndex;
    g_cachedWeaponCatalogCount = definitionCount;
}

[[nodiscard]] bool weapon_matches_search(const WeaponChoice& choice) noexcept {
    const std::string_view needle{g_weaponSearch.data(), std::strlen(g_weaponSearch.data())};
    if (needle.empty()) {
        return true;
    }
    if (contains_ascii_case_insensitive(choice.name, needle)) {
        return true;
    }
    std::array<char, 40> technical{};
    (void)std::snprintf(technical.data(),
                        technical.size(),
                        "%08X %u",
                        static_cast<unsigned>(choice.definition.definitionHash),
                        static_cast<unsigned>(choice.definition.definitionIndex));
    return contains_ascii_case_insensitive(technical.data(), needle);
}

void rebuild_visible_weapon_catalog() {
    g_visibleWeaponCatalog.clear();
    g_visibleWeaponCatalog.reserve(g_weaponCatalog.size());
    for (std::size_t index = 0; index < g_weaponCatalog.size(); ++index) {
        if (weapon_matches_search(g_weaponCatalog[index])) {
            g_visibleWeaponCatalog.push_back(index);
        }
    }
}

void replace_selected_weapon(const WeaponRow& weapon) noexcept {
    if (g_selectedReplacement >= g_weaponCatalog.size()) {
        return;
    }
    const WeaponChoice& replacement = g_weaponCatalog[g_selectedReplacement];
    if (replacement.definition.definitionHash == weapon.item.definitionHash) {
        (void)std::snprintf(g_message.data(), g_message.size(), "That weapon is already selected.");
        return;
    }
    if (!state::replace_item_definition_unrestricted(
            weapon.item.instanceSoid, replacement.definition.definitionHash)) {
        (void)std::snprintf(g_message.data(), g_message.size(), "Weapon replacement failed validation.");
        return;
    }
    const bool refreshQueued = server::bap::request_account_resync();
    (void)std::snprintf(g_message.data(),
                        g_message.size(),
                        refreshQueued ? "Weapon replaced with native defaults. Live refresh queued."
                                      : "Weapon replaced with native defaults.");
    g_browserMode = BrowserMode::plugs;
    g_selectedLane = 0;
    invalidate_plug_cache();
    invalidate_weapon_catalog();
}

[[nodiscard]] ImU32 accent_for_hash(std::uint32_t hash, float alpha = 1.0F) noexcept {
    const float hue = static_cast<float>((hash ^ (hash >> 13U)) % 360U) / 360.0F;
    return ImGui::ColorConvertFloat4ToU32(ImColor::HSV(hue, 0.42F, 0.78F, alpha));
}

void draw_fallback_art(std::uint32_t hash,
                       ImVec2 start,
                       ImVec2 end,
                       float rounding) noexcept {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, end, ui_color(IM_COL32(34, 39, 49, 255)), rounding);
    const float stripHeight = (end.y - start.y) * 0.28F;
    draw->AddRectFilled(ImVec2(start.x, end.y - stripHeight),
                        end,
                        ui_color(accent_for_hash(hash)),
                        rounding,
                        ImDrawFlags_RoundCornersBottom);
    draw->AddLine(ImVec2(start.x + (end.x - start.x) * 0.20F,
                         start.y + (end.y - start.y) * 0.32F),
                  ImVec2(start.x + (end.x - start.x) * 0.80F,
                         start.y + (end.y - start.y) * 0.48F),
                  ui_color(IM_COL32(220, 225, 235, 180)),
                  scaled(4.0F));
}

void draw_icon(std::uint32_t hash,
               ImVec2 start,
               ImVec2 end,
               float rounding = 4.0F) noexcept {
    const ImTextureID texture = gfx_textures::item_icon(hash);
    if (texture == ImTextureID_Invalid) {
        draw_fallback_art(hash, start, end, rounding);
        return;
    }
    ImGui::GetWindowDrawList()->AddImageRounded(ImTextureRef(texture),
                                                start,
                                                end,
                                                ImVec2(0.0F, 0.0F),
                                                ImVec2(1.0F, 1.0F),
                                                ui_color(IM_COL32_WHITE),
                                                rounding);
}

void tooltip_name_hash(std::string_view name,
                       std::uint32_t hash,
                       std::uint16_t definitionIndex) noexcept {
    if (!ImGui::IsItemHovered()) {
        return;
    }
    ImGui::BeginTooltip();
    if (!name.empty()) {
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
    } else {
        ImGui::TextUnformatted("Unknown item");
    }
    ImGui::TextDisabled("Hash 0x%08X", static_cast<unsigned>(hash));
    ImGui::TextDisabled("Definition %u", static_cast<unsigned>(definitionIndex));
    ImGui::EndTooltip();
}

[[nodiscard]] bool draw_weapon_card(const WeaponRow& weapon,
                                    std::size_t index,
                                    bool selected,
                                    float size) noexcept {
    ImGui::PushID(static_cast<int>(index));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("weapon_card", ImVec2(size, size));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 end{start.x + size, start.y + size};
    draw_icon(weapon.item.definitionHash, start, end, scaled(5.0F));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(ImVec2(start.x + 1.0F, start.y + 1.0F),
                  ImVec2(end.x - 1.0F, end.y - 1.0F),
                  ui_color(selected ? IM_COL32(246, 198, 52, 255)
                                    : (hovered ? IM_COL32(126, 137, 157, 255)
                                               : IM_COL32(72, 82, 99, 255))),
                  scaled(5.0F),
                  0,
                  selected ? scaled(2.5F) : scaled(1.0F));
    if (weapon.equipped) {
        draw->AddRectFilled(ImVec2(start.x + scaled(4.0F), start.y + scaled(4.0F)),
                            ImVec2(start.x + scaled(29.0F), start.y + scaled(22.0F)),
                            ui_color(IM_COL32(15, 18, 24, 235)),
                            scaled(3.0F));
        draw->AddText(ImVec2(start.x + scaled(8.0F), start.y + scaled(6.0F)),
                      ui_color(IM_COL32(246, 198, 52, 255)),
                      "EQ");
    }
    tooltip_name_hash(weapon.name, weapon.item.definitionHash, weapon.definition.definitionIndex);
    ImGui::PopID();
    return clicked;
}

[[nodiscard]] bool draw_perk_socket(const WeaponRow& weapon,
                                    std::uint8_t lane,
                                    bool selected,
                                    float size) noexcept {
    const std::optional<std::uint32_t> hash = current_plug_hash(weapon, lane);
    ImGui::PushID(static_cast<int>(lane));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("perk_socket", ImVec2(size, size));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 end{start.x + size, start.y + size};
    const float rounding = size * 0.5F;
    if (hash.has_value()) {
        draw_icon(*hash, start, end, rounding);
    } else {
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(start.x + size * 0.5F,
                                                           start.y + size * 0.5F),
                                                    size * 0.47F,
                                                    ui_color(IM_COL32(28, 33, 42, 255)));
    }
    ImGui::GetWindowDrawList()->AddCircle(ImVec2(start.x + size * 0.5F, start.y + size * 0.5F),
                                          size * 0.47F,
                                          ui_color(selected ? IM_COL32(246, 198, 52, 255)
                                                            : (hovered ? IM_COL32(155, 166, 185, 255)
                                                                       : IM_COL32(82, 92, 110, 255))),
                                          0,
                                          selected ? scaled(3.0F) : scaled(1.5F));
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::Text("Socket %u", static_cast<unsigned>(lane));
        ImGui::TextDisabled("Type %u", static_cast<unsigned>(weapon.detail.socketTypes[lane]));
        if (hash.has_value()) {
            const std::string_view name = display_name(*hash);
            if (!name.empty()) {
                ImGui::TextUnformatted(name.data(), name.data() + name.size());
            }
            ImGui::TextDisabled("0x%08X", static_cast<unsigned>(*hash));
        } else {
            ImGui::TextDisabled("Empty");
        }
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return clicked;
}

[[nodiscard]] bool draw_extra_socket(const WeaponRow& weapon,
                                     std::uint8_t lane,
                                     SocketKind kind,
                                     bool selected,
                                     float width) noexcept {
    constexpr float kAspect = 1.18F;
    const float height = width * kAspect;
    const float iconSize = width * 0.66F;
    const std::optional<std::uint32_t> hash = current_plug_hash(weapon, lane);
    ImGui::PushID(static_cast<int>(lane));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("extra_socket", ImVec2(width, height));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 end{start.x + width, start.y + height};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start,
                        end,
                        ui_color(hovered ? IM_COL32(39, 45, 56, 255) : IM_COL32(27, 32, 40, 255)),
                        scaled(5.0F));
    draw->AddRect(start,
                  end,
                  ui_color(selected ? IM_COL32(246, 198, 52, 255) : IM_COL32(65, 74, 90, 255)),
                  scaled(5.0F),
                  0,
                  selected ? scaled(2.0F) : scaled(1.0F));
    const ImVec2 iconStart{start.x + (width - iconSize) * 0.5F, start.y + scaled(6.0F)};
    const ImVec2 iconEnd{iconStart.x + iconSize, iconStart.y + iconSize};
    if (hash.has_value()) {
        draw_icon(*hash, iconStart, iconEnd, scaled(4.0F));
    }
    const char* label = socket_kind_label(kind);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(start.x + (width - textSize.x) * 0.5F, end.y - scaled(19.0F)),
                  ui_color(IM_COL32(188, 197, 212, 255)),
                  label);
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(label);
        ImGui::TextDisabled("Socket %u / Type %u",
                            static_cast<unsigned>(lane),
                            static_cast<unsigned>(weapon.detail.socketTypes[lane]));
        if (hash.has_value()) {
            const std::string_view name = display_name(*hash);
            if (!name.empty()) {
                ImGui::TextUnformatted(name.data(), name.data() + name.size());
            }
            ImGui::TextDisabled("0x%08X", static_cast<unsigned>(*hash));
        }
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return clicked;
}

void select_lane(const WeaponRow& weapon, std::uint8_t lane) noexcept {
    if (!editable_lane(weapon, lane)) {
        return;
    }
    g_selectedLane = lane;
    g_browserMode = BrowserMode::plugs;
    g_message[0] = '\0';
    invalidate_plug_cache();
}

void draw_weapon_details(WeaponRow& weapon) noexcept {
    ImGui::TextDisabled("SELECTED WEAPON");
    ImGui::Spacing();

    const float imageSize = scaled(104.0F);
    const ImVec2 imageStart = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("change_weapon", ImVec2(imageSize, imageSize));
    const bool changeClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool imageHovered = ImGui::IsItemHovered();
    const ImVec2 imageEnd{imageStart.x + imageSize, imageStart.y + imageSize};
    draw_icon(weapon.item.definitionHash, imageStart, imageEnd, scaled(5.0F));
    ImGui::GetWindowDrawList()->AddRect(imageStart,
                                        imageEnd,
                                        ui_color(imageHovered ? IM_COL32(246, 198, 52, 255)
                                                              : IM_COL32(92, 102, 122, 255)),
                                        scaled(5.0F),
                                        0,
                                        imageHovered ? scaled(2.0F) : scaled(1.0F));
    if (weapon.equipped) {
        ImGui::GetWindowDrawList()->AddText(ImVec2(imageStart.x + scaled(7.0F),
                                                   imageStart.y + scaled(7.0F)),
                                            ui_color(IM_COL32(246, 198, 52, 255)),
                                            "EQUIPPED");
    }
    if (imageHovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Change weapon definition");
        ImGui::EndTooltip();
    }
    if (changeClicked) {
        g_browserMode = BrowserMode::weapons;
        g_message[0] = '\0';
        invalidate_weapon_catalog();
    }

    ImGui::SameLine(0.0F, scaled(14.0F));
    ImGui::BeginGroup();
    if (!weapon.name.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(weapon.name.data(), weapon.name.data() + weapon.name.size());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextUnformatted("Unknown weapon");
    }
    ImGui::TextDisabled("%s", kWeaponSlotNames[g_activeSlot]);
    ImGui::Spacing();
    ImGui::TextDisabled("Hash  0x%08X", static_cast<unsigned>(weapon.item.definitionHash));
    ImGui::TextDisabled("Definition  %u", static_cast<unsigned>(weapon.definition.definitionIndex));
    ImGui::TextDisabled("Instance  0x%llX", static_cast<unsigned long long>(weapon.item.instanceSoid));
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.72F, 0.76F, 0.83F, 1.0F), "Click the weapon to replace it");
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    std::array<std::uint8_t, inventory::kPlugCapacity> perkLanes{};
    std::array<std::uint8_t, inventory::kPlugCapacity> extraLanes{};
    std::array<SocketKind, inventory::kPlugCapacity> extraKinds{};
    std::size_t perkCount = 0;
    std::size_t extraCount = 0;
    for (std::uint8_t lane = 0; lane < weapon.detail.ordinarySocketCount; ++lane) {
        const SocketKind kind = socket_kind(weapon, lane);
        if (kind == SocketKind::hidden) {
            continue;
        }
        if (kind == SocketKind::perk) {
            perkLanes[perkCount++] = lane;
        } else {
            extraLanes[extraCount] = lane;
            extraKinds[extraCount] = kind;
            ++extraCount;
        }
    }

    ImGui::TextDisabled("WEAPON PERKS");
    const float width = ImGui::GetContentRegionAvail().x;
    const std::size_t perkColumns = (std::max)(std::size_t{1}, (std::min)(perkCount, std::size_t{6}));
    const float perkSize = (std::clamp)(
        (width - kSocketGap * static_cast<float>(perkColumns - 1)) / static_cast<float>(perkColumns),
        scaled(34.0F),
        scaled(48.0F));
    for (std::size_t index = 0; index < perkCount; ++index) {
        if (index % perkColumns != 0) {
            ImGui::SameLine(0.0F, scaled(kSocketGap));
        }
        const std::uint8_t lane = perkLanes[index];
        if (draw_perk_socket(weapon, lane, lane == g_selectedLane, perkSize)) {
            select_lane(weapon, lane);
        }
    }
    if (perkCount == 0) {
        ImGui::TextDisabled("No perk sockets");
    }

    if (extraCount != 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("EXTRA SLOTS");
        const std::size_t extraColumns = (std::max)(std::size_t{1}, (std::min)(extraCount, std::size_t{5}));
        const float extraWidth = (std::clamp)(
            (width - kSocketGap * static_cast<float>(extraColumns - 1)) / static_cast<float>(extraColumns),
            scaled(54.0F),
            scaled(78.0F));
        for (std::size_t index = 0; index < extraCount; ++index) {
            if (index % extraColumns != 0) {
                ImGui::SameLine(0.0F, scaled(kSocketGap));
            }
            const std::uint8_t lane = extraLanes[index];
            if (draw_extra_socket(weapon,
                                  lane,
                                  extraKinds[index],
                                  lane == g_selectedLane,
                                  extraWidth)) {
                select_lane(weapon, lane);
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const std::optional<std::uint32_t> current = current_plug_hash(weapon, g_selectedLane);
    ImGui::TextDisabled("Selected: %s / Socket %u",
                        socket_kind_label(socket_kind(weapon, g_selectedLane)),
                        static_cast<unsigned>(g_selectedLane));
    if (current.has_value()) {
        const std::string_view name = display_name(*current);
        if (!name.empty()) {
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("0x%08X", static_cast<unsigned>(*current));
    } else {
        ImGui::TextDisabled("Empty");
    }
}

void draw_inventory_panel() noexcept {
    ImGui::TextDisabled("WEAPON INVENTORY");
    ImGui::Spacing();

    const float tabGap = scaled(8.0F);
    const float tabWidth = (ImGui::GetContentRegionAvail().x - tabGap * 2.0F) / 3.0F;
    for (std::size_t slot = 0; slot < kWeaponSlotNames.size(); ++slot) {
        if (slot != 0) {
            ImGui::SameLine(0.0F, tabGap);
        }
        if (slot == g_activeSlot) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.29F, 0.24F, 0.07F, 1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38F, 0.31F, 0.09F, 1.0F));
        }
        if (ImGui::Button(kWeaponSlotNames[slot], ImVec2(tabWidth, scaled(30.0F)))) {
            if (g_activeSlot != slot) {
                g_activeSlot = slot;
                if (WeaponRow* weapon = selected_weapon(); weapon != nullptr) {
                    reset_selection_for_weapon(*weapon);
                } else {
                    g_selectedLane = 0;
                    invalidate_plug_cache();
                    invalidate_weapon_catalog();
                }
            }
        }
        if (slot == g_activeSlot) {
            ImGui::PopStyleColor(2);
        }
    }

    ImGui::Spacing();
    auto& rows = g_slotWeapons[g_activeSlot];
    if (rows.empty()) {
        ImGui::TextWrapped("No weapon instances were found for this slot.");
        return;
    }

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float horizontal = (availableWidth - scaled(kInventoryGap) * static_cast<float>(kWeaponColumns - 1))
                           / static_cast<float>(kWeaponColumns);
    const float vertical = (availableHeight - scaled(kInventoryGap)) * 0.5F;
    const float cardSize = (std::max)(scaled(34.0F), (std::min)(horizontal, vertical));
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (index % kWeaponColumns != 0) {
            ImGui::SameLine(0.0F, scaled(kInventoryGap));
        }
        if (draw_weapon_card(rows[index],
                             index,
                             index == g_selectedWeaponBySlot[g_activeSlot],
                             cardSize)) {
            if (g_selectedWeaponBySlot[g_activeSlot] != index) {
                g_selectedWeaponBySlot[g_activeSlot] = index;
                reset_selection_for_weapon(rows[index]);
            }
        }
    }
}

[[nodiscard]] bool draw_plug_card(const PlugChoice& choice,
                                  std::size_t plugIndex,
                                  bool selected,
                                  float width) noexcept {
    const float cardHeight = scaled(kPlugCardHeight);
    const float iconSize = scaled(kPlugIconSize);
    ImGui::PushID(static_cast<int>(plugIndex));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("plug_card", ImVec2(width, cardHeight));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end{start.x + width, start.y + cardHeight};
    draw->AddRectFilled(start,
                        end,
                        ui_color(hovered ? IM_COL32(42, 48, 60, 255) : IM_COL32(29, 34, 43, 255)),
                        scaled(5.0F));
    draw->AddRect(ImVec2(start.x + 1.0F, start.y + 1.0F),
                  ImVec2(end.x - 1.0F, end.y - 1.0F),
                  ui_color(selected ? IM_COL32(246, 198, 52, 255) : IM_COL32(65, 73, 88, 255)),
                  scaled(5.0F),
                  0,
                  selected ? scaled(2.0F) : scaled(1.0F));

    const ImVec2 iconStart{start.x + scaled(7.0F), start.y + scaled(9.0F)};
    const ImVec2 iconEnd{iconStart.x + iconSize, iconStart.y + iconSize};
    draw_icon(choice.definition.definitionHash, iconStart, iconEnd, iconSize * 0.5F);

    const float textX = iconEnd.x + scaled(8.0F);
    const float textWidth = (std::max)(scaled(20.0F), end.x - textX - scaled(7.0F));
    if (!choice.name.empty()) {
        const char* begin = choice.name.data();
        const char* finish = begin + choice.name.size();
        const ImVec2 clippedMax{end.x - scaled(6.0F), start.y + scaled(36.0F)};
        draw->PushClipRect(ImVec2(textX, start.y + scaled(5.0F)), clippedMax, true);
        draw->AddText(ImGui::GetFont(),
                      ImGui::GetFontSize(),
                      ImVec2(textX, start.y + scaled(9.0F)),
                      ui_color(IM_COL32_WHITE),
                      begin,
                      finish,
                      textWidth);
        draw->PopClipRect();
    } else {
        draw->AddText(ImVec2(textX, start.y + scaled(9.0F)),
                      ui_color(IM_COL32_WHITE),
                      "Unknown plug");
    }
    std::array<char, 40> technical{};
    (void)std::snprintf(technical.data(),
                        technical.size(),
                        "0x%08X",
                        static_cast<unsigned>(choice.definition.definitionHash));
    draw->AddText(ImVec2(textX, start.y + scaled(43.0F)),
                  ui_color(IM_COL32(153, 164, 181, 255)),
                  technical.data());

    tooltip_name_hash(choice.name, choice.definition.definitionHash, choice.definition.definitionIndex);
    ImGui::PopID();
    return clicked;
}

[[nodiscard]] bool draw_weapon_choice_card(const WeaponChoice& choice,
                                           std::size_t index,
                                           bool selected,
                                           float width) noexcept {
    const float height = scaled(kWeaponCatalogCardHeight);
    const float iconSize = height - scaled(12.0F);
    ImGui::PushID(static_cast<int>(index));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("weapon_choice", ImVec2(width, height));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 end{start.x + width, start.y + height};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start,
                        end,
                        ui_color(hovered ? IM_COL32(42, 48, 60, 255) : IM_COL32(29, 34, 43, 255)),
                        scaled(5.0F));
    draw->AddRect(start,
                  end,
                  ui_color(selected ? IM_COL32(246, 198, 52, 255) : IM_COL32(65, 73, 88, 255)),
                  scaled(5.0F),
                  0,
                  selected ? scaled(2.0F) : scaled(1.0F));
    const ImVec2 iconStart{start.x + scaled(6.0F), start.y + scaled(6.0F)};
    const ImVec2 iconEnd{iconStart.x + iconSize, iconStart.y + iconSize};
    draw_icon(choice.definition.definitionHash, iconStart, iconEnd, scaled(4.0F));
    const float textX = iconEnd.x + scaled(8.0F);
    if (!choice.name.empty()) {
        draw->AddText(ImVec2(textX, start.y + scaled(12.0F)),
                      ui_color(IM_COL32_WHITE),
                      choice.name.data(),
                      choice.name.data() + choice.name.size());
    }
    std::array<char, 32> technical{};
    (void)std::snprintf(technical.data(),
                        technical.size(),
                        "0x%08X",
                        static_cast<unsigned>(choice.definition.definitionHash));
    draw->AddText(ImVec2(textX, start.y + scaled(40.0F)),
                  ui_color(IM_COL32(153, 164, 181, 255)),
                  technical.data());
    tooltip_name_hash(choice.name, choice.definition.definitionHash, choice.definition.definitionIndex);
    ImGui::PopID();
    return clicked;
}

void draw_plug_browser(WeaponRow& weapon) noexcept {
    rebuild_plugs(weapon);

    ImGui::TextDisabled("PLUG BROWSER");
    ImGui::SameLine();
    ImGui::Text("%s / Socket %u",
                socket_kind_label(socket_kind(weapon, g_selectedLane)),
                static_cast<unsigned>(g_selectedLane));
    ImGui::Spacing();

    const float width = ImGui::GetContentRegionAvail().x;
    const float searchWidth = (std::max)(scaled(120.0F), width - scaled(165.0F));
    ImGui::SetNextItemWidth(searchWidth);
    ImGui::InputTextWithHint("##plug_search",
                             "Search plug name or hash...",
                             g_search.data(),
                             g_search.size());
    ImGui::SameLine();
    const char* filterPreview = g_filter == PlugFilter::compatible ? "Compatible" : "All plugs";
    ImGui::SetNextItemWidth(scaled(145.0F));
    if (ImGui::BeginCombo("##plug_filter", filterPreview)) {
        if (ImGui::Selectable("Compatible", g_filter == PlugFilter::compatible)) {
            g_filter = PlugFilter::compatible;
            invalidate_plug_cache();
        }
        if (ImGui::Selectable("All plugs", g_filter == PlugFilter::all)) {
            g_filter = PlugFilter::all;
            invalidate_plug_cache();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth((std::max)(scaled(120.0F), width - scaled(84.0F)));
    ImGui::InputTextWithHint("##plug_hash", "Exact hex hash...", g_hashInput.data(), g_hashInput.size());
    ImGui::SameLine();
    if (ImGui::Button("Find", ImVec2(scaled(68.0F), 0.0F))) {
        select_hash(weapon);
    }

    if (g_filter == PlugFilter::all) {
        ImGui::TextColored(ImVec4(0.93F, 0.72F, 0.23F, 1.0F),
                           "Unrestricted: incompatible plugs may break the item.");
    }

    rebuild_plugs(weapon);
    rebuild_visible_plugs();
    ImGui::TextDisabled("%zu matching plugs", g_visiblePlugs.size());
    ImGui::Spacing();

    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = (std::max)(1, static_cast<int>(available / scaled(190.0F)));
    const float gap = scaled(7.0F);
    const float cardWidth = (available - static_cast<float>(columns - 1) * gap)
                          / static_cast<float>(columns);
    const std::size_t rowCount = (g_visiblePlugs.size() + static_cast<std::size_t>(columns) - 1)
                               / static_cast<std::size_t>(columns);

    const float browserHeight = (std::max)(scaled(95.0F), ImGui::GetContentRegionAvail().y - scaled(44.0F));
    if (ImGui::BeginChild("plug_grid",
                          ImVec2(0.0F, browserHeight),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rowCount), scaled(kPlugCardHeight + 7.0F));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const std::size_t base = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns);
                for (int column = 0; column < columns; ++column) {
                    const std::size_t visibleIndex = base + static_cast<std::size_t>(column);
                    if (visibleIndex >= g_visiblePlugs.size()) {
                        break;
                    }
                    if (column != 0) {
                        ImGui::SameLine(0.0F, gap);
                    }
                    const std::size_t plugIndex = g_visiblePlugs[visibleIndex];
                    if (draw_plug_card(g_plugs[plugIndex],
                                       plugIndex,
                                       plugIndex == g_selectedPlug,
                                       cardWidth)) {
                        g_selectedPlug = plugIndex;
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    const bool canApply = !g_plugs.empty() && g_selectedPlug < g_plugs.size();
    if (!canApply) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Apply changes", ImVec2(scaled(132.0F), scaled(30.0F)))) {
        apply_selected(weapon);
    }
    if (!canApply) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(scaled(82.0F), scaled(30.0F)))) {
        (void)server::bap::request_account_resync();
    }
}

void draw_weapon_browser(WeaponRow& weapon) noexcept {
    rebuild_weapon_catalog(weapon);
    rebuild_visible_weapon_catalog();

    ImGui::TextDisabled("WEAPON CATALOG");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kWeaponSlotNames[g_activeSlot]);
    ImGui::SameLine();
    if (ImGui::SmallButton("Back to plugs")) {
        g_browserMode = BrowserMode::plugs;
        return;
    }
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##weapon_search",
                             "Search weapon name or hash...",
                             g_weaponSearch.data(),
                             g_weaponSearch.size());
    rebuild_visible_weapon_catalog();
    ImGui::TextDisabled("%zu matching weapons", g_visibleWeaponCatalog.size());
    ImGui::Spacing();

    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = (std::max)(1, static_cast<int>(available / scaled(225.0F)));
    const float gap = scaled(7.0F);
    const float cardWidth = (available - static_cast<float>(columns - 1) * gap)
                          / static_cast<float>(columns);
    const std::size_t rowCount = (g_visibleWeaponCatalog.size() + static_cast<std::size_t>(columns) - 1)
                               / static_cast<std::size_t>(columns);
    const float browserHeight = (std::max)(scaled(110.0F), ImGui::GetContentRegionAvail().y - scaled(44.0F));

    if (ImGui::BeginChild("weapon_catalog_grid",
                          ImVec2(0.0F, browserHeight),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rowCount), scaled(kWeaponCatalogCardHeight + 7.0F));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const std::size_t base = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns);
                for (int column = 0; column < columns; ++column) {
                    const std::size_t visibleIndex = base + static_cast<std::size_t>(column);
                    if (visibleIndex >= g_visibleWeaponCatalog.size()) {
                        break;
                    }
                    if (column != 0) {
                        ImGui::SameLine(0.0F, gap);
                    }
                    const std::size_t choiceIndex = g_visibleWeaponCatalog[visibleIndex];
                    if (draw_weapon_choice_card(g_weaponCatalog[choiceIndex],
                                                choiceIndex,
                                                choiceIndex == g_selectedReplacement,
                                                cardWidth)) {
                        g_selectedReplacement = choiceIndex;
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    const bool canReplace = g_selectedReplacement < g_weaponCatalog.size()
                         && g_weaponCatalog[g_selectedReplacement].definition.definitionHash
                                != weapon.item.definitionHash;
    if (!canReplace) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Replace weapon", ImVec2(scaled(132.0F), scaled(30.0F)))) {
        replace_selected_weapon(weapon);
    }
    if (!canReplace) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Native default perks are restored first.");
}

void draw_zoom_controls() noexcept {
    const float right = ImGui::GetWindowContentRegionMax().x;
    const float controlsWidth = scaled(230.0F);
    const float desired = right - controlsWidth;
    if (desired > ImGui::GetCursorPosX() + scaled(20.0F)) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(desired);
    } else {
        ImGui::SameLine();
    }
    ImGui::TextDisabled("UI");
    ImGui::SameLine();
    if (ImGui::SmallButton("-")) {
        g_uiScale = (std::max)(0.75F, g_uiScale - 0.05F);
    }
    ImGui::SameLine();
    ImGui::Text("%d%%", static_cast<int>(g_uiScale * 100.0F + 0.5F));
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) {
        g_uiScale = (std::min)(1.20F, g_uiScale + 0.05F);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        g_uiScale = 1.0F;
    }
}

} // namespace

void draw() noexcept {
    if (!state::build_data::item_definitions_ready()
        || !state::build_data::configured_item_details_ready()
        || !state::build_data::socket_plug_rules_ready()) {
        ImGui::TextWrapped("Weapon Editor is waiting for item/socket build data...");
        return;
    }

    const state::AccountState account = state::account_snapshot();
    const std::size_t characterIndex = selected_character_index(account);
    if (characterIndex >= account.characterCount) {
        ImGui::TextWrapped("Select a character first.");
        return;
    }

    rebuild_weapons(account.characters[characterIndex]);
    WeaponRow* weapon = selected_weapon();
    if (weapon == nullptr) {
        ImGui::TextWrapped("No weapon instances with editable sockets were found for this slot.");
        return;
    }
    if (g_selectedLane >= weapon->detail.ordinarySocketCount || !editable_lane(*weapon, g_selectedLane)) {
        g_selectedLane = first_editable_lane(*weapon);
        invalidate_plug_cache();
    }

    ImGui::TextUnformatted("Weapon Editor");
    ImGui::SameLine();
    ImGui::TextDisabled("Live Socket & Item Editor");
    draw_zoom_controls();
    if (g_uiScale != 1.0F) {
        ImGui::TextColored(ImVec4(0.93F, 0.72F, 0.23F, 1.0F),
                           "UI scale may clip content. Reset returns to 100%%.");
    }
    ImGui::Separator();
    ImGui::Spacing();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float gap = scaled(10.0F);
    const float detailsWidth = (std::clamp)(available.x * 0.40F, scaled(360.0F), scaled(515.0F));
    const float rightWidth = (std::max)(scaled(320.0F), available.x - detailsWidth - gap);
    constexpr ImGuiWindowFlags kFixedPanelFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginChild("weapon_details",
                          ImVec2(detailsWidth, 0.0F),
                          ImGuiChildFlags_Borders,
                          kFixedPanelFlags)) {
        draw_weapon_details(*weapon);
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0F, gap);
    if (ImGui::BeginChild("weapon_right_column",
                          ImVec2(rightWidth, 0.0F),
                          ImGuiChildFlags_None,
                          kFixedPanelFlags)) {
        const float inventoryHeight = (std::clamp)(ImGui::GetContentRegionAvail().y * 0.32F,
                                                    scaled(210.0F),
                                                    scaled(258.0F));
        if (ImGui::BeginChild("weapon_inventory",
                              ImVec2(0.0F, inventoryHeight),
                              ImGuiChildFlags_Borders,
                              kFixedPanelFlags)) {
            draw_inventory_panel();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        WeaponRow* afterSelection = selected_weapon();
        if (afterSelection != nullptr) {
            if (g_selectedLane >= afterSelection->detail.ordinarySocketCount
                || !editable_lane(*afterSelection, g_selectedLane)) {
                g_selectedLane = first_editable_lane(*afterSelection);
                invalidate_plug_cache();
            }
            if (ImGui::BeginChild("weapon_browser",
                                  ImVec2(0.0F, 0.0F),
                                  ImGuiChildFlags_Borders,
                                  kFixedPanelFlags)) {
                if (g_browserMode == BrowserMode::weapons) {
                    draw_weapon_browser(*afterSelection);
                } else {
                    draw_plug_browser(*afterSelection);
                }
                if (g_message[0] != '\0') {
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", g_message.data());
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
}

} // namespace sunrise::server::ui::weapon_editor
