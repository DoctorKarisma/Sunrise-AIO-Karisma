#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../resources/resource.h"
#include "../filesystem/path.h"
#include "../logging/log.h"
#include "settings.h"
#include "settings_upgrade.h"

namespace sunrise::core::settings {
namespace {

/** The JSON settings file is stored directly in the owned Sunrise folder. */
constexpr std::wstring_view kSettingsFileSuffix = L"\\settings.json";

/** Default vendor catalog rules are stored beside settings.json. */
constexpr std::wstring_view kVendorCatalogFileSuffix = L"\\vendor_catalog.txt";

/** Default repeatable-bounty rules are stored beside settings.json. */
constexpr std::wstring_view kVendorBountyRollFileSuffix = L"\\vendor_bounty_roll.txt";

/** Default vendor exchange/recycling rules are stored beside settings.json. */
constexpr std::wstring_view kVendorExchangeFileSuffix = L"\\vendor_exchange.txt";

/** Default vendor placeholder substitutions are stored beside settings.json. */
constexpr std::wstring_view kVendorItemSubstituteFileSuffix = L"\\vendor_item_substitute.txt";

/** An upgraded document is staged under this suffix before it replaces the settings file. */
constexpr std::wstring_view kUpgradeStageSuffix = L".new";

/** Largest settings file accepted into fixed storage. */
constexpr std::size_t kConfigCapacity = 1024 * 1024;

Settings g_settings = defaults();

/**
 * Names the step that ended the load. Settings are read before the log sinks exist, so this line
 * is the only way to report a boot failure here.
 * @param reason Short key naming the step.
 * @return Always false, so callers can return it directly.
 */
[[nodiscard]] bool fail(std::string_view reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings result=fail reason=%.*s",
                                      static_cast<int>(reason.size()),
                                      reason.data());
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/**
 * Reports a file this build did not upgrade, which means a newer build wrote it.
 * @param fileVersion Version read from the file, or zero when the key was missing.
 */
void report_version(std::uint32_t fileVersion) noexcept {
    if (fileVersion == kSettingsVersion) {
        return;
    }

    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings stage=version result=mismatch file=%u build=%u",
                                      static_cast<unsigned>(fileVersion),
                                      static_cast<unsigned>(kSettingsVersion));
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Borrows one bundled RCDATA document out of the loaded Sunrise module.
 *
 * The returned string_view points directly into the loaded DLL resource and therefore remains
 * valid for the lifetime of the module.
 *
 * @param module Loaded DLL holding the resource.
 * @param resourceId Numeric RCDATA identifier from resource.h.
 * @param output Receives the resource bytes, owned by the module.
 * @return True when the resource is present and not empty.
 */
[[nodiscard]] bool
bundled_document(void* module, int resourceId, std::string_view& output) noexcept {
    const HMODULE loadedModule = static_cast<HMODULE>(module);

    const HRSRC resource = FindResourceW(loadedModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resource == nullptr) {
        return false;
    }

    const DWORD size = SizeofResource(loadedModule, resource);
    const HGLOBAL loaded = LoadResource(loadedModule, resource);

    const auto* bytes =
        loaded != nullptr ? static_cast<const char*>(LockResource(loaded)) : nullptr;

    if (size == 0 || bytes == nullptr) {
        return false;
    }

    output = std::string_view(bytes, size);
    return true;
}

/**
 * Borrows the bundled default settings document.
 *
 * Keeping this wrapper preserves the settings upgrade path while the generic resource reader is
 * also used for the vendor rule files.
 *
 * @param module Loaded DLL holding the default JSON resource.
 * @param output Receives the resource bytes, owned by the module.
 * @return True when the resource is present and not empty.
 */
[[nodiscard]] bool bundled_settings_document(void* module, std::string_view& output) noexcept {
    return bundled_document(module, IDR_DEFAULT_SETTINGS, output);
}

/**
 * Writes a bundled resource to a new file.
 *
 * CREATE_NEW deliberately refuses to overwrite an existing file. This is important for the vendor
 * rule files because they are intended to remain user-editable after Sunrise creates the defaults.
 *
 * @param module Loaded DLL holding the resource.
 * @param resourceId Numeric RCDATA identifier.
 * @param destination Null-terminated destination path.
 * @return True when the complete resource was written and the file closed cleanly.
 */
[[nodiscard]] bool
write_bundled_file(void* module, int resourceId, const path::Buffer& destination) noexcept {
    std::string_view document;
    if (!bundled_document(module, resourceId, document)) {
        return false;
    }

    const HANDLE file = CreateFileW(destination.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const auto size = static_cast<DWORD>(document.size());

    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;

    complete = CloseHandle(file) != FALSE && complete;

    if (!complete) {
        // A partially written bundled file must not be mistaken for a valid rule document.
        (void)DeleteFileW(destination.chars.data());
    }

    return complete;
}

/**
 * Copies the bundled default settings. An existing file is never overwritten.
 *
 * @param module Loaded DLL holding the default JSON resource.
 * @param configPath Null-terminated destination path.
 * @return True when every bundled byte is written and the file closes cleanly.
 */
[[nodiscard]] bool write_default(void* module, const path::Buffer& configPath) noexcept {
    return write_bundled_file(module, IDR_DEFAULT_SETTINGS, configPath);
}

/**
 * Creates one bundled vendor rule file when it is absent.
 *
 * Existing rule files are intentionally left untouched so users can customize them. A failure to
 * create a vendor rule is non-fatal to Core settings startup; the vendor subsystem can report or
 * handle its own missing rule data.
 *
 * @param module Loaded DLL holding the bundled rule.
 * @param artifactDirectory Owned Sunrise directory.
 * @param suffix File name beginning with a backslash.
 * @param resourceId Numeric RCDATA identifier.
 */
void ensure_vendor_rule(void* module,
                        const path::Buffer& artifactDirectory,
                        std::wstring_view suffix,
                        int resourceId) noexcept {
    path::Buffer destination = artifactDirectory;
    if (!path::append(destination, suffix)) {
        return;
    }

    const DWORD attributes = GetFileAttributesW(destination.chars.data());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        // Never overwrite an existing rule file.
        return;
    }

    if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
        return;
    }

