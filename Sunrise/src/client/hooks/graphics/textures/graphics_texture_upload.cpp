/**
 * Upload of bundled images into renderer textures. The interface draws them, so this layer owns
 * the D3D11 objects behind every published/lazily returned identifier.
 */

#include "graphics_texture_upload.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <wincodec.h>

#include "../../../../../resources/resource.h"
#include "../renderer/state.h"
#include "core/logging/log.h"
#include "core/ui/textures/ui_texture_slots.h"

namespace sunrise::client::hooks::graphics::textures {
namespace {

/** WIC images used here contain one frame. */
constexpr UINT kFirstFrameIndex = 0;
/** One mip level and one array slice. */
constexpr UINT kSingleLevel = 1;
/** The texture is not multisampled, which is quality level 0 of one sample. */
constexpr UINT kSingleSample = 1;
constexpr UINT kDefaultQuality = 0;
/** A 2D upload has no slice pitch. */
constexpr UINT kNoSlicePitch = 0;

/** Compact item icon pack: magic, count, sorted [hash, absolute offset, byte length] entries. */
constexpr std::array<std::byte, 8> kIconMagic{
    static_cast<std::byte>('S'),
    static_cast<std::byte>('D'),
    static_cast<std::byte>('2'),
    static_cast<std::byte>('I'),
    static_cast<std::byte>('C'),
    static_cast<std::byte>('O'),
    static_cast<std::byte>('N'),
    static_cast<std::byte>('1'),
};
constexpr std::size_t kIconHeaderSize = 12;
constexpr std::size_t kIconEntrySize = 12;
constexpr std::size_t kIconCacheCapacity = 192;

struct ResourceBytes {
    const std::byte* data{};
    std::size_t size{};
};

struct CachedIcon {
    std::uint32_t hash{};
    Uploaded uploaded{};
    std::uint64_t stamp{};
    bool used{};
};

std::array<CachedIcon, kIconCacheCapacity> g_itemIcons{};
std::uint64_t g_iconStamp{};
ResourceBytes g_iconPack{};
bool g_iconPackResolved{};

/** @param object COM object we own. Released and cleared when set. */
template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

/** @param line Whole log line. @return Always false, so callers can return it directly. */
[[nodiscard]] bool fail(std::string_view line) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, line);
    return false;
}

/** Balances one COM initialization on the calling thread. */
class ComScope final {
public:
    ComScope() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    ~ComScope() noexcept {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{};
};

/** @return The module holding Sunrise resources, resolved from this code's own address. */
[[nodiscard]] HMODULE owning_module() noexcept {
    HMODULE module = nullptr;
    (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&owning_module),
                             &module);
    return module;
}

/** Borrows one RCDATA payload from the loaded Sunrise module. */
[[nodiscard]] bool resource_bytes(int identifier, ResourceBytes& output) noexcept {
    const HMODULE module = owning_module();
    const HRSRC resource = module != nullptr
                               ? FindResourceW(module, MAKEINTRESOURCEW(identifier), RT_RCDATA)
                               : nullptr;
    if (resource == nullptr) {
        return false;
    }
    const DWORD resourceSize = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* resourceData = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (resourceSize == 0 || resourceData == nullptr) {
        return false;
    }
    output.data = static_cast<const std::byte*>(resourceData);
    output.size = static_cast<std::size_t>(resourceSize);
    return true;
}

/** Decodes PNG/JPEG/etc bytes into a cached 32bpp RGBA bitmap. */
[[nodiscard]] bool decode_image(IWICImagingFactory* factory,
                                const std::byte* bytes,
                                std::size_t size,
                                IWICBitmap*& output) noexcept {
    if (factory == nullptr || bytes == nullptr || size == 0
        || size > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* converted = nullptr;
    auto* mutableBytes = reinterpret_cast<WICInProcPointer>(const_cast<std::byte*>(bytes));
    const bool decoded =
        SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromMemory(mutableBytes, static_cast<DWORD>(size)))
        && SUCCEEDED(factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        && SUCCEEDED(decoder->GetFrame(kFirstFrameIndex, &frame))
        && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted))
        && SUCCEEDED(factory->CreateBitmapFromSource(converted, WICBitmapCacheOnLoad, &output));
    release_com(converted);
    release_com(frame);
    release_com(decoder);
    release_com(stream);
    return decoded;
}

