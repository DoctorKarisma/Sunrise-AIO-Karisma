#include "spawn_panel.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <span>
#include <string_view>
#include <vector>

#include "../../../client/content/items/packages/internal.h"
#include "../../../client/content/placements/placement_extract.h"
#include "../../../client/hooks/spawn/spawn_runtime.h"
#include "../../../client/player/player_position.h"
#include "../../../client/spawn/population_settings_store.h"
#include "../../../client/spawn/spawn_keybind_store.h"
#include "../../../core/filesystem/path.h"
#include "../../../core/ui/components/picker/ui_picker_component.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/activity/definition.h"
#include "../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::server::ui::spawn {
namespace {

namespace native = client::hooks::spawn;
namespace spawn_keys = client::spawn;
namespace package_reader = middleware::content::packages::reader;
namespace picker = core::ui::components::picker;

constexpr std::uint32_t kEntityClass = 0x80809C0FU;

enum class ObjectType : std::uint8_t {
    Inherited = 0,
    StaticMesh = 1, // Interactable
    PropSimpleDeprecated = 2,
    PropExpensiveDeprecated = 3,
    PropCosmeticStatic = 4,  // World effects / decorations
    PropCosmeticMovable = 5, // Moving props
    PropCosmeticMovableGarbage = 6,
    PropNetworkedStatic = 7,  // Ad spawns
    PropNetworkedMovable = 8, // Explodable
    PropCinematic = 9,
    Speedtree = 10,
    Interactive = 11,
    Biped = 12, // Guardians, Enemies, NPCs
    Creature = 13,
    Weapon = 14,  // Weapon props
    Vehicle = 15, // Sparrows, Pikes, Ships
    Turret = 16,  // VehicleEntity
    Emitter = 17, // Effects, some interactive projectiles
    Projectile = 18,
    Item = 19,
    ItemAmmo = 20,
    ItemLoot = 21,
    Gear = 22,
    HopOn = 23,
    HopOnGearBiped = 24,
    HopOnGearWeapon = 25,
    HopOnGearShip = 26,
    HopOnGearSparrow = 27,
    System = 28,
    Invalid = 0xFF,
};

constexpr std::array<const char*, 29> kObjectTypeNames{
    "Inherited",
    "StaticMesh",
    "PropSimpleDeprecated",
    "PropExpensiveDeprecated",
    "PropCosmeticStatic",
    "PropCosmeticMovable",
    "PropCosmeticMovableGarbage",
    "PropNetworkedStatic",
    "PropNetworkedMovable",
    "PropCinematic",
    "Speedtree",
    "Interactive",
    "Biped",
    "Creature",
    "Weapon",
    "Vehicle",
    "Turret",
    "Emitter",
    "Projectile",
    "Item",
    "ItemAmmo",
    "ItemLoot",
    "Gear",
    "HopOn",
    "HopOnGearBiped",
    "HopOnGearWeapon",
    "HopOnGearShip",
    "HopOnGearSparrow",
    "System",
};

static_assert(kObjectTypeNames.size() == static_cast<std::uint8_t>(ObjectType::System) + 1);

enum class SpawnAllMode : std::uint8_t {
    none,
    all,
    selectedType,
};

struct Candidate {
    std::uint32_t tag{};
    ObjectType type{};
    bool named{};
    std::array<char, 224> label{};
};

struct Column {
    std::vector<Candidate> candidates{};
    std::vector<picker::Item> items{};
    std::size_t selected{};
    native::Settings settings{};
    int amount{1};
    int perRow{10};
    float spacing{1.0F};
};

Column g_main{};
Column g_projectile{};
Column g_loot{};
std::vector<Candidate> g_allMainCandidates{};
std::vector<state::build_data::entity_names::Name> g_names{};
bool g_scanned{};
std::size_t g_capturingKey{spawn_keys::kActionCount};

enum class PopulationSource : int {
    visibleMain = 0,
    selectedMain = 1,
    map = 2,
};

native::PopulationSettings g_population{};
PopulationSource g_populationSource{PopulationSource::visibleMain};
bool g_populationPrimed{};
bool g_extractCombatantsOnly{true};
bool g_extractPublicOnly{true};
client::content::placements::ExtractResult g_lastExtract{};
bool g_lastExtractValid{};

void key_name(std::uint32_t virtualKey, std::array<char, 64>& output) noexcept {
    if (virtualKey == spawn_keys::kNoKey) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, 64> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                        wide.data(),
                                                        static_cast<int>(wide.size()))
                                      : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

[[nodiscard]] bool capture_key(std::uint32_t& output) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        output = spawn_keys::kNoKey;
        return true;
    }
    for (int key = 7; key <= 254; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            output = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool
key_picker(spawn_keys::Action action, std::uint32_t& virtualKey, float width) noexcept {
    const std::size_t index = static_cast<std::size_t>(action);
    ImGui::PushID(static_cast<int>(index));
    if (g_capturingKey == index) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturingKey = spawn_keys::kActionCount;
        }
        ImGui::PopID();
        std::uint32_t picked = spawn_keys::kNoKey;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturingKey = spawn_keys::kActionCount;
            return true;
        }
        return false;
    }
    std::array<char, 64> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturingKey = index;
    }
    return false;
}

