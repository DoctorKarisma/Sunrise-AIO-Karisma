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

/** Width of the custom right-edge resize zone. */
constexpr float kResizeEdgeExtent = 12.0F;
/** Height of the custom bottom-edge resize zone. */
constexpr float kResizeBottomExtent = 12.0F;
/** Size of the visible bottom-right resize grip. */
constexpr float kResizeGripVisualExtent = 14.0F;

/**
 * Dear ImGui's built-in resize behavior is disabled.
 *
 * Sunrise handles resizing itself so only the right edge, bottom edge and bottom-right corner
 * can resize the window. The top and left edges remain ordinary drag areas.
 */
constexpr ImGuiWindowFlags kMainWindowFlags =
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
    | ImGuiWindowFlags_NoTitleBar;

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

/** Session-only menu position. */
ImVec2 g_menuPosition{};
bool g_menuPositionInitialized = false;

/** Session-only menu size. */
ImVec2 g_menuSize{};
bool g_menuSizeInitialized = false;

/** True while the right edge is actively being dragged. */
bool g_resizingRight = false;
/** True while the bottom edge is actively being dragged. */
bool g_resizingBottom = false;

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

/** Returns the largest menu size that still leaves the authored viewport margin. */
[[nodiscard]] ImVec2 maximum_window_size(const ImGuiViewport& viewport) noexcept {
    const float margin = scaling::dpi::pixels(kViewportMargin);

    return {(std::max)(viewport.Size.x - (margin * kViewportMarginCount), 0.0F),
            (std::max)(viewport.Size.y - (margin * kViewportMarginCount), 0.0F)};
}

/** Draws the visible bottom-right resize grip using the active ImGui theme color. */
void draw_resize_grip() noexcept {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    if (drawList == nullptr) {
        return;
    }

    const ImVec2 windowPosition = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    const float extent = scaling::dpi::pixels(kResizeGripVisualExtent);

    const ImVec2 bottomRight{
        windowPosition.x + windowSize.x,
        windowPosition.y + windowSize.y,
    };

    const ImU32 color = ImGui::GetColorU32(ImGuiCol_ResizeGrip);

    drawList->AddTriangleFilled(bottomRight,
                                ImVec2(bottomRight.x - extent, bottomRight.y),
                                ImVec2(bottomRight.x, bottomRight.y - extent),
                                color);
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

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kTitleTextRatio);

    const float titleHeight = ImGui::GetTextLineHeight();
    const float rowY = ImGui::GetCursorPosY();

    const float titleY =
        logoDrawn ? rowY + ((std::max)(extent - titleHeight, 0.0F) / kHalfExtent) : rowY;

    ImGui::SetCursorPosY(titleY);
    ImGui::TextUnformatted(kTitle);

    ImGui::PopFont();

    ImGui::SameLine();

    ImGui::SetCursorPosY(
        titleY + ((std::max)(titleHeight - ImGui::GetTextLineHeight(), 0.0F) / kHalfExtent));

    ImGui::TextDisabled(SUNRISE_VER_STRING);
}

} // namespace

/** Draws the Sunrise surface with session-only draggable positioning and custom resizing. */
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

    const float progress = animation::transition::update(kSurfaceAnimationId,
                                                         animation::transition::Lane::visibility,
                                                         visible,
                                                         kVisibilityRates,
                                                         kClosedProgress);

    if (progress <= kClosedProgress) {
        return false;
    }

    const float scale = kOpeningScale + ((kOpenScale - kOpeningScale) * progress);

    if (!g_menuSizeInitialized) {
        g_menuSize = minimumSize;
        g_menuSizeInitialized = true;
    }

    g_menuSize.x = (std::clamp)(g_menuSize.x, minimumSize.x, maximumSize.x);
    g_menuSize.y = (std::clamp)(g_menuSize.y, minimumSize.y, maximumSize.y);

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
     * Dear ImGui resizing is disabled, so Sunrise always owns the window size.
     * The opening animation still applies the existing 0.96 -> 1.00 scale.
     */
    ImGui::SetNextWindowSize({g_menuSize.x * scale, g_menuSize.y * scale}, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);

    const bool submitContents = ImGui::Begin("Sunrise", nullptr, kMainWindowFlags);

    draw_resize_grip();

    const ImVec2 windowPosition = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    const ImVec2 mousePosition = ImGui::GetMousePos();
    const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;

    const float rightExtent = scaling::dpi::pixels(kResizeEdgeExtent);
    const float bottomExtent = scaling::dpi::pixels(kResizeBottomExtent);

    const float rightEdge = windowPosition.x + windowSize.x;
    const float bottomEdge = windowPosition.y + windowSize.y;

    const bool mouseNearRight =
        mousePosition.x >= rightEdge - rightExtent && mousePosition.x <= rightEdge + rightExtent
        && mousePosition.y >= windowPosition.y && mousePosition.y <= bottomEdge;

    const bool mouseNearBottom =
        mousePosition.y >= bottomEdge - bottomExtent && mousePosition.y <= bottomEdge + bottomExtent
        && mousePosition.x >= windowPosition.x && mousePosition.x <= rightEdge;

    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool leftClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    /*
     * A resize operation begins only from the right edge, bottom edge, or bottom-right corner.
     * Top and left edges therefore remain ordinary menu drag areas.
     */
    if (leftClicked) {
        g_resizingRight = mouseNearRight;
        g_resizingBottom = mouseNearBottom;
    }

    if (!leftDown) {
        g_resizingRight = false;
        g_resizingBottom = false;
    }

    const bool resizing = g_resizingRight || g_resizingBottom;

    /*
     * Resize width from the right edge only. The window's left edge never moves.
     */
    if (g_resizingRight && leftDown && progress >= kOpenScale) {
        g_menuSize.x = (std::clamp)(g_menuSize.x + mouseDelta.x, minimumSize.x, maximumSize.x);
    }

    /*
     * Resize height from the bottom edge only. The window's top edge never moves.
     */
    if (g_resizingBottom && leftDown && progress >= kOpenScale) {
        g_menuSize.y = (std::clamp)(g_menuSize.y + mouseDelta.y, minimumSize.y, maximumSize.y);
    }

    /*
     * Preserve the existing whole-window drag behavior whenever the user is not resizing.
     *
     * This includes the top and left edges by design.
     */
    if (!resizing && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        g_menuPosition.x += mouseDelta.x;
        g_menuPosition.y += mouseDelta.y;
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
