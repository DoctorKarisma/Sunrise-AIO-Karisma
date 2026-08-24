#include "spawn_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/activity/definition.h"
#include "../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../hooking/detour.h"
#include "../../spawn/spawn_keybind_store.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::spawn {
namespace {

constexpr std::uintptr_t kPlacementInitializeRva = 0x4B25F0;
constexpr std::uintptr_t kDirectInitializeRva = 0x4B2570;
constexpr std::uintptr_t kObjectFactoryRva = 0x56D990;
constexpr std::uintptr_t kObjectTransformRva = 0x559B10;
constexpr std::uintptr_t kTagResolverRva = 0x1258970;
constexpr std::uintptr_t kWorldRaycastRva = 0x128E3D0;
constexpr std::uintptr_t kPlayerComponentUpdateRva = 0xBB0DB0;
constexpr std::uint32_t kInvalidDatum = 0xFFFFFFFFU;
constexpr std::size_t kDefinitionObjectType = 0x96;
constexpr std::uint32_t kMaximumAmount = 4096;
constexpr std::size_t kMaximumLineItems = 262144;
constexpr std::size_t kPlacementHeaderBytes = 0x40;
constexpr std::size_t kPlacementPayloadBytes = 0x800;

constexpr std::uintptr_t kObjectDatumDescriptorRva = 0x1F93420;
constexpr std::size_t kObjectDatumBaseOffset = 0x08;
constexpr std::size_t kObjectDatumStrideOffset = 0x10;
constexpr std::size_t kObjectDatumBytes = 0xE0;
constexpr std::size_t kObjectHandleOffset = 0x0C;
constexpr std::size_t kActivationCapacity = 64;
constexpr std::uint8_t kActivationAttempts = 4;
constexpr std::uint64_t kRequestTimeoutMs = 3000;

using PlacementInitialize = std::uint8_t(__fastcall*)(void*, std::uint32_t);
using ObjectFactory = std::uint32_t*(__fastcall*)(std::uint32_t*, void*);
using ObjectTransform = void(__fastcall*)(void*, const float*);
using TagResolver = const std::byte*(__fastcall*)(std::uint32_t);
using WorldRaycast = bool(__fastcall*)(const float*,
                                       const float*,
                                       const float*,
                                       const float*,
                                       std::int32_t,
                                       std::int32_t,
                                       float,
                                       float*,
                                       float*,
                                       std::int32_t*);
using PlayerComponentUpdate = void(__fastcall*)(void*, void*, void*);

struct alignas(16) PlacementStorage {
    std::array<std::byte, kPlacementHeaderBytes + kPlacementPayloadBytes> bytes{};
};

struct Request {
    std::vector<std::uint32_t> tags{};
    Settings settings{};
    Origin origin{Origin::player};
    std::uint32_t amount{};
    std::uint32_t itemsPerRow{1};
    float spacing{1.0F};
    std::uint64_t lastProgress{};
    std::size_t cursor{};
    bool line{};
};

struct Activation {
    std::uint32_t handle{kInvalidDatum};
    std::array<float, 8> transform{};
    std::uint8_t attempts{};
};

struct Shortcut {
    Settings settings{};
    std::uint32_t tag{kInvalidDatum};
    std::uint32_t amount{};
};

hooking::detour::Handle g_updateHook{};
std::atomic_bool g_installed{};
PlacementInitialize g_initialize{};
PlacementInitialize g_directInitialize{};
ObjectFactory g_factory{};
ObjectTransform g_transform{};
TagResolver g_resolver{};
WorldRaycast g_raycast{};
HMODULE g_gameModule{};

SRWLOCK g_requestLock{SRWLOCK_INIT};
Request g_request{};
SRWLOCK g_activationLock{SRWLOCK_INIT};
std::array<Activation, kActivationCapacity> g_activations{};
std::size_t g_activationCount{};
SRWLOCK g_shortcutLock{SRWLOCK_INIT};
std::array<Shortcut, client::spawn::kActionCount> g_shortcuts{};
std::array<std::atomic_bool, client::spawn::kActionCount> g_shortcutDown{};

/** Placements the populator tracks at once. */
constexpr std::size_t kPopulationCapacity = 256;
/** Recorded points one destination map may hold. */
constexpr std::size_t kPopulationPointCapacity = 2048;
/** Entity tags the populator draws from. */
constexpr std::size_t kPopulationTagCapacity = 512;
constexpr float kGroundProbeUp = 150.0F;
constexpr float kGroundProbeDown = 400.0F;

struct MapSlot {
    std::uint32_t tag{kInvalidDatum};
    std::array<float, 3> position{};
    std::uint32_t handle{kInvalidDatum};
    std::uint64_t readyAt{};
};

struct Tracked {
    std::uint32_t handle{kInvalidDatum};
    std::array<float, 3> origin{};
};

SRWLOCK g_populationLock{SRWLOCK_INIT};
PopulationSettings g_population{};
std::array<std::uint32_t, kPopulationTagCapacity> g_populationTags{};
std::size_t g_populationTagCount{};
std::array<Tracked, kPopulationCapacity> g_tracked{};
std::size_t g_trackedCount{};
std::uint64_t g_nextPlacement{};
std::uint64_t g_randomState{};
std::vector<MapSlot> g_points{};
std::size_t g_mapLive{};
PlacementOutcome g_lastOutcome{PlacementOutcome::idle};
float g_nearestFree{-1.0F};
std::array<float, 3> g_lastPlayer{};
std::array<float, 3> g_lastPlaced{};
float g_lastSnap{};

SRWLOCK g_autoLoadLock{SRWLOCK_INIT};
std::array<char, 64> g_loadedDestination{};
std::size_t g_loadedDestinationLength{};

template <typename T> [[nodiscard]] bool safe_read(const void* source, T& value) noexcept {
    __try {
        std::memcpy(&value, source, sizeof value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = {};
        return false;
    }
}

void reset_storage(PlacementStorage& storage) noexcept {
    storage = {};
    constexpr std::uint64_t zero = 0;
    constexpr std::uint64_t capacity = kPlacementPayloadBytes;
    constexpr std::uint64_t alignment = 0x10;
    std::memcpy(storage.bytes.data(), &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x10, &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x18, &kInvalidDatum, sizeof kInvalidDatum);
    std::memcpy(storage.bytes.data() + 0x20, &capacity, sizeof capacity);
    std::memcpy(storage.bytes.data() + 0x28, &alignment, sizeof alignment);
    std::memcpy(storage.bytes.data() + 0x30, &zero, sizeof zero);
}

[[nodiscard]] void* descriptor_of(PlacementStorage& storage) noexcept {
    std::int64_t relative = 0;
    return safe_read(storage.bytes.data(), relative) && relative != 0
               ? storage.bytes.data() + relative
               : nullptr;
}

[[nodiscard]] std::byte* resolve_object(std::uint32_t handle) noexcept {
    if (handle == kInvalidDatum || g_gameModule == nullptr) {
        return nullptr;
    }
    std::byte* const descriptor =
        reinterpret_cast<std::byte*>(g_gameModule) + kObjectDatumDescriptorRva;
    std::byte* base = nullptr;
    std::uint32_t stride = 0;
    if (!safe_read(descriptor + kObjectDatumBaseOffset, base)
        || !safe_read(descriptor + kObjectDatumStrideOffset, stride) || base == nullptr
        || stride != kObjectDatumBytes) {
        return nullptr;
    }
    std::byte* const object = base + (handle & 0x1FFFU) * stride;
    std::uint32_t live = kInvalidDatum;
    return safe_read(object + kObjectHandleOffset, live) && live == handle ? object : nullptr;
}

[[nodiscard]] bool needs_activation(std::uint32_t tag) noexcept {
    if (g_resolver == nullptr) {
        return false;
    }
    const std::byte* definition = nullptr;
    std::uint8_t type = 0;
    __try {
        definition = g_resolver(tag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (definition == nullptr || !safe_read(definition + kDefinitionObjectType, type)) {
        return false;
    }
    return type == 8 || type == 11 || type == 20 || type == 21;
}

void queue_activation(std::uint32_t handle,
                      const std::array<float, 4>& rotation,
                      const std::array<float, 4>& position) noexcept {
    Activation value{};
    value.handle = handle;
    std::copy(rotation.begin(), rotation.end(), value.transform.begin());
    std::copy(position.begin(), position.end(), value.transform.begin() + 4);
    AcquireSRWLockExclusive(&g_activationLock);
    if (g_activationCount == g_activations.size()) {
        std::move(g_activations.begin() + 1, g_activations.end(), g_activations.begin());
        --g_activationCount;
    }
    g_activations[g_activationCount++] = value;
    ReleaseSRWLockExclusive(&g_activationLock);
}

void service_activations() noexcept {
    if (g_transform == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&g_activationLock);
    std::size_t index = 0;
    while (index < g_activationCount) {
        Activation& value = g_activations[index];
        std::byte* const object = resolve_object(value.handle);
        bool finished = false;
        if (object != nullptr) {
            __try {
                g_transform(object, value.transform.data());
                finished = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (finished || ++value.attempts >= kActivationAttempts) {
            g_activations[index] = g_activations[--g_activationCount];
            continue;
        }
        ++index;
    }
    ReleaseSRWLockExclusive(&g_activationLock);
}

[[nodiscard]] bool camera_rotation(const std::array<float, 3>& forward,
                                   std::array<float, 4>& rotation) noexcept {
    const float length =
        forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2];
    if (!std::isfinite(length) || length <= 1.0e-8F) {
        return false;
    }
    const float inverse = 1.0F / std::sqrt(length);
    const std::array<float, 3> unit{
        forward[0] * inverse, forward[1] * inverse, forward[2] * inverse};
    if (unit[0] <= -0.9999F) {
        rotation = {0.0F, 0.0F, 1.0F, 0.0F};
        return true;
    }
    rotation = {0.0F, -unit[2], unit[1], 1.0F + unit[0]};
    const float quaternionLength = rotation[0] * rotation[0] + rotation[1] * rotation[1]
                                   + rotation[2] * rotation[2] + rotation[3] * rotation[3];
    if (!std::isfinite(quaternionLength) || quaternionLength <= 1.0e-8F) {
        return false;
    }
    const float inverseQuaternion = 1.0F / std::sqrt(quaternionLength);
    for (float& lane : rotation) {
        lane *= inverseQuaternion;
    }
    return true;
}

[[nodiscard]] bool crosshair_hit(float distance,
                                 const std::array<float, 3>& camera,
                                 const std::array<float, 3>& forward,
                                 std::array<float, 3>& output) noexcept {
    if (g_raycast == nullptr || !std::isfinite(distance) || distance <= 0.0F) {
        return false;
    }
    std::array<float, 4> up{0.0F, 0.0F, 1.0F, 0.0F};
    std::array<float, 4> start{camera[0], camera[1], camera[2], 0.0F};
    std::array<float, 4> end{camera[0] + forward[0] * distance,
                             camera[1] + forward[1] * distance,
                             camera[2] + forward[2] * distance,
                             0.0F};
    std::array<float, 4> hit = end;
    float fraction = 1.0F;
    std::int32_t material = -1;
    std::uint32_t controlled = kInvalidDatum;
    (void)teleport::current_controlled_handle(controlled);
    const std::int32_t ignored =
        controlled == kInvalidDatum ? -1 : static_cast<std::int32_t>(controlled);
    bool result = false;
    __try {
        result = g_raycast(up.data(),
                           up.data(),
                           start.data(),
                           end.data(),
                           ignored,
                           ignored,
                           0.0F,
                           &fraction,
                           hit.data(),
                           &material);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }
    output = {hit[0], hit[1], hit[2]};
    return result;
}

[[nodiscard]] std::uint32_t spawn_one(std::uint32_t tag,
                                      const std::array<float, 3>& world,
                                      const std::array<float, 4>& rotation,
                                      float scale) noexcept {
    PlacementStorage storage{};
    reset_storage(storage);
    std::uint32_t result = kInvalidDatum;
    __try {
        bool initialized = g_initialize(storage.bytes.data(), tag) != 0;
        if (!initialized && g_resolver(tag) != nullptr) {
            reset_storage(storage);
            initialized = g_directInitialize(storage.bytes.data(), tag) != 0;
        }
        void* const descriptor = initialized ? descriptor_of(storage) : nullptr;
        if (descriptor == nullptr) {
            return kInvalidDatum;
        }
        const std::array<float, 4> position{world[0], world[1], world[2], scale};
        std::memcpy(static_cast<std::byte*>(descriptor) + 0x10, rotation.data(), sizeof rotation);
        std::memcpy(static_cast<std::byte*>(descriptor) + 0x20, position.data(), sizeof position);
        (void)g_factory(&result, descriptor);
        if (result != kInvalidDatum && needs_activation(tag)) {
            queue_activation(result, rotation, position);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = kInvalidDatum;
    }
    return result;
}

void service_request() noexcept {
    if (!busy()) {
        return;
    }
    std::array<float, 3> camera{};
    std::array<float, 3> forward{};
    if (!teleport::current_camera_pose(camera, forward)) {
        return;
    }

    AcquireSRWLockExclusive(&g_requestLock);
    const std::size_t total = g_request.line ? g_request.tags.size() : g_request.amount;
    if (g_request.tags.empty() || g_request.cursor >= total) {
        g_request = {};
        ReleaseSRWLockExclusive(&g_requestLock);
        return;
    }

    std::array<float, 3> position{};
    bool placed = g_request.origin == Origin::player
                      ? teleport::current_position(position)
                      : crosshair_hit(g_request.settings.rayDistance, camera, forward, position);
    if (!placed) {
        g_request = {};
        ReleaseSRWLockExclusive(&g_requestLock);
        return;
    }

    const Settings settings = g_request.settings;
    const std::size_t cursor = g_request.cursor++;
    g_request.lastProgress = GetTickCount64();
    const std::uint32_t tag = g_request.line ? g_request.tags[cursor] : g_request.tags.front();
    if (g_request.line) {
        const std::uint32_t width = (std::max)(g_request.itemsPerRow, 1U);
        const float horizontalLength = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
        const std::array<float, 3> rowForward =
            horizontalLength > 1.0e-5F ? std::array<float, 3>{forward[0] / horizontalLength,
                                                              forward[1] / horizontalLength,
                                                              0.0F}
                                       : std::array<float, 3>{1.0F, 0.0F, 0.0F};
        const std::array<float, 3> right{-rowForward[1], rowForward[0], 0.0F};
        const float column = static_cast<float>(cursor % width);
        const float row = static_cast<float>(cursor / width);
        position[0] +=
            right[0] * column * g_request.spacing + rowForward[0] * row * g_request.spacing;
        position[1] +=
            right[1] * column * g_request.spacing + rowForward[1] * row * g_request.spacing;
    }
    if (g_request.cursor >= total) {
        g_request = {};
    }
    ReleaseSRWLockExclusive(&g_requestLock);

    position[0] += settings.offset[0];
    position[1] += settings.offset[1];
    position[2] += settings.offset[2] + settings.lift;
    std::array<float, 4> rotation = settings.rotation;
    if (!settings.overrideRotation) {
        rotation = {0.0F, 0.0F, 0.0F, 1.0F};
        if (settings.useCameraRotation) {
            (void)camera_rotation(forward, rotation);
        }
    }
    (void)spawn_one(tag, position, rotation, settings.scale);
}

void poll_shortcuts() noexcept {
    const client::spawn::Keybinds keybinds = client::spawn::get();
    DWORD foregroundProcess = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground != nullptr) {
        (void)GetWindowThreadProcessId(foreground, &foregroundProcess);
    }
    const bool blocked =
        foregroundProcess != GetCurrentProcessId() || core::ui::runtime::snapshot().visible;

    for (std::size_t index = 0; index < keybinds.virtualKeys.size(); ++index) {
        const std::uint32_t key = keybinds.virtualKeys[index];
        const bool down =
            key != client::spawn::kNoKey && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        if (blocked) {
            g_shortcutDown[index].store(down, std::memory_order_relaxed);
            continue;
        }
        if (!down || g_shortcutDown[index].exchange(true, std::memory_order_acq_rel)) {
            if (!down) {
                g_shortcutDown[index].store(false, std::memory_order_relaxed);
            }
            continue;
        }

        Shortcut shortcut{};
        AcquireSRWLockShared(&g_shortcutLock);
        shortcut = g_shortcuts[index];
        ReleaseSRWLockShared(&g_shortcutLock);
        if (shortcut.tag == kInvalidDatum || shortcut.amount == 0 || busy()) {
            continue;
        }
        const Origin origin = (index & 1U) == 0 ? Origin::player : Origin::crosshair;
        (void)request(shortcut.tag, origin, shortcut.amount, shortcut.settings);
    }
}

[[nodiscard]] std::uint64_t next_random() noexcept {
    if (g_randomState == 0) {
        g_randomState = GetTickCount64() | 1ULL;
    }
    g_randomState ^= g_randomState >> 12;
    g_randomState ^= g_randomState << 25;
    g_randomState ^= g_randomState >> 27;
    return g_randomState * 0x2545F4914F6CDD1DULL;
}

[[nodiscard]] float random_unit() noexcept {
    constexpr float kScale = 1.0F / 16777216.0F;
    return static_cast<float>(next_random() >> 40) * kScale;
}

[[nodiscard]] float planar_distance_squared(const std::array<float, 3>& first,
                                            const std::array<float, 3>& second) noexcept {
    const float x = first[0] - second[0];
    const float y = first[1] - second[1];
    return x * x + y * y;
}

[[nodiscard]] bool ground_below(const std::array<float, 3>& candidate,
                                std::array<float, 3>& output) noexcept {
    if (g_raycast == nullptr) {
        return false;
    }

    std::array<float, 4> up{0.0F, 0.0F, 1.0F, 0.0F};
    std::array<float, 4> start{candidate[0], candidate[1], candidate[2] + kGroundProbeUp, 0.0F};
    std::array<float, 4> end{candidate[0], candidate[1], candidate[2] - kGroundProbeDown, 0.0F};
    std::array<float, 4> hit = end;
    float fraction = 1.0F;
    std::int32_t material = -1;
    std::uint32_t controlled = kInvalidDatum;
    (void)teleport::current_controlled_handle(controlled);
    const std::int32_t ignored =
        controlled == kInvalidDatum ? -1 : static_cast<std::int32_t>(controlled);

    bool result = false;
    __try {
        result = g_raycast(up.data(),
                           up.data(),
                           start.data(),
                           end.data(),
                           ignored,
                           ignored,
                           0.0F,
                           &fraction,
                           hit.data(),
                           &material);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }

    if (!result || !std::isfinite(hit[0]) || !std::isfinite(hit[1]) || !std::isfinite(hit[2])) {
        return false;
    }

    output = {hit[0], hit[1], hit[2]};
    return true;
}

void prune_population(const std::array<float, 3>& player, float forgetRadius) noexcept {
    const float limit = forgetRadius * forgetRadius;
    std::size_t index = 0;
    while (index < g_trackedCount) {
        const Tracked& value = g_tracked[index];
        const bool gone = resolve_object(value.handle) == nullptr;
        const bool distant = planar_distance_squared(player, value.origin) > limit;
        if (gone || distant) {
            g_tracked[index] = g_tracked[--g_trackedCount];
            continue;
        }
        ++index;
    }
}

[[nodiscard]] bool place_one(const std::array<float, 3>& player,
                             const PopulationSettings& settings) noexcept {
    if (g_populationTagCount == 0 || g_trackedCount >= g_tracked.size()) {
        return false;
    }

    const std::uint32_t tag =
        g_populationTags[static_cast<std::size_t>(next_random() % g_populationTagCount)];
    if (!is_tag_resident(tag)) {
        return false;
    }

    constexpr float kTwoPi = 6.28318530718F;
    const float angle = random_unit() * kTwoPi;
    const float span = settings.maximumRadius - settings.minimumRadius;
    const float radius = settings.minimumRadius + random_unit() * span;
    const std::array<float, 3> candidate{
        player[0] + std::cos(angle) * radius, player[1] + std::sin(angle) * radius, player[2]};

    std::array<float, 3> ground{};
    if (!ground_below(candidate, ground)) {
        return false;
    }
    ground[2] += settings.lift;

    constexpr std::array<float, 4> upright{0.0F, 0.0F, 0.0F, 1.0F};
    const std::uint32_t handle = spawn_one(tag, ground, upright, settings.scale);
    if (handle == kInvalidDatum) {
        return false;
    }

    g_tracked[g_trackedCount++] = Tracked{handle, ground};
    return true;
}

void prune_map(const std::array<float, 3>& player,
               const PopulationSettings& settings,
               std::uint64_t now) noexcept {
    const float forget = settings.forgetRadius * settings.forgetRadius;
    g_mapLive = 0;

    for (MapSlot& slot : g_points) {
        if (slot.handle == kInvalidDatum) {
            continue;
        }

        if (resolve_object(slot.handle) == nullptr) {
            slot.handle = kInvalidDatum;
            slot.readyAt = now + settings.respawnDelayMs;
            continue;
        }

        if (planar_distance_squared(player, slot.position) > forget) {
            slot.handle = kInvalidDatum;
            continue;
        }

        ++g_mapLive;
    }
}

[[nodiscard]] bool place_from_map(const std::array<float, 3>& player,
                                  const PopulationSettings& settings,
                                  std::uint64_t now) noexcept {
    const float minimum = settings.minimumRadius * settings.minimumRadius;
    const float maximum = settings.maximumRadius * settings.maximumRadius;

    std::size_t chosen = g_points.size();
    std::size_t seen = 0;
    g_nearestFree = -1.0F;

    for (std::size_t index = 0; index < g_points.size(); ++index) {
        const MapSlot& slot = g_points[index];
        if (slot.handle != kInvalidDatum || now < slot.readyAt) {
            continue;
        }

        const float distanceSquared = planar_distance_squared(player, slot.position);
        const float distance = std::sqrt(distanceSquared);
        if (g_nearestFree < 0.0F || distance < g_nearestFree) {
            g_nearestFree = distance;
        }
        if (distanceSquared < minimum || distanceSquared > maximum) {
            continue;
        }

        ++seen;
        if (next_random() % seen == 0) {
            chosen = index;
        }
    }

    if (chosen >= g_points.size()) {
        g_lastOutcome = PlacementOutcome::noneInRange;
        return false;
    }

    MapSlot& slot = g_points[chosen];
    if (!is_tag_resident(slot.tag)) {
        slot.readyAt = now + settings.intervalMs * 8ULL;
        g_lastOutcome = PlacementOutcome::notResident;
        return false;
    }

    std::array<float, 3> position = slot.position;
    g_lastSnap = 0.0F;
    if (settings.snapToGround) {
        std::array<float, 3> ground{};
        if (!ground_below(position, ground)) {
            slot.readyAt = now + settings.intervalMs * 4ULL;
            g_lastOutcome = PlacementOutcome::noGround;
            return false;
        }
        g_lastSnap = ground[2] - position[2];
        position = ground;
    }

    position[2] += settings.lift;
    g_lastPlaced = position;
    constexpr std::array<float, 4> upright{0.0F, 0.0F, 0.0F, 1.0F};
    const std::uint32_t handle = spawn_one(slot.tag, position, upright, settings.scale);
    if (handle == kInvalidDatum) {
        g_lastOutcome = PlacementOutcome::spawnFailed;
        return false;
    }

    slot.handle = handle;
    ++g_mapLive;
    g_lastOutcome = PlacementOutcome::placed;
    return true;
}

void service_auto_load() noexcept {
    const PopulationSettings settings = population();
    if (!settings.autoOnLoad) {
        return;
    }

    const std::uint64_t sessionId =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    if (sessionId == state::activity::kAbsentSessionId) {
        return;
    }

    state::activity::destination::DestinationSelection selection{};
    if (!state::activity::destination::snapshot(sessionId, selection)
        || selection.packageNameLength == 0) {
        return;
    }

    const std::string_view destination(reinterpret_cast<const char*>(selection.packageName.data()),
                                       selection.packageNameLength);

    AcquireSRWLockExclusive(&g_autoLoadLock);
    const std::string_view loaded(g_loadedDestination.data(), g_loadedDestinationLength);
    const bool arrived = destination != loaded && destination.size() <= g_loadedDestination.size();
    if (arrived) {
        g_loadedDestination = {};
        std::copy(destination.begin(), destination.end(), g_loadedDestination.begin());
        g_loadedDestinationLength = destination.size();
    }
    ReleaseSRWLockExclusive(&g_autoLoadLock);

    if (!arrived) {
        return;
    }

    std::vector<PopulationPoint> points{};
    if (client::spawn::load_map(destination)) {
        std::vector<client::spawn::MapPoint> stored(client::spawn::map_size());
        const std::size_t count = client::spawn::copy_map(stored);
        points.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            points.push_back(PopulationPoint{stored[index].tag, stored[index].position});
        }
    }

    set_population_points(points);
    PopulationSettings armed = population();
    armed.useMap = true;
    armed.enabled = !points.empty();
    configure_population(armed);
}

void service_population() noexcept {
    AcquireSRWLockExclusive(&g_populationLock);
    const PopulationSettings settings = g_population;
    if (!settings.enabled) {
        g_lastOutcome = PlacementOutcome::disabled;
        ReleaseSRWLockExclusive(&g_populationLock);
        return;
    }

    const std::uint64_t now = GetTickCount64();
    if (now < g_nextPlacement) {
        ReleaseSRWLockExclusive(&g_populationLock);
        return;
    }
    g_nextPlacement = now + settings.intervalMs;

    std::array<float, 3> player{};
    g_lastOutcome = PlacementOutcome::noPlayer;
    if (teleport::current_position(player)) {
        g_lastPlayer = player;
        if (settings.useMap) {
            prune_map(player, settings, now);
            if (g_points.empty()) {
                g_lastOutcome = PlacementOutcome::noPoints;
            } else if (g_mapLive >= settings.target) {
                g_lastOutcome = PlacementOutcome::atTarget;
            } else {
                (void)place_from_map(player, settings, now);
            }
        } else {
            prune_population(player, settings.forgetRadius);
            if (g_trackedCount >= settings.target) {
                g_lastOutcome = PlacementOutcome::atTarget;
            } else if (g_populationTagCount == 0) {
                g_lastOutcome = PlacementOutcome::noPoints;
            } else {
                g_lastOutcome = place_one(player, settings) ? PlacementOutcome::placed
                                                            : PlacementOutcome::spawnFailed;
            }
        }
    }

    ReleaseSRWLockExclusive(&g_populationLock);
}

void __fastcall player_component_update(void* object, void* input, void* authored) noexcept {
    const auto next = reinterpret_cast<PlayerComponentUpdate>(g_updateHook.original);
    if (next != nullptr) {
        next(object, input, authored);
    }
    if (teleport::is_controlled_object(object)) {
        poll_shortcuts();
        service_activations();
        service_request();
        service_auto_load();
        service_population();
    }
}

[[nodiscard]] bool valid_settings(const Settings& settings) noexcept {
    return std::isfinite(settings.lift) && std::isfinite(settings.rayDistance)
           && settings.rayDistance > 0.0F && std::isfinite(settings.scale) && settings.scale > 0.0F
           && std::all_of(settings.offset.begin(),
                          settings.offset.end(),
                          [](float value) { return std::isfinite(value); })
           && std::all_of(settings.rotation.begin(), settings.rotation.end(), [](float value) {
                  return std::isfinite(value);
              });
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    g_gameModule = GetModuleHandleW(nullptr);
    if (g_gameModule == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=spawn stage=install result=fail reason=target");
        return false;
    }
    std::byte* const base = reinterpret_cast<std::byte*>(g_gameModule);
    g_initialize = reinterpret_cast<PlacementInitialize>(base + kPlacementInitializeRva);
    g_directInitialize = reinterpret_cast<PlacementInitialize>(base + kDirectInitializeRva);
    g_factory = reinterpret_cast<ObjectFactory>(base + kObjectFactoryRva);
    g_transform = reinterpret_cast<ObjectTransform>(base + kObjectTransformRva);
    g_resolver = reinterpret_cast<TagResolver>(base + kTagResolverRva);
    g_raycast = reinterpret_cast<WorldRaycast>(base + kWorldRaycastRva);
    void* const update = base + kPlayerComponentUpdateRva;
    if (!hooking::detour::install({update, reinterpret_cast<void*>(&player_component_update)},
                                  g_updateHook)) {
        uninstall();
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=spawn stage=install result=fail reason=attach");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=spawn stage=install result=ok");
    return true;
}

void uninstall() noexcept {
    g_installed.store(false, std::memory_order_release);
    cancel();
    (void)hooking::detour::uninstall(g_updateHook);
    g_updateHook = {};
    g_initialize = nullptr;
    g_directInitialize = nullptr;
    g_factory = nullptr;
    g_transform = nullptr;
    g_resolver = nullptr;
    g_raycast = nullptr;
    g_gameModule = nullptr;
    AcquireSRWLockExclusive(&g_activationLock);
    g_activationCount = 0;
    ReleaseSRWLockExclusive(&g_activationLock);
    AcquireSRWLockExclusive(&g_populationLock);
    g_population = {};
    g_populationTagCount = 0;
    g_trackedCount = 0;
    g_points.clear();
    g_mapLive = 0;
    g_nextPlacement = 0;
    g_lastOutcome = PlacementOutcome::idle;
    g_nearestFree = -1.0F;
    g_lastPlayer = {};
    g_lastPlaced = {};
    g_lastSnap = 0.0F;
    ReleaseSRWLockExclusive(&g_populationLock);
    AcquireSRWLockExclusive(&g_autoLoadLock);
    g_loadedDestination = {};
    g_loadedDestinationLength = 0;
    ReleaseSRWLockExclusive(&g_autoLoadLock);

    AcquireSRWLockExclusive(&g_shortcutLock);
    g_shortcuts = {};
    ReleaseSRWLockExclusive(&g_shortcutLock);
    for (std::atomic_bool& down : g_shortcutDown) {
        down.store(false, std::memory_order_relaxed);
    }
}

bool ready() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

bool busy() noexcept {
    AcquireSRWLockExclusive(&g_requestLock);
    if (!g_request.tags.empty() && GetTickCount64() - g_request.lastProgress >= kRequestTimeoutMs) {
        g_request = {};
    }
    const bool value = !g_request.tags.empty();
    ReleaseSRWLockExclusive(&g_requestLock);
    return value;
}

bool is_tag_resident(std::uint32_t tag) noexcept {
    if (!ready() || tag == kInvalidDatum || g_resolver == nullptr) {
        return false;
    }
    __try {
        return g_resolver(tag) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool object_type(std::uint32_t tag, std::uint8_t& type) noexcept {
    type = 0;
    if (!is_tag_resident(tag)) {
        return false;
    }
    __try {
        const std::byte* const definition = g_resolver(tag);
        return definition != nullptr && safe_read(definition + kDefinitionObjectType, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool request(std::uint32_t tag,
             Origin origin,
             std::uint32_t amount,
             const Settings& settings) noexcept {
    if (!ready() || !is_tag_resident(tag) || amount == 0 || amount > kMaximumAmount
        || !valid_settings(settings)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_requestLock);
    if (!g_request.tags.empty()) {
        ReleaseSRWLockExclusive(&g_requestLock);
        return false;
    }
    g_request = {};
    g_request.tags.push_back(tag);
    g_request.settings = settings;
    g_request.origin = origin;
    g_request.amount = amount;
    g_request.lastProgress = GetTickCount64();
    ReleaseSRWLockExclusive(&g_requestLock);
    return true;
}

bool request_line(std::span<const std::uint32_t> tags,
                  Origin origin,
                  std::uint32_t itemsPerRow,
                  float spacing,
                  const Settings& settings) noexcept {
    if (!ready() || tags.empty() || tags.size() > kMaximumLineItems || itemsPerRow == 0
        || !std::isfinite(spacing) || spacing <= 0.0F || !valid_settings(settings)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_requestLock);
    if (!g_request.tags.empty()) {
        ReleaseSRWLockExclusive(&g_requestLock);
        return false;
    }
    g_request = {};
    g_request.tags.assign(tags.begin(), tags.end());
    g_request.settings = settings;
    g_request.origin = origin;
    g_request.itemsPerRow = itemsPerRow;
    g_request.spacing = spacing;
    g_request.line = true;
    g_request.lastProgress = GetTickCount64();
    ReleaseSRWLockExclusive(&g_requestLock);
    return true;
}

void configure_shortcut(client::spawn::Action action,
                        std::uint32_t tag,
                        std::uint32_t amount,
                        const Settings& settings) noexcept {
    const std::size_t index = static_cast<std::size_t>(action);
    if (index >= g_shortcuts.size()) {
        return;
    }
    Shortcut shortcut{};
    if (tag != kInvalidDatum && amount > 0 && amount <= kMaximumAmount
        && valid_settings(settings)) {
        shortcut.tag = tag;
        shortcut.amount = amount;
        shortcut.settings = settings;
    }
    AcquireSRWLockExclusive(&g_shortcutLock);
    g_shortcuts[index] = shortcut;
    ReleaseSRWLockExclusive(&g_shortcutLock);
}

void cancel() noexcept {
    AcquireSRWLockExclusive(&g_requestLock);
    g_request = {};
    ReleaseSRWLockExclusive(&g_requestLock);
}

[[nodiscard]] bool valid_population(const PopulationSettings& settings) noexcept {
    return std::isfinite(settings.minimumRadius) && std::isfinite(settings.maximumRadius)
           && std::isfinite(settings.forgetRadius) && std::isfinite(settings.lift)
           && std::isfinite(settings.scale) && settings.minimumRadius >= 0.0F
           && settings.maximumRadius >= settings.minimumRadius && settings.forgetRadius > 0.0F
           && settings.scale > 0.0F && settings.target <= kPopulationCapacity
           && settings.intervalMs > 0;
}

void configure_population(const PopulationSettings& settings) noexcept {
    if (!valid_population(settings)) {
        return;
    }
    AcquireSRWLockExclusive(&g_populationLock);
    g_population = settings;
    ReleaseSRWLockExclusive(&g_populationLock);
}

PopulationSettings population() noexcept {
    AcquireSRWLockShared(&g_populationLock);
    const PopulationSettings result = g_population;
    ReleaseSRWLockShared(&g_populationLock);
    return result;
}

void set_population_tags(std::span<const std::uint32_t> tags) noexcept {
    AcquireSRWLockExclusive(&g_populationLock);
    g_populationTagCount = (std::min)(tags.size(), g_populationTags.size());
    std::copy_n(tags.begin(), g_populationTagCount, g_populationTags.begin());
    ReleaseSRWLockExclusive(&g_populationLock);
}

std::size_t population_live() noexcept {
    AcquireSRWLockShared(&g_populationLock);
    const std::size_t result = g_population.useMap ? g_mapLive : g_trackedCount;
    ReleaseSRWLockShared(&g_populationLock);
    return result;
}

std::size_t population_source_count() noexcept {
    AcquireSRWLockShared(&g_populationLock);
    const std::size_t result = g_populationTagCount;
    ReleaseSRWLockShared(&g_populationLock);
    return result;
}

void set_population_points(std::span<const PopulationPoint> points) noexcept {
    const std::size_t count = (std::min)(points.size(), kPopulationPointCapacity);
    AcquireSRWLockExclusive(&g_populationLock);
    g_points.clear();
    g_points.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        MapSlot slot{};
        slot.tag = points[index].tag;
        slot.position = points[index].position;
        g_points.push_back(slot);
    }
    g_mapLive = 0;
    g_nearestFree = -1.0F;
    ReleaseSRWLockExclusive(&g_populationLock);
}

std::size_t population_point_count() noexcept {
    AcquireSRWLockShared(&g_populationLock);
    const std::size_t result = g_points.size();
    ReleaseSRWLockShared(&g_populationLock);
    return result;
}

PopulationStatus population_status() noexcept {
    AcquireSRWLockShared(&g_populationLock);
    PopulationStatus status{};
    status.points = g_points.size();
    status.live = g_mapLive;
    status.nearest = g_nearestFree;
    status.last = g_lastOutcome;
    status.player = g_lastPlayer;
    status.placed = g_lastPlaced;
    status.snapped = g_lastSnap;
    ReleaseSRWLockShared(&g_populationLock);
    return status;
}

void clear_population_tracking() noexcept {
    AcquireSRWLockExclusive(&g_populationLock);
    g_trackedCount = 0;
    for (MapSlot& slot : g_points) {
        slot.handle = kInvalidDatum;
        slot.readyAt = 0;
    }
    g_mapLive = 0;
    ReleaseSRWLockExclusive(&g_populationLock);
}

} // namespace sunrise::client::hooks::spawn
