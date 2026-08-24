#include "population_settings_store.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::spawn {
namespace {

namespace native = hooks::spawn;

constexpr std::wstring_view kFileSuffix = L"\\population.json";
constexpr std::size_t kFileCapacity = 1024;
constexpr std::size_t kScalarCapacity = 32;

SRWLOCK g_lock{SRWLOCK_INIT};
core::path::Buffer g_path{};
bool g_pathResolved{};

void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=population stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool scalar_for(std::string_view text,
                              std::string_view key,
                              std::string_view& output) noexcept {
    const std::size_t keyAt = text.find(key);
    if (keyAt == std::string_view::npos) {
        return false;
    }

    const std::size_t colon = text.find(':', keyAt + key.size());
    if (colon == std::string_view::npos) {
        return false;
    }

    std::size_t start = colon + 1;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
        ++start;
    }

    std::size_t end = start;
    while (end < text.size() && text[end] != ',' && text[end] != '\n' && text[end] != '}') {
        ++end;
    }

    if (end <= start) {
        return false;
    }

    output = text.substr(start, end - start);
    return true;
}

[[nodiscard]] bool terminated(std::string_view value,
                              std::array<char, kScalarCapacity>& output) noexcept {
    if (value.size() >= output.size()) {
        return false;
    }

    output = {};
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = value[index];
    }
    output[value.size()] = '\0';
    return true;
}

void parse(std::string_view text, native::PopulationSettings& output) noexcept {
    std::string_view scalar;
    std::array<char, kScalarCapacity> buffer{};

    if (scalar_for(text, "\"enabled\"", scalar)) {
        output.enabled = scalar.starts_with("true");
    }
    if (scalar_for(text, "\"auto_on_load\"", scalar)) {
        output.autoOnLoad = scalar.starts_with("true");
    }
    if (scalar_for(text, "\"use_map\"", scalar)) {
        output.useMap = scalar.starts_with("true");
    }
    if (scalar_for(text, "\"snap_to_ground\"", scalar)) {
        output.snapToGround = scalar.starts_with("true");
    }
    if (scalar_for(text, "\"target\"", scalar) && terminated(scalar, buffer)) {
        output.target = static_cast<std::uint32_t>(std::strtoul(buffer.data(), nullptr, 0));
    }
    if (scalar_for(text, "\"interval_ms\"", scalar) && terminated(scalar, buffer)) {
        output.intervalMs = static_cast<std::uint32_t>(std::strtoul(buffer.data(), nullptr, 0));
    }
    if (scalar_for(text, "\"respawn_delay_ms\"", scalar) && terminated(scalar, buffer)) {
        output.respawnDelayMs =
            static_cast<std::uint32_t>(std::strtoul(buffer.data(), nullptr, 0));
    }
    if (scalar_for(text, "\"minimum_radius\"", scalar) && terminated(scalar, buffer)) {
        output.minimumRadius = std::strtof(buffer.data(), nullptr);
    }
    if (scalar_for(text, "\"maximum_radius\"", scalar) && terminated(scalar, buffer)) {
        output.maximumRadius = std::strtof(buffer.data(), nullptr);
    }
    if (scalar_for(text, "\"forget_radius\"", scalar) && terminated(scalar, buffer)) {
        output.forgetRadius = std::strtof(buffer.data(), nullptr);
    }
    if (scalar_for(text, "\"lift\"", scalar) && terminated(scalar, buffer)) {
        output.lift = std::strtof(buffer.data(), nullptr);
    }
    if (scalar_for(text, "\"scale\"", scalar) && terminated(scalar, buffer)) {
        output.scale = std::strtof(buffer.data(), nullptr);
    }
}

void load() noexcept {
    native::PopulationSettings parsed{};
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        native::configure_population(parsed);
        return;
    }

    std::array<char, kFileCapacity> document{};
    DWORD read = 0;
    const bool ok =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (ok) {
        parse(std::string_view(document.data(), read), parsed);
    }
    native::configure_population(parsed);
}

[[nodiscard]] bool store(const native::PopulationSettings& settings) noexcept {
    if (!g_pathResolved) {
        return false;
    }

    std::array<char, kFileCapacity> document{};
    const int size = std::snprintf(document.data(),
                                   document.size(),
                                   "{\n  \"enabled\": %s,\n  \"auto_on_load\": %s,\n"
                                   "  \"use_map\": %s,\n"
                                   "  \"snap_to_ground\": %s,\n  \"target\": %u,\n"
                                   "  \"interval_ms\": %u,\n  \"respawn_delay_ms\": %u,\n"
                                   "  \"minimum_radius\": %.3f,\n  \"maximum_radius\": %.3f,\n"
                                   "  \"forget_radius\": %.3f,\n  \"lift\": %.3f,\n"
                                   "  \"scale\": %.3f\n}\n",
                                   settings.enabled ? "true" : "false",
                                   settings.autoOnLoad ? "true" : "false",
                                   settings.useMap ? "true" : "false",
                                   settings.snapToGround ? "true" : "false",
                                   static_cast<unsigned>(settings.target),
                                   static_cast<unsigned>(settings.intervalMs),
                                   static_cast<unsigned>(settings.respawnDelayMs),
                                   static_cast<double>(settings.minimumRadius),
                                   static_cast<double>(settings.maximumRadius),
                                   static_cast<double>(settings.forgetRadius),
                                   static_cast<double>(settings.lift),
                                   static_cast<double>(settings.scale));

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

} // namespace

void initialize_population(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    if (g_pathResolved) {
        load();
    } else {
        report_fail("path");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown_population() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_path = {};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

bool publish_population(const native::PopulationSettings& settings) noexcept {
    native::configure_population(settings);
    AcquireSRWLockExclusive(&g_lock);
    const bool stored = store(settings);
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return true;
}

} // namespace sunrise::client::spawn
