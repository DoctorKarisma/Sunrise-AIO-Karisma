#include "spawn_keybind_store.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::spawn {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\spawn_keybinds.json";
constexpr std::size_t kFileCapacity = 512;
constexpr std::uint32_t kMaximumVirtualKey = 254;
constexpr std::array<std::string_view, kActionCount> kNames{
    "main_player",
    "main_crosshair",
    "projectile_player",
    "projectile_crosshair",
    "loot_player",
    "loot_crosshair",
};

SRWLOCK g_lock{SRWLOCK_INIT};
Keybinds g_keybinds{};
core::path::Buffer g_path{};
bool g_pathResolved{};

core::path::Buffer g_mapRoot{};
bool g_mapRootResolved{};
std::array<MapPoint, kMapCapacity> g_map{};
std::size_t g_mapCount{};

[[nodiscard]] bool valid(const Keybinds& keybinds) noexcept {
    for (const std::uint32_t key : keybinds.virtualKeys) {
        if (key > kMaximumVirtualKey) {
            return false;
        }
    }
    return true;
}

void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=spawn_keybinds stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

template <typename Integer>
[[nodiscard]] bool
parse_value(std::string_view document, std::string_view name, Integer& output) noexcept {
    std::array<char, 48> quoted{};
    const int length = std::snprintf(
        quoted.data(), quoted.size(), "\"%.*s\"", static_cast<int>(name.size()), name.data());
    if (length <= 0 || static_cast<std::size_t>(length) >= quoted.size()) {
        return false;
    }
    const std::size_t at =
        document.find(std::string_view(quoted.data(), static_cast<std::size_t>(length)));
    const std::size_t colon = at == std::string_view::npos ? at : document.find(':', at + length);
    if (colon == std::string_view::npos) {
        return false;
    }
    const char* begin = document.data() + colon + 1;
    const char* const end = document.data() + document.size();
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        ++begin;
    }
    const auto parsed = std::from_chars(begin, end, output, 10);
    return parsed.ec == std::errc{};
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

    std::array<char, kFileCapacity> document{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (!readOk || read == 0) {
        return;
    }

    Keybinds parsed{};
    const std::string_view text(document.data(), read);
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        (void)parse_value(text, kNames[index], parsed.virtualKeys[index]);
    }
    (void)parse_value(text, "hidden_main_types", parsed.hiddenMainTypes);

    if (valid(parsed)) {
        g_keybinds = parsed;
    } else {
        report_fail("range");
    }
}