/** Creates one immutable texture and SRV from a decoded bitmap. */
[[nodiscard]] bool
create_texture(ID3D11Device* device, IWICBitmap* bitmap, Uploaded& output) noexcept {
    if (device == nullptr || bitmap == nullptr) {
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(bitmap->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }
    const WICRect area{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    IWICBitmapLock* lock = nullptr;
    if (FAILED(bitmap->Lock(&area, WICBitmapLockRead, &lock))) {
        return false;
    }

    UINT stride = 0;
    UINT pixelBytes = 0;
    BYTE* pixels = nullptr;
    Uploaded created{};
    bool uploaded = SUCCEEDED(lock->GetStride(&stride))
                    && SUCCEEDED(lock->GetDataPointer(&pixelBytes, &pixels)) && pixels != nullptr;
    if (uploaded) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = kSingleLevel;
        description.ArraySize = kSingleLevel;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = kSingleSample;
        description.SampleDesc.Quality = kDefaultQuality;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{pixels, stride, kNoSlicePitch};
        uploaded =
            SUCCEEDED(device->CreateTexture2D(&description, &initial, &created.texture))
            && SUCCEEDED(device->CreateShaderResourceView(created.texture, nullptr, &created.view));
    }
    release_com(lock);
    if (!uploaded) {
        release_com(created.view);
        release_com(created.texture);
        return false;
    }
    output = created;
    return true;
}

[[nodiscard]] std::uint32_t read_u32_le(const std::byte* bytes) noexcept {
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8U)
           | (static_cast<std::uint32_t>(raw[2]) << 16U)
           | (static_cast<std::uint32_t>(raw[3]) << 24U);
}

/** Resolves the bundled pack once; module resource bytes stay valid for the DLL lifetime. */
[[nodiscard]] const ResourceBytes& icon_pack() noexcept {
    if (!g_iconPackResolved) {
        g_iconPackResolved = true;
        ResourceBytes candidate{};
        if (resource_bytes(IDR_ITEM_ICONS, candidate) && candidate.size >= kIconHeaderSize
            && std::memcmp(candidate.data, kIconMagic.data(), kIconMagic.size()) == 0) {
            g_iconPack = candidate;
        }
    }
    return g_iconPack;
}

/** Binary-searches the packed icon index and borrows encoded image bytes. */
[[nodiscard]] bool
packed_icon(std::uint32_t hash, const std::byte*& bytes, std::size_t& length) noexcept {
    bytes = nullptr;
    length = 0;
    const ResourceBytes& pack = icon_pack();
    if (pack.data == nullptr || pack.size < kIconHeaderSize) {
        return false;
    }
    const std::uint32_t count = read_u32_le(pack.data + 8);
    const std::size_t countSize = static_cast<std::size_t>(count);
    if (countSize > (pack.size - kIconHeaderSize) / kIconEntrySize) {
        return false;
    }
    const std::byte* index = pack.data + kIconHeaderSize;
    std::size_t low = 0;
    std::size_t high = countSize;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const std::byte* entry = index + middle * kIconEntrySize;
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
        const std::size_t size = static_cast<std::size_t>(read_u32_le(entry + 8));
        if (offset > pack.size || size > pack.size - offset || size == 0) {
            return false;
        }
        bytes = pack.data + offset;
        length = size;
        return true;
    }
    return false;
}

void release_uploaded(Uploaded& uploaded) noexcept {
    release_com(uploaded.view);
    release_com(uploaded.texture);
}

[[nodiscard]] CachedIcon* cache_slot_for(std::uint32_t hash) noexcept {
    ++g_iconStamp;
    if (g_iconStamp == 0) {
        g_iconStamp = 1;
    }
    CachedIcon* freeSlot = nullptr;
    CachedIcon* oldest = nullptr;
    for (auto& entry : g_itemIcons) {
        if (entry.used && entry.hash == hash) {
            entry.stamp = g_iconStamp;
            return &entry;
        }
        if (!entry.used && freeSlot == nullptr) {
            freeSlot = &entry;
        }
        if (entry.used && (oldest == nullptr || entry.stamp < oldest->stamp)) {
            oldest = &entry;
        }
    }
    CachedIcon* chosen = freeSlot != nullptr ? freeSlot : oldest;
    if (chosen != nullptr && chosen->used) {
        release_uploaded(chosen->uploaded);
        *chosen = {};
    }
    if (chosen != nullptr) {
        chosen->hash = hash;
        chosen->stamp = g_iconStamp;
    }
    return chosen;
}

} // namespace

