#include <algorithm>
#include <array>
#include <imgui.h>
#include <string>
#include <string_view>

#include "../../../../resources/resource.h"
#include "../animation/transition/ui_transition_animation.h"
#include "../components/card/ui_card_component.h"
#include "../components/logo/ui_logo_component.h"
#include "../components/section/ui_section_component.h"
#include "../scaling/dpi/ui_dpi_scaling.h"
#include "navigation/ui_layout_navigation.h"
#include "ui_layout_lifecycle.h"

namespace sunrise::core::ui::layout {
namespace {

/** The authored width leaves room for a narrow menu and a wide settings panel. */
constexpr float kPreferredWindowWidth = 920.0F;
/** The authored height fits a 720p viewport with game space left around it. */
constexpr float kPreferredWindowHeight = 580.0F;
/** A 420-pixel minimum keeps the two columns from overlapping. */
constexpr float kMinimumWindowWidth = 420.0F;
/** A 300-pixel minimum keeps the navigation list and credits footer. */
constexpr float kMinimumWindowHeight = 300.0F;
/** 24 pixels keep the centered surface away from viewport edges. */
constexpr float kViewportMargin = 24.0F;
/** Two margins hold the same space on opposite viewport edges. */
constexpr float kViewportMarginCount = 2.0F;
/** 180 pixels caps the narrow module navigation. */
constexpr float kNavigationWidth = 180.0F;
/** Zero width lets Dear ImGui fill the space left on the current row. */
constexpr float kAutomaticWidth = 0.0F;
/** A half-axis pivot centers the window on both viewport axes. */
constexpr ImVec2 kCenterPivot{0.5F, 0.5F};
/** Bottom-right area reserved for Dear ImGui's resize grip. */
constexpr float kResizeGripExtent = 20.0F;

/**
 * The main surface is draggable and resizable but does not use Dear ImGui's saved settings.
 *
 * The normal fitted menu size is the minimum. Users may enlarge the menu by dragging the
 * bottom-right resize grip.
 */
constexpr ImGuiWindowFlags kMainWindowFlags =
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;

/** One trailing null byte turns a descriptor name into a component label. */
constexpr std::size_t kLabelTerminatorBytes = 1;
/** Fixed animation key. Every visibility-lane user needs its own, so keep these distinct. */
constexpr ImGuiID kSurfaceAnimationId = 1;
/** Response rates for opening and closing, in the same range as the other components. */
constexpr animation::transition::Rates kVisibilityRates{16.0F, 14.0F};
/** A closed surface has finished its transition and draws nothing. */
constexpr float kClosedProgress = 0.0F;
/** The surface grows from this fraction of its size while it opens. */
constexpr float kOpeningScale = 0.96F;
/** Full size, reached when the surface is fully open. */
constexpr float kOpenScale = 1.0F;
/** 34 authored pixels give the title logo presence without crowding the title row. */
constexpr float kTitleLogoExtent = 34.0F;
/** The title is drawn at this multiple of the body text, so it holds the logo's row. */
constexpr float kTitleTextRatio = 1.5F;
/** Half a difference centers one item against a taller one. */
constexpr float kHalfExtent = 2.0F;
/** The surface names the tool with the same wordmark the HUD card carries. */
constexpr char kTitle[] = "SUNRISE";

/**
 * Session-only menu position.
 *
 * This intentionally lives only in memory. It is not written to an ImGui settings file, so the
 * menu starts centered again on a fresh game launch.
 */
ImVec2 g_menuPosition{};
bool g_menuPositionInitialized = false;

/**
 * Session-only menu size.
 *
 * The first opening starts at the normal authored/fitted size. A user resize is remembered while
 * Sunrise remains loaded, but no size is written to disk.
 */
ImVec2 g_menuSize{};
bool g_menuSizeInitialized = false;

/**
 * Copies one display name into null-terminated component storage.
 * @return Fixed label storage, always with a trailing null.
 */
[[nodiscard]] std::array<char, modules::kDisplayNameCapacity + kLabelTerminatorBytes>
component_label(const modules::Descriptor& descriptor) noexcept {
    std::array<char, modules::kDisplayNameCapacity + kLabelTerminatorBytes> label{};
    const std::string_view displayName = descriptor.display_name();
    std::copy(displayName.begin(), displayName.end(), label.begin());
    return label;
}

/**
 * Works out the normal menu size that fits the active viewport.
 *
 * On a large enough viewport this is the authored 920x580 size. On smaller viewports the same
 * existing fallback is retained so Sunrise never demands a window larger than the game surface.
 *
 * @param viewport Active Dear ImGui viewport.
 * @return Main window size, or zero axes when the viewport is not ready.
 */
[[nodiscard]] ImVec2 window_size(const ImGuiViewport& viewport) noexcept {
    if (viewport.Size.x <= 0.0F || viewport.Size.y <= 0.0F) {
        return {};
    }

    const float margin = scaling::dpi::pixels(kViewportMargin);
    const float availableWidth = viewport.Size.x - (margin * kViewportMarginCount);
    const float availableHeight = viewport.Size.y - (margin * kViewportMarginCount);
    const float minimumWidth = scaling::dpi::pixels(kMinimumWindowWidth);
    const float minimumHeight = scaling::dpi::pixels(kMinimumWindowHeight);

    if (availableWidth < minimumWidth || availableHeight < minimumHeight) {
        return {};
    }

    return {(std::min)(scaling::dpi::pixels(kPreferredWindowWidth), availableWidth),
            (std::min)(scaling::dpi::pixels(kPreferredWindowHeight), availableHeight)};
}

/**
 * Returns the largest menu size that still leaves the authored margin around the viewport.
 *
 * @param viewport Active Dear ImGui viewport.
 * @return Maximum resizable dimensions.
 */
[[nodiscard]] ImVec2 maximum_window_size(const ImGuiViewport& viewport) noexcept {
    const float margin = scaling::dpi::pixels(kViewportMargin);

    return {(std::max)(viewport.Size.x - (margin * kViewportMarginCount), 0.0F),
            (std::max)(viewport.Size.y - (margin * kViewportMarginCount), 0.0F)};
}

/**
 * Draws the wide content panel and calls only the selected module's callback.
 * @param selected Descriptor copied from one registry snapshot.
 */
void draw_content(const navigation::Selection& selected) noexcept {
    if (!selected.moduleAvailable) {
        ImGui::TextDisabled("No modules are registered.");
        return;
    }

    const auto displayName = component_label(selected.descriptor);

    components::section::header(displayName.data());

    // One spacing height below the title row, so a module's first line never sits against it.
    ImGui::Dummy({kAutomaticWidth, ImGui::GetStyle().ItemSpacing.y});

    selected.descriptor.frame_callback()();
}

/** Draws the animated logo, then the name and version, on one title row. */
void draw_title() noexcept {
    const float extent = scaling::dpi::pixels(kTitleLogoExtent);
    const bool logoDrawn = components::logo::draw(extent);

    if (logoDrawn) {
        ImGui::SameLine();
    }

    // The size is the authored one, because the style carries the display scale separately.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kTitleTextRatio);

    const float titleHeight = ImGui::GetTextLineHeight();
    const float rowY = ImGui::GetCursorPosY();

    // The title is shorter than the logo, so it sits lower to stay level with it.
    const float titleY =
        logoDrawn ? rowY + ((std::max)(extent - titleHeight, 0.0F) / kHalfExtent) : rowY;

    ImGui::SetCursorPosY(titleY);
    ImGui::TextUnformatted(kTitle);

    ImGui::PopFont();

    ImGui::SameLine();

    // SameLine returns to the row the logo opened, so the version is placed against the title
    // again, centered on it because it stays at body size.
    ImGui::SetCursorPosY(
        titleY + ((std::max)(titleHeight - ImGui::GetTextLineHeight(), 0.0F) / kHalfExtent));

    ImGui::TextDisabled(SUNRISE_VER_STRING);
}

} // namespace

/** Draws the Sunrise surface with session-only draggable positioning and resizing. */
bool render(bool visible) noexcept {
    if (!internal::context_is_current()) {
        return false;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();

    if (viewport == nullptr) {
        return false;
    }

    const ImVec2 minimumSize = window_size(*viewport);

    if (minimumSize.x <= 0.0F || minimumSize.y <= 0.0F) {
        return false;
    }

    const ImVec2 maximumSize = maximum_window_size(*viewport);

    if (maximumSize.x < minimumSize.x || maximumSize.y < minimumSize.y) {
        return false;
    }

    // A new lane starts closed, so the surface animates open the first time it is asked for.
    const float progress = animation::transition::update(kSurfaceAnimationId,
                                                         animation::transition::Lane::visibility,
                                                         visible,
                                                         kVisibilityRates,
                                                         kClosedProgress);

    if (progress <= kClosedProgress) {
        return false;
    }

    const float scale = kOpeningScale + ((kOpenScale - kOpeningScale) * progress);

    /*
     * The first opening starts at the normal menu size. After the user resizes the menu, that
     * larger size remains in memory for later closes/reopens during the same game session.
     */
    if (!g_menuSizeInitialized) {
        g_menuSize = minimumSize;
        g_menuSizeInitialized = true;
    }

    /*
     * A viewport or resolution change can make a previously enlarged menu too large. Clamp the
     * remembered size back into the currently valid range while never allowing it below the
     * normal fitted menu dimensions.
     */
    g_menuSize.x = (std::clamp)(g_menuSize.x, minimumSize.x, maximumSize.x);
    g_menuSize.y = (std::clamp)(g_menuSize.y, minimumSize.y, maximumSize.y);

    /*
     * First opening of the current session:
     * center the menu.
     *
     * After that, g_menuPosition is retained and used when the menu is reopened.
     */
    if (!g_menuPositionInitialized) {
        const ImVec2 scaledSize{g_menuSize.x * scale, g_menuSize.y * scale};

        g_menuPosition = {
            viewport->GetCenter().x - (scaledSize.x * kCenterPivot.x),
            viewport->GetCenter().y - (scaledSize.y * kCenterPivot.y),
        };

        g_menuPositionInitialized = true;
    }

    ImGui::SetNextWindowPos(g_menuPosition, ImGuiCond_Always);

    /*
     * While opening, retain the existing slight size-growth animation. Once fully open, stop
     * forcing the window dimensions so Dear ImGui's normal resize grip can control the size.
     */
    if (progress < kOpenScale) {
        ImGui::SetNextWindowSize({g_menuSize.x * scale, g_menuSize.y * scale}, ImGuiCond_Always);

        ImGui::SetNextWindowSizeConstraints({minimumSize.x * scale, minimumSize.y * scale},
                                            maximumSize);
    } else {
        ImGui::SetNextWindowSizeConstraints(minimumSize, maximumSize);
        ImGui::SetNextWindowSize(g_menuSize, ImGuiCond_FirstUseEver);
    }

    // One style alpha fades the surface and everything drawn inside it together.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);

    const bool submitContents = ImGui::Begin("Sunrise", nullptr, kMainWindowFlags);

    /*
     * Keep the existing whole-window drag behavior, except inside the bottom-right resize area.
     *
     * This allows the menu to remain draggable the same way it was before while preventing the
     * custom movement code from fighting Dear ImGui when the resize grip is being dragged.
     */
    const ImVec2 windowPosition = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    const ImVec2 mousePosition = ImGui::GetMousePos();

    const float resizeGripExtent = scaling::dpi::pixels(kResizeGripExtent);

    const bool mouseInResizeGrip =
        mousePosition.x >= windowPosition.x + windowSize.x - resizeGripExtent
        && mousePosition.y >= windowPosition.y + windowSize.y - resizeGripExtent;

    const bool resizing = mouseInResizeGrip && ImGui::IsMouseDragging(ImGuiMouseButton_Left);

    if (!resizing && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;

        g_menuPosition.x += mouseDelta.x;
        g_menuPosition.y += mouseDelta.y;
    }

    /*
     * Once fully open, remember any dimensions chosen using the resize grip.
     * Opening-animation dimensions are deliberately not recorded.
     */
    if (progress >= kOpenScale) {
        const ImVec2 currentSize = ImGui::GetWindowSize();

        g_menuSize.x = (std::clamp)(currentSize.x, minimumSize.x, maximumSize.x);
        g_menuSize.y = (std::clamp)(currentSize.y, minimumSize.y, maximumSize.y);
    }

    if (submitContents) {
        draw_title();
        ImGui::Separator();

        const StateSnapshot state = snapshot();
        navigation::Selection selected{};

        const float panelHeight = ImGui::GetContentRegionAvail().y;

        {
            const components::card::Scope navigationCard(
                "##navigation_card", ImVec2(scaling::dpi::pixels(kNavigationWidth), panelHeight));

            if (navigationCard.visible()) {
                selected = navigation::draw(state);
            }
        }

        ImGui::SameLine();

        {
            const components::card::Scope contentCard("##content_card",
                                                      ImVec2(kAutomaticWidth, panelHeight));

            if (contentCard.visible()) {
                draw_content(selected);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();

    return true;
}

} // namespace sunrise::core::ui::layout