[[nodiscard]] std::span<const state::build_data::entity_names::Name>
names_of(std::uint32_t tag) noexcept {
    const auto first = std::lower_bound(
        g_names.begin(), g_names.end(), tag, [](const auto& name, std::uint32_t wanted) {
            return name.tag < wanted;
        });
    const auto last =
        std::upper_bound(first, g_names.end(), tag, [](std::uint32_t wanted, const auto& name) {
            return wanted < name.tag;
        });
    return {first, last};
}

[[nodiscard]] bool projectile_name(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 12> markers{
        "projectile",
        "missile",
        "rocket",
        "grenade",
        "fireball",
        "mortar",
        "cannonball",
        "seeker",
        "tracer",
        "bullet",
        "plasma_bolt",
        "weapon_bolt",
    };
    return std::any_of(markers.begin(), markers.end(), [name](std::string_view marker) {
        return name.find(marker) != std::string_view::npos;
    });
}

void family_text(std::wstring_view family, std::array<char, 96>& output) noexcept {
    output = {};
    const std::size_t count = (std::min)(family.size(), output.size() - 1);
    for (std::size_t index = 0; index < count; ++index) {
        const wchar_t value = family[index];
        output[index] = value >= 32 && value <= 126 ? static_cast<char>(value) : '?';
    }
}

[[nodiscard]] constexpr const char* object_type_name(ObjectType type) noexcept {
    if (type == ObjectType::Invalid) {
        return "Invalid";
    }
    const std::size_t index = static_cast<std::uint8_t>(type);
    return index < kObjectTypeNames.size() ? kObjectTypeNames[index] : nullptr;
}

constexpr std::uint64_t kUnknownObjectTypeBit = 1ULL << 62;
constexpr std::uint64_t kInvalidObjectTypeBit = 1ULL << 63;

[[nodiscard]] constexpr std::uint64_t object_type_filter_bit(ObjectType type) noexcept {
    if (object_type_name(type) == nullptr) {
        return kUnknownObjectTypeBit;
    }
    if (type == ObjectType::Invalid) {
        return kInvalidObjectTypeBit;
    }
    return 1ULL << static_cast<std::uint8_t>(type);
}

void add_candidate(Column& column,
                   std::uint32_t tag,
                   std::uint8_t type,
                   std::wstring_view family,
                   const state::build_data::entity_names::Name* resolvedName) {
    std::array<char, 96> package{};
    family_text(family, package);
    Candidate value{};
    value.tag = tag;
    value.type = static_cast<ObjectType>(type);
    const char* const name = resolvedName != nullptr ? resolvedName->text.data() : nullptr;
    const char* typeName = object_type_name(value.type);
    std::array<char, 32> unknownType{};
    if (typeName == nullptr) {
        (void)std::snprintf(unknownType.data(), unknownType.size(), "Unknown(%u)", type);
        typeName = unknownType.data();
    }
    value.named = name != nullptr;
    if (name != nullptr) {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "%s | %s | 0x%08X | %s",
                            name,
                            typeName,
                            tag,
                            package.data());
    } else {
        (void)std::snprintf(value.label.data(),
                            value.label.size(),
                            "0x%08X | %s | %s",
                            tag,
                            typeName,
                            package.data());
    }
    column.candidates.push_back(value);
}