/** Decodes the bundled logo sheet and publishes its view to the Core interface. */
bool upload_logo_sheet(ID3D11Device* device, Uploaded& output) noexcept {
    if (device == nullptr) {
        return false;
    }
    ResourceBytes sheet{};
    if (!resource_bytes(IDR_LOGO_SHEET, sheet)) {
        return fail("ev=texture stage=logo_sheet result=fail reason=resource");
    }
    const ComScope com;
    if (!com.usable()) {
        return fail("ev=texture stage=logo_sheet result=fail reason=com");
    }
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return fail("ev=texture stage=logo_sheet result=fail reason=factory");
    }
    IWICBitmap* bitmap = nullptr;
    const bool decoded = decode_image(factory, sheet.data, sheet.size, bitmap);
    release_com(factory);
    if (!decoded) {
        return fail("ev=texture stage=logo_sheet result=fail reason=decode");
    }
    const bool created = create_texture(device, bitmap, output);
    release_com(bitmap);
    if (!created) {
        return fail("ev=texture stage=logo_sheet result=fail reason=texture");
    }
    core::ui::textures::publish(core::ui::textures::Slot::logoSheet,
                                reinterpret_cast<ImTextureID>(output.view));
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=texture stage=logo_sheet result=ok");
    return true;
}

/** Empties the published slot, then releases both objects. */
void release_logo_sheet(Uploaded& uploaded) noexcept {
    if (uploaded.view != nullptr) {
        core::ui::textures::publish(core::ui::textures::Slot::logoSheet, ImTextureID_Invalid);
    }
    release_uploaded(uploaded);
}

/** Lazily uploads one item/plug icon from the packed resource. */
ImTextureID item_icon(std::uint32_t definitionHash) noexcept {
    for (auto& entry : g_itemIcons) {
        if (entry.used && entry.hash == definitionHash && entry.uploaded.view != nullptr) {
            ++g_iconStamp;
            if (g_iconStamp == 0) {
                g_iconStamp = 1;
            }
            entry.stamp = g_iconStamp;
            return reinterpret_cast<ImTextureID>(entry.uploaded.view);
        }
    }

    const std::byte* encoded = nullptr;
    std::size_t encodedSize = 0;
    if (!packed_icon(definitionHash, encoded, encodedSize)) {
        return ImTextureID_Invalid;
    }
    ID3D11Device* const device = renderer::g_resources.device;
    if (device == nullptr) {
        return ImTextureID_Invalid;
    }

    const ComScope com;
    if (!com.usable()) {
        return ImTextureID_Invalid;
    }
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return ImTextureID_Invalid;
    }
    IWICBitmap* bitmap = nullptr;
    const bool decoded = decode_image(factory, encoded, encodedSize, bitmap);
    release_com(factory);
    if (!decoded) {
        release_com(bitmap);
        return ImTextureID_Invalid;
    }
    Uploaded uploaded{};
    const bool created = create_texture(device, bitmap, uploaded);
    release_com(bitmap);
    if (!created) {
        release_uploaded(uploaded);
        return ImTextureID_Invalid;
    }

    CachedIcon* const slot = cache_slot_for(definitionHash);
    if (slot == nullptr) {
        release_uploaded(uploaded);
        return ImTextureID_Invalid;
    }
    slot->uploaded = uploaded;
    slot->used = true;
    return reinterpret_cast<ImTextureID>(slot->uploaded.view);
}

/** Releases the bounded icon cache before D3D11 device teardown. */
void release_item_icons() noexcept {
    for (auto& entry : g_itemIcons) {
        if (entry.used) {
            release_uploaded(entry.uploaded);
        }
        entry = {};
    }
    g_iconStamp = 0;
}

} // namespace sunrise::client::hooks::graphics::textures
