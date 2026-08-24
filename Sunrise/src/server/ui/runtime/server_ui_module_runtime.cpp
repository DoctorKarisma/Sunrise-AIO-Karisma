#include "server_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../activity_override/activity_override_panel.h"
#include "../spawn/spawn_panel.h"
#include "../weapon_editor/weapon_editor_panel.h"

namespace sunrise::server::ui::runtime {
namespace {

/** A namespaced stable ID keeps Server modules from clashing with Client modules. */
constexpr std::string_view kOverrideStableId = "server.activity_override";
/** Short menu label for the activity override page. */
constexpr std::string_view kOverrideDisplayName = "Activity";

constexpr std::string_view kSpawnStableId = "server.spawn";
constexpr std::string_view kSpawnDisplayName = "Spawn";

constexpr std::string_view kWeaponEditorStableId = "server.weapon_editor";
constexpr std::string_view kWeaponEditorDisplayName = "Gear Editor";

core::ui::modules::registry::PageRegistration g_overridePage;
core::ui::modules::registry::PageRegistration g_spawnPage;
core::ui::modules::registry::PageRegistration g_weaponEditorPage;

} // namespace

/** @return True when the Server module owns its Core UI registry slot. */
bool initialize() noexcept {
    if (!g_overridePage.acquire(core::ui::modules::Owner::server,
                                kOverrideStableId,
                                kOverrideDisplayName,
                                &activity_override::draw)) {
        return false;
    }

    if (!g_spawnPage.acquire(
            core::ui::modules::Owner::server, kSpawnStableId, kSpawnDisplayName, &spawn::draw)) {
        g_overridePage.release();
        return false;
    }

    if (!g_weaponEditorPage.acquire(core::ui::modules::Owner::server,
                                    kWeaponEditorStableId,
                                    kWeaponEditorDisplayName,
                                    &weapon_editor::draw)) {
        g_spawnPage.release();
        g_overridePage.release();
        return false;
    }

    return true;
}

/** Removes the Server module from the Core UI registry. */
void shutdown() noexcept {
    g_weaponEditorPage.release();
    g_spawnPage.release();
    g_overridePage.release();
}

} // namespace sunrise::server::ui::runtime