    (void)write_bundled_file(module, resourceId, destination);
}

/**
 * Ensures the default vendor interaction rule set exists beside settings.json.
 *
 * Each rule is created independently and only when missing. This makes the AIO self-contained
 * while preserving PR #87's editable external rule files.
 *
 * @param module Loaded Sunrise DLL.
 * @param artifactDirectory Owned Sunrise directory.
 */
void ensure_vendor_rules(void* module, const path::Buffer& artifactDirectory) noexcept {
    ensure_vendor_rule(module, artifactDirectory, kVendorCatalogFileSuffix, IDR_VENDOR_CATALOG);

    ensure_vendor_rule(
        module, artifactDirectory, kVendorBountyRollFileSuffix, IDR_VENDOR_BOUNTY_ROLL);

    ensure_vendor_rule(module, artifactDirectory, kVendorExchangeFileSuffix, IDR_VENDOR_EXCHANGE);

    ensure_vendor_rule(
        module, artifactDirectory, kVendorItemSubstituteFileSuffix, IDR_VENDOR_ITEM_SUBSTITUTE);
}

/**
 * Replaces the settings file with an upgraded document.
 * The text is staged beside the file and moved over it, so a failed write cannot leave half a file.
 *
 * @param configPath Null-terminated settings path.
 * @param document Complete upgraded document.
 * @return True when the file now holds the upgraded document.
 */
[[nodiscard]] bool store_upgraded(const path::Buffer& configPath,
                                  std::string_view document) noexcept {
    path::Buffer stagePath = configPath;
    if (!path::append(stagePath, kUpgradeStageSuffix)) {
        return false;
    }

    const HANDLE file = CreateFileW(stagePath.chars.data(),
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
    const auto size = static_cast<DWORD>(document.size());

    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;

    complete = CloseHandle(file) != FALSE && complete;

    complete =
        complete
        && MoveFileExW(stagePath.chars.data(), configPath.chars.data(), MOVEFILE_REPLACE_EXISTING)
               != FALSE;

    if (!complete) {
        (void)DeleteFileW(stagePath.chars.data());
    }

    return complete;
}

/**
 * Reports the outcome of an in-place upgrade of the settings file.
 *
 * @param stored True when the upgraded document replaced the file on disk.
 */
void report_upgrade(bool stored) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings stage=upgrade version=%u stored=%u",
                                      static_cast<unsigned>(kSettingsVersion),
                                      stored ? 1U : 0U);
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Drops a leading UTF-8 byte order mark.
 *
 * Notepad and PowerShell's `Set-Content -Encoding utf8` both write one, and the parser reads it
 * as a stray token. That made an unparsable file, and an unparsable file kills startup before
 * the log opens, so the failure arrives with nothing to read.
 *
 * @param document Whole settings text as read from disk.
 * @return The same text with any BOM removed.
 */
[[nodiscard]] std::string_view without_byte_order_mark(std::string_view document) noexcept {
    // UTF-8 byte order mark. An editor writes it and the parser must not see it.
    constexpr std::string_view kMark = "\xEF\xBB\xBF";
    return document.starts_with(kMark) ? document.substr(kMark.size()) : document;
}

} // namespace

