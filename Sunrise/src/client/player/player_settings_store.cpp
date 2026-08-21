/**
 * The player configuration store. Separate from Core settings because the interface changes these
 * values while the game runs and saves each one at once. Core settings are read once.
 */

#include "player_settings_store.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::player {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\player.json";
constexpr std::size_t kFileCapacity = 256;
constexpr std::size_t kScalarCapacity = 32;

SRWLOCK g_lock{SRWLOCK_INIT};
Settings g_settings{};
core::path::Buffer g_path{};
bool g_pathResolved{};

void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=player stage=store result=fail reason=%s", reason);

    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void boolean_for(std::string_view text, std::string_view key, bool& output) noexcept {
    const std::size_t at = text.find(key);

    if (at == std::string_view::npos) {
        return;
    }

    const std::size_t colon = text.find(':', at + key.size());

    if (colon == std::string_view::npos) {
        return;
    }

    std::size_t begin = colon + 1;

    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }

    output = text.substr(begin).starts_with("true");
}

void float_for(std::string_view text, std::string_view key, float& output) noexcept {
    const std::size_t at = text.find(key);

    if (at == std::string_view::npos) {
        return;
    }

    const std::size_t colon = text.find(':', at + key.size());

    if (colon == std::string_view::npos) {
        return;
    }

    std::size_t begin = colon + 1;

    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }

    std::size_t end = begin;

    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n'
           && text[end] != '\r') {
        ++end;
    }

    const std::string_view scalar = text.substr(begin, end - begin);

    if (scalar.empty() || scalar.size() >= kScalarCapacity) {
        return;
    }

    std::array<char, kScalarCapacity> buffer{};
    std::copy(scalar.begin(), scalar.end(), buffer.begin());

    char* parsedEnd = nullptr;
    const float parsed = std::strtof(buffer.data(), &parsedEnd);

    if (parsedEnd != buffer.data() && std::isfinite(parsed)) {
        output = std::clamp(parsed, kMinimumWorldSpeed, kMaximumWorldSpeed);
    }
}

void parse(std::string_view text, Settings& output) noexcept {
    boolean_for(text, "\"infinite_ammo_enabled\"", output.infiniteAmmoEnabled);
    boolean_for(text, "\"no_turnback_enabled\"", output.noTurnbackEnabled);
    boolean_for(text, "\"godmode_enabled\"", output.godmodeEnabled);
    float_for(text, "\"world_speed\"", output.worldSpeed);
}

[[nodiscard]] bool store(const Settings& settings) noexcept {
    if (!g_pathResolved) {
        return false;
    }

    std::array<char, kFileCapacity> document{};

    const int size = std::snprintf(document.data(),
                                   document.size(),
                                   "{\n"
                                   "  \"infinite_ammo_enabled\": %s,\n"
                                   "  \"no_turnback_enabled\": %s,\n"
                                   "  \"godmode_enabled\": %s,\n"
                                   "  \"world_speed\": %.3f\n"
                                   "}\n",
                                   settings.infiniteAmmoEnabled ? "true" : "false",
                                   settings.noTurnbackEnabled ? "true" : "false",
                                   settings.godmodeEnabled ? "true" : "false",
                                   static_cast<double>(settings.worldSpeed));

    if (size <= 0 || static_cast<std::size_t>(size) >= document.size()) {
        return false;
    }

    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;

    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(size), &written, nullptr) != FALSE
        && written == static_cast<DWORD>(size);

    complete = CloseHandle(file) != FALSE && complete;

    return complete;
}

void load() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    std::array<char, kFileCapacity> buffer{};
    DWORD read = 0;

    const bool readOk =
        ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, nullptr)
        != FALSE;

    (void)CloseHandle(file);

    if (!readOk || read == 0) {
        return;
    }

    parse(std::string_view(buffer.data(), read), g_settings);
}

} // namespace

void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);

    g_settings = Settings{};

    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);

    if (g_pathResolved) {
        load();
    } else {
        report_fail("path");
    }

    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);

    g_settings = Settings{};
    g_path = core::path::Buffer{};
    g_pathResolved = false;

    ReleaseSRWLockExclusive(&g_lock);
}

Settings get() noexcept {
    AcquireSRWLockShared(&g_lock);

    const Settings snapshot = g_settings;

    ReleaseSRWLockShared(&g_lock);

    return snapshot;
}

bool publish(const Settings& settings) noexcept {
    if (!std::isfinite(settings.worldSpeed) || settings.worldSpeed < kMinimumWorldSpeed
        || settings.worldSpeed > kMaximumWorldSpeed) {
        return false;
    }

    AcquireSRWLockExclusive(&g_lock);

    g_settings = settings;
    const bool stored = store(settings);

    ReleaseSRWLockExclusive(&g_lock);

    if (!stored) {
        report_fail("write");
    }

    return stored;
}

} // namespace sunrise::client::player