bool collect_entity(void*, const package_reader::ClassEntry& entry) noexcept {
    if (!native::is_tag_resident(entry.tag)) {
        return true;
    }
    std::uint8_t type = 0;
    if (!native::object_type(entry.tag, type)) {
        return true;
    }
    const auto names = names_of(entry.tag);
    const auto objectType = static_cast<ObjectType>(type);
    const bool namedProjectile = std::any_of(names.begin(), names.end(), [](const auto& name) {
        return projectile_name({name.text.data(), name.length});
    });
    Column* const column =
        objectType == ObjectType::Projectile || namedProjectile                    ? &g_projectile
        : objectType == ObjectType::ItemAmmo || objectType == ObjectType::ItemLoot ? &g_loot
                                                                                   : &g_main;
    if (names.empty()) {
        add_candidate(*column, entry.tag, type, entry.packageFamily, nullptr);
    } else {
        for (const auto& name : names) {
            add_candidate(*column, entry.tag, type, entry.packageFamily, &name);
        }
    }
    return true;
}

void finish_column(Column& column) {
    std::sort(column.candidates.begin(),
              column.candidates.end(),
              [](const Candidate& first, const Candidate& second) {
                  if (first.named != second.named) {
                      return first.named;
                  }
                  return std::string_view(first.label.data())
                         < std::string_view(second.label.data());
              });
    column.candidates.erase(std::unique(column.candidates.begin(),
                                        column.candidates.end(),
                                        [](const Candidate& first, const Candidate& second) {
                                            return first.tag == second.tag
                                                   && std::string_view(first.label.data())
                                                          == std::string_view(second.label.data());
                                        }),
                            column.candidates.end());
    column.items.clear();
    column.items.reserve(column.candidates.size());
    for (const Candidate& candidate : column.candidates) {
        column.items.push_back({candidate.label.data()});
    }
    column.selected = 0;
}

void apply_main_type_filter(std::uint64_t hiddenTypes) {
    g_main.candidates.clear();
    g_main.candidates.reserve(g_allMainCandidates.size());
    for (const Candidate& candidate : g_allMainCandidates) {
        if ((hiddenTypes & object_type_filter_bit(candidate.type)) == 0) {
            g_main.candidates.push_back(candidate);
        }
    }
    finish_column(g_main);
}

void refresh() noexcept {
    g_main.candidates.clear();
    g_projectile.candidates.clear();
    g_loot.candidates.clear();
    g_allMainCandidates.clear();
    g_names.resize(state::build_data::entity_name_count());
    std::size_t nameCount = 0;
    if (!state::build_data::snapshot_entity_names(g_names, nameCount)) {
        g_names.clear();
    } else {
        g_names.resize(nameCount);
    }
    core::path::Buffer directory{};
    const bool hasDirectory = client::content::items::packages::package_directory(directory);
    if (native::ready() && hasDirectory) {
        package_reader::ScanResult result{};
        (void)package_reader::scan_class_entries(
            directory.chars.data(), kEntityClass, &collect_entity, nullptr, result);
        package_reader::release_caches();
    }
    finish_column(g_main);
    g_allMainCandidates = g_main.candidates;
    apply_main_type_filter(spawn_keys::get().hiddenMainTypes);
    finish_column(g_projectile);
    finish_column(g_loot);
    g_scanned = true;
}

[[nodiscard]] bool current_destination(std::string_view& output) noexcept {
    const std::uint64_t sessionId =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    if (sessionId == state::activity::kAbsentSessionId) {
        return false;
    }

    state::activity::destination::DestinationSelection selection{};
    if (!state::activity::destination::snapshot(sessionId, selection)) {
        return false;
    }

    output = std::string_view(reinterpret_cast<const char*>(selection.packageName.data()),
                              selection.packageNameLength);
    return !output.empty();
}

void publish_map_points() noexcept {
    std::vector<spawn_keys::MapPoint> stored(spawn_keys::map_size());
    const std::size_t count = spawn_keys::copy_map(stored);
    std::vector<native::PopulationPoint> points{};
    points.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        native::PopulationPoint point{};
        point.tag = stored[index].tag;
        point.position = stored[index].position;
        points.push_back(point);
    }
    native::set_population_points(points);
}