/** Loads the settings file from the owned folder, or creates the default one. */
bool initialize(void* module) noexcept {
    path::Buffer artifactDirectory;
    if (!path::artifact_directory(module, artifactDirectory)) {
        return fail("path");
    }

    // Vendor interaction rules are bundled into the DLL exactly so a normal AIO user does not have
    // to download or manually copy these files. Existing customized files are preserved.
    ensure_vendor_rules(module, artifactDirectory);

    path::Buffer configPath = artifactDirectory;
    if (!path::append(configPath, kSettingsFileSuffix)) {
        return fail("path");
    }

    const HANDLE file = CreateFileW(configPath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);

    HANDLE readableFile = file;

    if (readableFile == INVALID_HANDLE_VALUE) {
        // A missing file is created once; other open failures remain fatal.
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return fail("open");
        }

        if (!write_default(module, configPath)) {
            return fail("write_default");
        }

        readableFile = CreateFileW(configPath.chars.data(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);

        if (readableFile == INVALID_HANDLE_VALUE) {
            return fail("reopen");
        }
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(readableFile, &size) || size.QuadPart <= 0) {
        CloseHandle(readableFile);
        return fail("empty");
    }

    if (static_cast<std::uint64_t>(size.QuadPart) > kConfigCapacity) {
        // Silence here reads exactly like a crash, and the cap is the usual cause.
        CloseHandle(readableFile);
        return fail("too_large");
    }

    // Static because two 1 MiB banks overflow the stack. Settings load once, on one thread.
    static std::array<char, kConfigCapacity> buffer{};

    DWORD read = 0;

    const bool readOk =
        ReadFile(readableFile, buffer.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)
            != FALSE
        && read == size.QuadPart;

    const bool closed = CloseHandle(readableFile) != FALSE;

    if (!readOk || !closed) {
        return fail("read");
    }

    std::string_view document = without_byte_order_mark(std::string_view(buffer.data(), read));

    static std::array<char, kConfigCapacity> upgradedBuffer{};

    const bool upgrading = upgrade::needed(document);

    if (upgrading) {
        std::string_view bundled;
        std::size_t upgraded = 0;

        if (!bundled_settings_document(module, bundled)
            || !upgrade::apply(document, bundled, upgradedBuffer, upgraded)) {
            return fail("upgrade");
        }

        document = std::string_view(upgradedBuffer.data(), upgraded);
    }

    Settings parsed;
    if (!parse(document, parsed)) {
        return fail("parse");
    }

    // The file is replaced only once the upgraded document is known to parse.
    if (upgrading) {
        report_upgrade(store_upgraded(configPath, document));
    }

    report_version(parsed.version);
    g_settings = parsed;
    return true;
}

/** Resets active settings to the fixed defaults. */
void shutdown() noexcept {
    g_settings = defaults();
}

/** @return Active read-only Core settings. */
const Settings& get() noexcept {
    return g_settings;
}

} // namespace sunrise::core::settings
