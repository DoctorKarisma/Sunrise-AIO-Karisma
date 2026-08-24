#include "ui_toast_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <imgui.h>

#include "../scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::core::ui::toast {
namespace {

constexpr std::size_t kMaxToasts = 6;
constexpr std::size_t kMessageCapacity = 128;
constexpr float kMargin = 20.0F;
constexpr float kGap = 8.0F;
constexpr float kMinWidth = 220.0F;
constexpr std::uint64_t kFadeDurationMs = 300;

struct ToastEntry {
    std::array<char, kMessageCapacity> message{};
    std::size_t messageLength{};
    Type type{Type::info};
    std::uint64_t startTick{};
    std::uint64_t durationMs{2500};
    bool active{false};
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<ToastEntry, kMaxToasts> g_toasts{};

ImVec4 color_for_type(Type type) noexcept {
    switch (type) {
    case Type::success:
        return ImVec4(0.20F, 0.85F, 0.40F, 1.0F);
    case Type::warning:
        return ImVec4(1.00F, 0.75F, 0.15F, 1.0F);
    case Type::error:
        return ImVec4(0.95F, 0.25F, 0.25F, 1.0F);
    case Type::info:
    default:
        return ImVec4(0.30F, 0.70F, 1.00F, 1.0F);
    }
}

const char* icon_for_type(Type type) noexcept {
    switch (type) {
    case Type::success:
        return "[+]";
    case Type::warning:
        return "[!]";
    case Type::error:
        return "[x]";
    case Type::info:
    default:
        return "[i]";
    }
}

} // namespace

void post(std::string_view message, Type type, std::uint32_t durationMs) noexcept {
    if (message.empty()) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t now = GetTickCount64();

    // Find first inactive slot, or replace the oldest
    std::size_t targetIndex = 0;
    std::uint64_t oldestTick = UINT64_MAX;
    for (std::size_t i = 0; i < kMaxToasts; ++i) {
        if (!g_toasts[i].active) {
            targetIndex = i;
            break;
        }
        if (g_toasts[i].startTick < oldestTick) {
            oldestTick = g_toasts[i].startTick;
            targetIndex = i;
        }
    }

    ToastEntry& entry = g_toasts[targetIndex];
    entry.message = {};
    const std::size_t copyLen = (std::min)(message.size(), entry.message.size() - 1);
    std::copy_n(message.data(), copyLen, entry.message.begin());
    entry.messageLength = copyLen;
    entry.type = type;
    entry.startTick = now;
    entry.durationMs = durationMs;
    entry.active = true;

    ReleaseSRWLockExclusive(&g_lock);
}

void post(Type type, std::string_view message, std::uint32_t durationMs) noexcept {
    post(message, type, durationMs);
}

void post(Type type, std::string_view title, std::string_view message, std::uint32_t durationMs) noexcept {
    if (title.empty()) {
        post(message, type, durationMs);
        return;
    }
    std::array<char, kMessageCapacity> fullMsg{};
    (void)std::snprintf(fullMsg.data(), fullMsg.size(), "%.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
    post(std::string_view(fullMsg.data()), type, durationMs);
}

void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (auto& t : g_toasts) {
        t = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool draw() noexcept {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0F || viewport->Size.y <= 0.0F) {
        return false;
    }

    AcquireSRWLockShared(&g_lock);
    const std::uint64_t now = GetTickCount64();
    std::array<ToastEntry, kMaxToasts> copy = g_toasts;
    ReleaseSRWLockShared(&g_lock);

    const float margin = scaling::dpi::pixels(kMargin);
    const float gap = scaling::dpi::pixels(kGap);
    const float minW = scaling::dpi::pixels(kMinWidth);

    float currentBottomY = viewport->WorkPos.y + viewport->WorkSize.y - margin;
    bool anyDrawn = false;
    bool anyExpired = false;

    // Draw from newest at bottom to older above
    for (std::size_t i = 0; i < kMaxToasts; ++i) {
        const ToastEntry& toast = copy[i];
        if (!toast.active) {
            continue;
        }

        const std::uint64_t elapsed = now - toast.startTick;
        const std::uint64_t totalLifetime = toast.durationMs + kFadeDurationMs;

        if (elapsed >= totalLifetime) {
            anyExpired = true;
            continue;
        }

        // Calculate alpha
        float alpha = 1.0F;
        if (elapsed < kFadeDurationMs) {
            alpha = static_cast<float>(elapsed) / static_cast<float>(kFadeDurationMs);
        } else if (elapsed > toast.durationMs) {
            const std::uint64_t fadeOutElapsed = elapsed - toast.durationMs;
            alpha = 1.0F - (static_cast<float>(fadeOutElapsed) / static_cast<float>(kFadeDurationMs));
        }
        alpha = std::clamp(alpha, 0.0F, 1.0F);

        char windowId[32]{};
        (void)std::snprintf(windowId, sizeof(windowId), "##toast_%zu", i);

        ImGui::SetNextWindowBgAlpha(0.88F * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_Border, color_for_type(toast.type));

        constexpr ImGuiWindowFlags kToastFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;

        // Measure or estimate width
        const ImVec2 textSize = ImGui::CalcTextSize(toast.message.data());
        const float boxWidth = (std::max)(minW, textSize.x + scaling::dpi::pixels(50.0F));
        const float targetX = viewport->WorkPos.x + viewport->WorkSize.x - margin - boxWidth;

        // Position window anchored at bottom-right
        ImGui::SetNextWindowPos(ImVec2(targetX, currentBottomY), ImGuiCond_Always, ImVec2(0.0F, 1.0F));

        if (ImGui::Begin(windowId, nullptr, kToastFlags)) {
            ImGui::TextColored(color_for_type(toast.type), "%s", icon_for_type(toast.type));
            ImGui::SameLine();
            ImGui::TextUnformatted(toast.message.data());
            const float windowHeight = ImGui::GetWindowSize().y;
            currentBottomY -= (windowHeight + gap);
            anyDrawn = true;
        }
        ImGui::End();

        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    if (anyExpired) {
        AcquireSRWLockExclusive(&g_lock);
        const std::uint64_t currentNow = GetTickCount64();
        for (auto& t : g_toasts) {
            if (t.active && (currentNow - t.startTick >= t.durationMs + kFadeDurationMs)) {
                t = {};
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    return anyDrawn;
}

} // namespace sunrise::core::ui::toast