void publish_population_source() noexcept {
    if (g_populationSource == PopulationSource::map) {
        publish_map_points();
        return;
    }

    std::vector<std::uint32_t> tags{};
    if (g_populationSource == PopulationSource::selectedMain) {
        if (g_main.selected < g_main.candidates.size()) {
            tags.push_back(g_main.candidates[g_main.selected].tag);
        }
    } else {
        tags.reserve(g_main.candidates.size());
        for (const Candidate& candidate : g_main.candidates) {
            tags.push_back(candidate.tag);
        }
        std::sort(tags.begin(), tags.end());
        tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    }

    native::set_population_tags(tags);
}

void draw_population_tab() noexcept {
    if (!g_populationPrimed) {
        g_population = native::population();
        g_populationSource =
            g_population.useMap ? PopulationSource::map : PopulationSource::visibleMain;
        publish_population_source();
        g_populationPrimed = true;
    }

    bool changed = false;

    changed = ImGui::Checkbox("Populate the world", &g_population.enabled) || changed;
    ImGui::SameLine();
    changed = ImGui::Checkbox("Populate on load", &g_population.autoOnLoad) || changed;
    ImGui::SameLine();
    ImGui::TextDisabled("%zu live | %zu source entities",
                        native::population_live(),
                        native::population_source_count());

    int source = static_cast<int>(g_populationSource);
    bool sourceChanged = ImGui::RadioButton("Visible main entities", &source, 0);
    ImGui::SameLine();
    sourceChanged = ImGui::RadioButton("Selected main entity", &source, 1) || sourceChanged;
    ImGui::SameLine();
    sourceChanged = ImGui::RadioButton("Recorded map", &source, 2) || sourceChanged;
    if (sourceChanged) {
        g_populationSource = static_cast<PopulationSource>(source);
        g_population.useMap = g_populationSource == PopulationSource::map;
        publish_population_source();
        changed = true;
    }

    if (g_populationSource != PopulationSource::map) {
        if (ImGui::Button("Apply source")) {
            publish_population_source();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(g_populationSource == PopulationSource::selectedMain
                                ? "Uses the entity currently selected under Main spawner."
                                : "Uses the entities currently visible under Main spawner.");
    } else {
        std::string_view destination{};
        const bool located = current_destination(destination);

        ImGui::Separator();
        ImGui::TextUnformatted("Population map");
        ImGui::TextDisabled("Destination: %.*s | %zu recorded | %zu published",
                            static_cast<int>(located ? destination.size() : 7),
                            located ? destination.data() : "unknown",
                            spawn_keys::map_size(),
                            native::population_point_count());

        const bool hasEntity = g_main.selected < g_main.candidates.size();
        if (ImGui::Button("Record point here") && hasEntity) {
            const client::player::position::Snapshot player = client::player::position::snapshot();
            if (player.present) {
                spawn_keys::MapPoint point{};
                point.tag = g_main.candidates[g_main.selected].tag;
                point.position = player.position;
                if (spawn_keys::add_map_point(point)) {
                    publish_map_points();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Undo last")) {
            if (spawn_keys::remove_last_map_point()) {
                publish_map_points();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear map")) {
            spawn_keys::clear_map();
            publish_map_points();
        }

        ImGui::BeginDisabled(!located);
        if (ImGui::Button("Save map")) {
            (void)spawn_keys::save_map(destination);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load map")) {
            (void)spawn_keys::load_map(destination);
            publish_map_points();
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!located);
        if (ImGui::Button("Extract authored placements")) {
            client::content::placements::ExtractResult extracted{};
            if (client::content::placements::extract(
                    destination, g_extractCombatantsOnly, g_extractPublicOnly, extracted)) {
                g_lastExtract = extracted;
                g_lastExtractValid = true;
                publish_map_points();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Combatants only", &g_extractCombatantsOnly);
        ImGui::SameLine();
        ImGui::Checkbox("Public areas only", &g_extractPublicOnly);

        if (g_lastExtractValid) {
            ImGui::TextDisabled(
                "Extracted: %zu placements, %zu kept, %zu non-combatants, %zu not resident, "
                "%zu private, %zu overflow%s",
                g_lastExtract.placements,
                g_lastExtract.kept,
                g_lastExtract.notCombatant,
                g_lastExtract.notResident,
                g_lastExtract.notPublic,
                g_lastExtract.overflowed,
                g_lastExtract.budgetHit ? ", budget hit" : "");
        }

        constexpr std::array<const char*, 10> kOutcomes{
            "idle",
            "placed",
            "disabled",
            "no player",
            "no points",
            "at target",
            "none in range",
            "not resident",
            "no ground",
            "spawn failed",
        };
        const native::PopulationStatus status = native::population_status();
        const std::size_t outcomeIndex = static_cast<std::size_t>(status.last) < kOutcomes.size()
                                             ? static_cast<std::size_t>(status.last)
                                             : 0;
        if (status.nearest >= 0.0F) {
            ImGui::TextDisabled("Map: %zu points | nearest free %.0f | last: %s",
                                status.points,
                                static_cast<double>(status.nearest),
                                kOutcomes[outcomeIndex]);
        } else {
            ImGui::TextDisabled(
                "Map: %zu points | last: %s", status.points, kOutcomes[outcomeIndex]);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Population settings");

    int target = static_cast<int>(g_population.target);
    if (ImGui::SliderInt("Live count", &target, 1, 256)) {
        g_population.target = static_cast<std::uint32_t>(target);
        changed = true;
    }

    int interval = static_cast<int>(g_population.intervalMs);
    if (ImGui::SliderInt("Placement interval (ms)", &interval, 100, 5000)) {
        g_population.intervalMs = static_cast<std::uint32_t>(interval);
        changed = true;
    }

    int respawn = static_cast<int>(g_population.respawnDelayMs);
    if (ImGui::SliderInt("Respawn delay (ms)", &respawn, 0, 300000)) {
        g_population.respawnDelayMs = static_cast<std::uint32_t>(respawn);
        changed = true;
    }

    changed = ImGui::SliderFloat("Nearest distance", &g_population.minimumRadius, 0.0F, 120.0F)
              || changed;
    changed = ImGui::SliderFloat("Furthest distance", &g_population.maximumRadius, 0.0F, 400.0F)
              || changed;
    changed = ImGui::SliderFloat("Forget distance", &g_population.forgetRadius, 20.0F, 1200.0F)
              || changed;
    changed = ImGui::Checkbox("Snap map points to ground", &g_population.snapToGround) || changed;
    changed = ImGui::SliderFloat("Ground lift", &g_population.lift, 0.0F, 5.0F) || changed;
    changed = ImGui::SliderFloat("Entity scale", &g_population.scale, 0.1F, 5.0F) || changed;

    if (g_population.maximumRadius < g_population.minimumRadius) {
        g_population.maximumRadius = g_population.minimumRadius;
        changed = true;
    }
    const float forgetFloor = g_population.maximumRadius * 1.5F;
    if (g_population.forgetRadius < forgetFloor) {
        g_population.forgetRadius = forgetFloor;
        changed = true;
    }

    if (ImGui::Button("Forget tracked population")) {
        native::clear_population_tracking();
    }

    if (changed) {
        (void)client::spawn::publish_population(g_population);
    }

    ImGui::TextDisabled(
        "Population only tracks spawned entities; leaving an area does not delete objects already "
        "placed by the game.");
}

[[nodiscard]] const char* preview(const Column& column) noexcept {
    return column.selected < column.candidates.size()
               ? column.candidates[column.selected].label.data()
               : "[None]";
}

void draw_main_type_filter(spawn_keys::Keybinds& settings, bool& changed) noexcept {
    if (!ImGui::TreeNodeEx("Sort", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }

    bool filterChanged = false;
    ImGui::TextDisabled("Checked types are visible");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1.0F));
    if (ImGui::BeginTable("type_visibility", 4, ImGuiTableFlags_SizingStretchSame)) {
        const auto drawType = [&](ObjectType type) {
            ImGui::TableNextColumn();
            const std::uint64_t bit = object_type_filter_bit(type);
            bool visible = (settings.hiddenMainTypes & bit) == 0;
            if (ImGui::Checkbox(object_type_name(type), &visible)) {
                settings.hiddenMainTypes =
                    visible ? settings.hiddenMainTypes & ~bit : settings.hiddenMainTypes | bit;
                filterChanged = true;
            }
        };
        for (std::size_t index = 0; index < kObjectTypeNames.size(); ++index) {
            drawType(static_cast<ObjectType>(index));
        }
        drawType(ObjectType::Invalid);
        ImGui::TableNextColumn();
        bool unknownVisible = (settings.hiddenMainTypes & kUnknownObjectTypeBit) == 0;
        if (ImGui::Checkbox("Unknown", &unknownVisible)) {
            settings.hiddenMainTypes = unknownVisible
                                           ? settings.hiddenMainTypes & ~kUnknownObjectTypeBit
                                           : settings.hiddenMainTypes | kUnknownObjectTypeBit;
            filterChanged = true;
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::TreePop();

    if (filterChanged) {
        apply_main_type_filter(settings.hiddenMainTypes);
        changed = true;
    }
}

void draw_settings(Column& column, const char* id, SpawnAllMode spawnAllMode) noexcept {
    ImGui::PushID(id);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float controlWidth = (ImGui::GetContentRegionAvail().x - spacing) * 0.5F;

    ImGui::BeginGroup();
    ImGui::TextUnformatted("Amount:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragInt("##amount", &column.amount, 1.0F, 1, 4096, "%d");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Vertical lift:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat("##vertical_lift", &column.settings.lift, 0.1F, -100.0F, 100.0F, "%.1f");
    ImGui::EndGroup();

    ImGui::BeginGroup();
    ImGui::TextUnformatted("Scale:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat("##scale", &column.settings.scale, 0.01F, 0.01F, 100.0F, "%.2f");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Ray distance:");
    ImGui::SetNextItemWidth(controlWidth);
    ImGui::DragFloat("##ray_distance", &column.settings.rayDistance, 1.0F, 1.0F, 2000.0F, "%.0f");
    ImGui::EndGroup();

    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Checkbox("Camera rotation", &column.settings.useCameraRotation);
        ImGui::Checkbox("Override rotation", &column.settings.overrideRotation);
        ImGui::TextUnformatted("Position offset:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat3("##position_offset", column.settings.offset.data(), "%.2f");
        if (column.settings.overrideRotation) {
            ImGui::TextUnformatted("Rotation quaternion:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat4("##rotation_quaternion", column.settings.rotation.data(), "%.3f");
        }
        ImGui::TreePop();
    }

    const bool filterByType = spawnAllMode == SpawnAllMode::selectedType;
    const char* const spawnAllLabel =
        filterByType ? "Spawn All of Type [unstable]" : "Spawn All [unstable]";
    if (spawnAllMode != SpawnAllMode::none
        && ImGui::TreeNodeEx(spawnAllLabel, ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::TextUnformatted("Items per row:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragInt("##items_per_row", &column.perRow, 1.0F, 1, 4096, "%d");
        ImGui::TextUnformatted("Spacing:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##spacing", &column.spacing, 0.1F, 0.1F, 100.0F, "%.1f");
        const bool hasSelection = column.selected < column.candidates.size();
        ImGui::BeginDisabled(native::busy() || (filterByType && !hasSelection));
        if (ImGui::Button(filterByType ? "Spawn selected type at crosshair"
                                       : "Spawn all at crosshair",
                          ImVec2(-FLT_MIN, 0.0F))) {
            std::vector<std::uint32_t> tags{};
            tags.reserve(column.candidates.size());
            const ObjectType selectedType =
                hasSelection ? column.candidates[column.selected].type : ObjectType::Invalid;
            for (const Candidate& candidate : column.candidates) {
                if (!filterByType || candidate.type == selectedType) {
                    tags.push_back(candidate.tag);
                }
            }
            std::sort(tags.begin(), tags.end());
            tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
            (void)native::request_line(tags,
                                       native::Origin::crosshair,
                                       static_cast<std::uint32_t>((std::max)(column.perRow, 1)),
                                       column.spacing,
                                       column.settings);
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_keybinds(spawn_keys::Action playerAction,
                   spawn_keys::Action crosshairAction,
                   spawn_keys::Keybinds& keybinds,
                   bool& changed) noexcept {
    if (!ImGui::TreeNodeEx("Keybinds", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    const float labelWidth =
        ImGui::CalcTextSize("At crosshair").x + ImGui::GetStyle().ItemSpacing.x * 2.0F;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At player");
    ImGui::SameLine(labelWidth);
    changed = key_picker(playerAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(playerAction)],
                         controlWidth)
              || changed;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("At crosshair");
    ImGui::SameLine(labelWidth);
    changed = key_picker(crosshairAction,
                         keybinds.virtualKeys[static_cast<std::size_t>(crosshairAction)],
                         controlWidth)
              || changed;
    ImGui::TreePop();
}

void draw_column(const char* title,
                 const char* id,
                 Column& column,
                 spawn_keys::Action playerAction,
                 spawn_keys::Action crosshairAction,
                 bool showTypeFilter,
                 SpawnAllMode spawnAllMode,
                 spawn_keys::Keybinds& keybinds,
                 bool& keybindsChanged) noexcept {
    ImGui::PushID(id);
    const std::uint32_t selectedTag = column.selected < column.candidates.size()
                                          ? column.candidates[column.selected].tag
                                          : 0xFFFFFFFFU;
    const std::uint32_t amount = static_cast<std::uint32_t>((std::max)(column.amount, 1));
    native::configure_shortcut(playerAction, selectedTag, amount, column.settings);
    native::configure_shortcut(crosshairAction, selectedTag, amount, column.settings);

    if (ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (showTypeFilter) {
            draw_main_type_filter(keybinds, keybindsChanged);
        }
        const std::span<const picker::Item> rows(column.items.data(), column.items.size());
        (void)picker::control("picker", preview(column), rows, column.selected);

        ImGui::BeginDisabled(column.selected >= column.candidates.size() || native::busy());
        if (ImGui::Button("At player", ImVec2(ImGui::GetContentRegionAvail().x * 0.49F, 0.0F))) {
            (void)native::request(column.candidates[column.selected].tag,
                                  native::Origin::player,
                                  static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                                  column.settings);
        }
        ImGui::SameLine();
        if (ImGui::Button("At crosshair", ImVec2(-FLT_MIN, 0.0F))) {
            (void)native::request(column.candidates[column.selected].tag,
                                  native::Origin::crosshair,
                                  static_cast<std::uint32_t>((std::max)(column.amount, 1)),
                                  column.settings);
        }
        ImGui::EndDisabled();
        draw_keybinds(playerAction, crosshairAction, keybinds, keybindsChanged);
        draw_settings(column, "settings", spawnAllMode);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace

void draw() noexcept {
    if (!g_scanned) {
        refresh();
    }

    if (ImGui::BeginTabBar("spawn_tabs")) {
        if (ImGui::BeginTabItem("Spawner")) {
            if (ImGui::Button("Refresh loaded entities")) {
                refresh();
                if (g_populationPrimed && g_populationSource == PopulationSource::visibleMain) {
                    publish_population_source();
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu main  |  %zu projectiles  |  %zu loot",
                                g_main.candidates.size(),
                                g_projectile.candidates.size(),
                                g_loot.candidates.size());
            if (native::busy()) {
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    native::cancel();
                }
            }

            spawn_keys::Keybinds keybinds = spawn_keys::get();
            bool keybindsChanged = false;
            draw_column("Main spawner",
                        "main",
                        g_main,
                        spawn_keys::Action::mainPlayer,
                        spawn_keys::Action::mainCrosshair,
                        true,
                        SpawnAllMode::selectedType,
                        keybinds,
                        keybindsChanged);
            draw_column("Projectile spawner",
                        "projectile",
                        g_projectile,
                        spawn_keys::Action::projectilePlayer,
                        spawn_keys::Action::projectileCrosshair,
                        false,
                        SpawnAllMode::all,
                        keybinds,
                        keybindsChanged);
            draw_column("Loot spawner",
                        "loot",
                        g_loot,
                        spawn_keys::Action::lootPlayer,
                        spawn_keys::Action::lootCrosshair,
                        false,
                        SpawnAllMode::all,
                        keybinds,
                        keybindsChanged);
            if (keybindsChanged) {
                (void)spawn_keys::publish(keybinds);
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("World Population")) {
            draw_population_tab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace sunrise::server::ui::spawn