[[nodiscard]] bool store(const Keybinds& keybinds) noexcept {
    if (!g_pathResolved) {
        return false;
    }

    std::array<char, kFileCapacity> document{};
    const int size = std::snprintf(document.data(),
                                   document.size(),
                                   "{\n  \"main_player\": %u,\n  \"main_crosshair\": %u,\n"
                                   "  \"projectile_player\": %u,\n  \"projectile_crosshair\": %u,\n"
                                   "  \"loot_player\": %u,\n  \"loot_crosshair\": %u,\n"
                                   "  \"hidden_main_types\": %llu\n}\n",
                                   static_cast<unsigned>(keybinds.virtualKeys[0]),
                                   static_cast<unsigned>(keybinds.virtualKeys[1]),
                                   static_cast<unsigned>(keybinds.virtualKeys[2]),
                                   static_cast<unsigned>(keybinds.virtualKeys[3]),
                                   static_cast<unsigned>(keybinds.virtualKeys[4]),
                                   static_cast<unsigned>(keybinds.virtualKeys[5]),
                                   static_cast<unsigned long long>(keybinds.hiddenMainTypes));
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

[[nodiscard]] bool map_path(std::string_view destination, core::path::Buffer& output) noexcept {
    if (!g_mapRootResolved || destination.empty()) {
        return false;
    }

    output = g_mapRoot;
    std::array<wchar_t, 96> name{};
    std::size_t length = 0;
    for (const char value : destination) {
        if (length == name.size()) {
            return false;
        }
        const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                          || (value >= '0' && value <= '9') || value == '-' || value == '_';
        name[length++] = safe ? static_cast<wchar_t>(value) : L'_';
    }

    return core::path::append(output, L"\\spawn_map_")
           && core::path::append(output, std::wstring_view(name.data(), length))
           && core::path::append(output, L".txt");
}

[[nodiscard]] bool read_map(const core::path::Buffer& path) noexcept {
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::vector<char> document(kMapCapacity * 64 + 1, '\0');
    DWORD read = 0;
    const bool ok =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (!ok) {
        return false;
    }

    document[read] = '\0';
    g_mapCount = 0;
    const char* cursor = document.data();
    while (*cursor != '\0' && g_mapCount < g_map.size()) {
        unsigned tag = 0;
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
        int consumed = 0;
        if (sscanf_s(cursor, "%x %f %f %f%n", &tag, &x, &y, &z, &consumed) == 4 && consumed > 0) {
            MapPoint point{};
            point.tag = static_cast<std::uint32_t>(tag);
            point.position = {x, y, z};
            g_map[g_mapCount++] = point;
            cursor += consumed;
        } else {
            while (*cursor != '\0' && *cursor != '\n') {
                ++cursor;
            }
        }
        while (*cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
    }
    return true;
}

[[nodiscard]] bool write_map(const core::path::Buffer& path) noexcept {
    std::vector<char> document{};
    document.reserve(g_mapCount * 64 + 64);
    std::array<char, 96> line{};

    for (std::size_t index = 0; index < g_mapCount; ++index) {
        const MapPoint& point = g_map[index];
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "%08X %.3f %.3f %.3f\n",
                                          static_cast<unsigned>(point.tag),
                                          point.position[0],
                                          point.position[1],
                                          point.position[2]);
        if (written <= 0) {
            return false;
        }
        document.insert(document.end(), line.data(), line.data() + written);
    }

    const HANDLE file = CreateFileW(path.chars.data(),
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
        document.empty()
        || (WriteFile(file, document.data(), static_cast<DWORD>(document.size()), &written, nullptr)
                != FALSE
            && written == static_cast<DWORD>(document.size()));
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

} // namespace

void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_keybinds = {};
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    g_mapCount = 0;
    g_mapRootResolved = core::path::artifact_directory(module, g_mapRoot);

    if (g_pathResolved) {
        load();
    } else {
        report_fail("path");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_keybinds = {};
    g_path = {};
    g_pathResolved = false;
    g_mapRoot = {};
    g_mapRootResolved = false;
    g_mapCount = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

Keybinds get() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Keybinds snapshot = g_keybinds;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

bool publish(const Keybinds& keybinds) noexcept {
    if (!valid(keybinds)) {
        return false;
    }

    AcquireSRWLockExclusive(&g_lock);
    g_keybinds = keybinds;
    const bool stored = store(keybinds);
    ReleaseSRWLockExclusive(&g_lock);

    if (!stored) {
        report_fail("write");
    }
    return true;
}

bool load_map(std::string_view destination) noexcept {
    core::path::Buffer path{};
    AcquireSRWLockExclusive(&g_lock);
    g_mapCount = 0;
    const bool loaded = map_path(destination, path) && read_map(path);
    ReleaseSRWLockExclusive(&g_lock);
    return loaded;
}

bool save_map(std::string_view destination) noexcept {
    core::path::Buffer path{};
    AcquireSRWLockExclusive(&g_lock);
    const bool saved = map_path(destination, path) && write_map(path);
    ReleaseSRWLockExclusive(&g_lock);
    if (!saved) {
        report_fail("map_write");
    }
    return saved;
}

void clear_map() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_mapCount = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

bool add_map_point(const MapPoint& point) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool room = g_mapCount < g_map.size();
    if (room) {
        g_map[g_mapCount++] = point;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return room;
}

bool remove_last_map_point() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool held = g_mapCount != 0;
    if (held) {
        --g_mapCount;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return held;
}

std::size_t map_size() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_mapCount;
    ReleaseSRWLockShared(&g_lock);
    return count;
}

std::size_t copy_map(std::span<MapPoint> output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = (std::min)(output.size(), g_mapCount);
    std::copy_n(g_map.begin(), count, output.begin());
    ReleaseSRWLockShared(&g_lock);
    return count;
}

} // namespace sunrise::client::spawn
