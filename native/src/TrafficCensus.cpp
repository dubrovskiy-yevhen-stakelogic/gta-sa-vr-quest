#include "TrafficCensus.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>

namespace savr::traffic_census {

bool IsHelicopterModel(int modelId) {
    switch (modelId) {
        case 417:  // Leviathan
        case 425:  // Hunter
        case 447:  // Seasparrow
        case 465:  // RC Raider
        case 469:  // Sparrow
        case 487:  // Maverick
        case 488:  // News Chopper
        case 497:  // Police Maverick
        case 501:  // RC Goblin
        case 548:  // Cargobob
        case 563:  // Raindance
            return true;
        default:
            return false;
    }
}

bool IsAircraftModel(int modelId) {
    if (IsHelicopterModel(modelId)) return true;
    switch (modelId) {
        case 460:  // Skimmer
        case 464:  // RC Baron
        case 476:  // Rustler
        case 511:  // Beagle
        case 512:  // Cropduster
        case 513:  // Stuntplane
        case 519:  // Shamal
        case 520:  // Hydra
        case 553:  // Nevada
        case 577:  // AT-400
        case 592:  // Andromada
        case 593:  // Dodo
            return true;
        default:
            return false;
    }
}

bool IsRoadVehicleModel(int modelId) {
    if (modelId < 400 || modelId > 611 || IsAircraftModel(modelId)) {
        return false;
    }
    switch (modelId) {
        // Boats / hovercraft.
        case 430: case 446: case 452: case 453: case 454:
        case 472: case 473: case 484: case 493: case 539: case 595:
        // Rail vehicles.
        case 449: case 537: case 538: case 569: case 570: case 590:
            return false;
        default:
            return true;
    }
}

bool IsPoliceRoadVehicleModel(int modelId) {
    // Models selected by SA's police/wanted vehicle chooser. Keep this list
    // exact instead of treating every emergency-looking model as pursuit
    // traffic; the census uses it only to expose response-model lifetimes.
    switch (modelId) {
        case 427:  // Enforcer
        case 432:  // Rhino
        case 433:  // Barracks
        case 490:  // FBI Rancher
        case 523:  // HPV1000 / police bike
        case 528:  // FBI Truck
        case 596:  // LSPD
        case 597:  // SFPD
        case 598:  // LVPD
        case 599:  // Police Ranger
        case 601:  // S.W.A.T.
            return true;
        default:
            return false;
    }
}

namespace {

// All values below are hard fingerprints of retail Android 2.11.311 arm64.
constexpr std::uintptr_t kVehiclePoolGlobalOffset = 0xA019D0U;
constexpr std::size_t kPoolCapacity = 110U;
constexpr std::size_t kVehicleStride = 0xC68U;
constexpr std::size_t kVehicleCreatedByOffset = 0x610U;
constexpr std::size_t kVehicleLawFlagsOffset = 0x568U;
constexpr std::size_t kVehicleFadeFlagsOffset = 0x56AU;
constexpr std::size_t kVehicleDriverOffset = 0x5A0U;
constexpr std::size_t kVehicleNeededUntilOffset = 0x658U;
constexpr std::size_t kEntityModelIdOffset = 0x32U;
constexpr std::size_t kEntityTypeStatusOffset = 0x5AU;
constexpr std::size_t kEntityMatrixOffset = 0x18U;
constexpr std::size_t kEntityPlacementPositionOffset = 0x08U;
constexpr std::size_t kMatrixPositionOffset = 0x30U;
constexpr std::size_t kPhysicalMoveSpeedOffset = 0x68U;
constexpr std::size_t kPoolObjectsOffset = 0x00U;
constexpr std::size_t kPoolSlotFlagsOffset = 0x08U;
constexpr std::size_t kPoolSizeOffset = 0x10U;
constexpr std::size_t kPoolOwnsAllocationsOffset = 0x18U;
constexpr std::uint8_t kPoolSlotFreeBit = 0x80U;
constexpr std::uint8_t kPoolGenerationMask = 0x7FU;
constexpr std::uint8_t kLawEnforcerBit = 0x01U;
constexpr std::uint8_t kFadeOutBit = 0x04U;
constexpr std::uint8_t kEntityStatusShift = 3U;
constexpr std::uint8_t kStatusSimple = 2U;
constexpr std::uint8_t kStatusPhysics = 3U;
constexpr std::uint8_t kStatusAbandoned = 4U;
constexpr std::uint8_t kStatusWrecked = 5U;
constexpr std::uint64_t kSamplePeriodFrames = 16U;
constexpr float kMovingSpeedSq = 0.02F * 0.02F;
constexpr std::uint32_t kCleanupDebrisBudget = 8U;
constexpr std::uint32_t kCleanupMinimumTerminalAgeMs = 5000U;
constexpr std::uint32_t kCleanupRecentRenderGraceMs = 3000U;
constexpr std::uint32_t kStockRenderHeartbeatMs = 3000U;
constexpr std::uint32_t kCleanupGuardRetryMs = 2000U;
constexpr std::uint32_t kCleanupStaleRetryMs = 500U;
// Vice City VR keeps only the innermost 15 m unconditionally. Beyond this
// radius a terminal response vehicle must be outside the physical HMD field
// and past both render-history grace periods before it can be recycled.
constexpr float kCleanupNearRadiusM = 15.0F;
constexpr float kCleanupRetiredHelicopterRadiusM = 170.0F;
constexpr float kCleanupConeCosHalfAngle = 0.5F;
constexpr float kRenderWitnessConeCosHalfAngle = 0.34202015F;  // cos(70 deg)

constexpr std::uint8_t kCreatedByRandom = 1U;
constexpr std::uint8_t kCreatedByMission = 2U;
constexpr std::uint8_t kCreatedByParked = 3U;
constexpr std::uint8_t kCreatedByPermanent = 4U;

constexpr std::size_t kLastVehicleReadOffset =
    (kPoolCapacity - 1U) * kVehicleStride + kVehicleCreatedByOffset;

static_assert(kVehicleLawFlagsOffset < kVehicleStride);
static_assert(kVehicleFadeFlagsOffset < kVehicleStride);
static_assert(kVehicleDriverOffset < kVehicleStride);
static_assert(kPhysicalMoveSpeedOffset + sizeof(float) * 3U <= kVehicleStride);
static_assert(kVehicleCreatedByOffset < kVehicleStride);
static_assert(kPoolCapacity == 110U);

struct PoolView {
    const std::byte* objects{};
    const std::uint8_t* slotFlags{};
    std::int32_t size{};
    std::uint8_t ownsAllocations{};
};

std::mutex g_mutex;
void** g_poolGlobal{};  // Read only after Configure; the game remains its owner.
bool g_configured{};
bool g_cadenceArmed{};
std::uint64_t g_lastSampleFrame{};

const void* g_poolIdentity{};
const std::byte* g_objectsIdentity{};
const std::uint8_t* g_slotFlagsIdentity{};
bool g_hasBaseline{};
std::array<std::uint8_t, kPoolCapacity> g_previousSlotFlags{};
struct LifecycleSlotState {
    std::uint8_t generation{};
    bool generationValid{};
    // Response identity outlives the mutable law/created-by axes but never the
    // exact generation represented by this state record.
    bool responseProvenance{};
    bool retiredHelicopter{};
    bool terminal{};
    bool cleanupSuppressed{};
    bool renderWitnessValid{};
    bool atomicRenderWitnessValid{};
    bool explicitNeededValid{};
    std::uint32_t terminalSinceMs{};
    std::uint32_t retryAfterMs{};
    std::uint32_t lastRenderedMs{};
    std::uint32_t lastAtomicRenderedMs{};
    std::uint32_t lastAtomicProbeMs{};
    std::uint32_t explicitNeededUntilMs{};
    RenderEntryZone lastEntryZone{RenderEntryZone::Unknown};
    float lastEntryDistanceM{};
};
std::array<LifecycleSlotState, kPoolCapacity> g_lifecycle{};
CleanupTicket g_cleanupCandidate{};
std::atomic<const void*> g_cleanupCandidateEntity{nullptr};
std::uint64_t g_cleanupSelectionsTotal{};
std::uint64_t g_cleanupClaimsTotal{};
std::uint64_t g_cleanupGuardedTotal{};
std::uint64_t g_cleanupStaleTotal{};
std::uint64_t g_cleanupDeletedTotal{};
std::uint64_t g_cleanupDeleteUnconfirmedTotal{};
std::uint64_t g_renderMarksTotal{};
std::uint64_t g_atomicRenderMarksTotal{};
std::uint64_t g_atomicProbeAttemptsTotal{};
std::uint64_t g_atomicProbeNoAtomicTotal{};
std::uint64_t g_atomicProbePipelineFailTotal{};
std::uint64_t g_atomicProbeClumpMismatchTotal{};

std::uint64_t g_revision{};
std::uint64_t g_samples{};
std::uint64_t g_birthsTotal{};
std::uint64_t g_deathsTotal{};
Snapshot g_snapshot{};
std::atomic<int> g_directorBaseTarget{0};
std::atomic<int> g_directorEffectiveTarget{0};
std::atomic<std::uint32_t> g_directorWantedLevel{0};
std::atomic<bool> g_renderWitnessAvailable{false};
std::atomic<bool> g_atomicRenderWitnessAvailable{false};

template <typename T>
T ReadUnaligned(const std::byte* address) {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

bool CanReadThrough(std::uintptr_t base, std::size_t lastOffset) {
    return base != 0U &&
           base <= std::numeric_limits<std::uintptr_t>::max() - lastOffset;
}

std::uint32_t ElapsedMsClamped(std::uint32_t nowMs,
                               std::uint32_t sinceMs) {
    if (nowMs == 0U) return 0U;
    const std::int32_t elapsed =
        static_cast<std::int32_t>(nowMs - sinceMs);
    return elapsed > 0 ? static_cast<std::uint32_t>(elapsed) : 0U;
}

bool DeadlineReached(std::uint32_t nowMs, std::uint32_t deadlineMs) {
    // Zero is GTA's "no deadline" sentinel.  Treat it explicitly so a long
    // session crossing bit 31 cannot turn a never-rendered/never-deferred
    // vehicle into one with a deadline roughly 24 days in the future.
    return nowMs != 0U &&
        (deadlineMs == 0U ||
         static_cast<std::int32_t>(nowMs - deadlineMs) >= 0);
}

bool RenderWitnessGraceExpiredLocked(const LifecycleSlotState& state,
                                     std::uint32_t nowMs,
                                     std::uint32_t graceMs) {
    return state.renderWitnessValid && nowMs != 0U &&
        ElapsedMsClamped(nowMs, state.lastRenderedMs) >= graceMs;
}

bool ObserveExplicitNeededLocked(LifecycleSlotState& state,
                                 std::uint32_t nowMs,
                                 std::uint32_t neededUntilMs) {
    if (nowMs == 0U) return true;

    // CAutomobile::Render writes exactly now+3000. CSetPiece::Update writes
    // now+10000/25000 to the same field. Latch only the latter class so a later
    // render heartbeat cannot shorten retail's explicit ownership window.
    const std::int32_t currentFuture =
        static_cast<std::int32_t>(neededUntilMs - nowMs);
    if (neededUntilMs != 0U &&
        currentFuture > static_cast<std::int32_t>(kStockRenderHeartbeatMs)) {
        if (!state.explicitNeededValid ||
            static_cast<std::int32_t>(
                neededUntilMs - state.explicitNeededUntilMs) > 0) {
            state.explicitNeededUntilMs = neededUntilMs;
        }
        state.explicitNeededValid = true;
    }
    if (state.explicitNeededValid &&
        DeadlineReached(nowMs, state.explicitNeededUntilMs)) {
        state.explicitNeededValid = false;
        state.explicitNeededUntilMs = 0U;
    }
    return state.explicitNeededValid;
}

bool SameCleanupIdentity(const CleanupTicket& a, const CleanupTicket& b) {
    return a.valid && b.valid && a.entity == b.entity && a.slot == b.slot &&
        a.generation == b.generation && a.kind == b.kind;
}

bool ReadPoolView(const void* pool, PoolView& out) {
    const auto poolAddress = reinterpret_cast<std::uintptr_t>(pool);
    if (!CanReadThrough(poolAddress, kPoolOwnsAllocationsOffset) ||
        (poolAddress % alignof(void*)) != 0U) {
        return false;
    }

    const auto* bytes = static_cast<const std::byte*>(pool);
    out.objects = ReadUnaligned<const std::byte*>(bytes + kPoolObjectsOffset);
    out.slotFlags =
        ReadUnaligned<const std::uint8_t*>(bytes + kPoolSlotFlagsOffset);
    out.size = ReadUnaligned<std::int32_t>(bytes + kPoolSizeOffset);
    out.ownsAllocations =
        ReadUnaligned<std::uint8_t>(bytes + kPoolOwnsAllocationsOffset);

    const auto objectsAddress = reinterpret_cast<std::uintptr_t>(out.objects);
    const auto flagsAddress = reinterpret_cast<std::uintptr_t>(out.slotFlags);
    return out.size == static_cast<std::int32_t>(kPoolCapacity) &&
           out.ownsAllocations == 1U &&
           (objectsAddress % alignof(void*)) == 0U &&
           CanReadThrough(objectsAddress, kLastVehicleReadOffset) &&
           CanReadThrough(flagsAddress, kPoolCapacity - 1U);
}

void ClearTrackingLocked(bool resetCadence) {
    if (resetCadence) {
        g_cadenceArmed = false;
        g_lastSampleFrame = 0U;
    }
    g_poolIdentity = nullptr;
    g_objectsIdentity = nullptr;
    g_slotFlagsIdentity = nullptr;
    g_hasBaseline = false;
    g_previousSlotFlags.fill(kPoolSlotFreeBit);
    g_lifecycle.fill(LifecycleSlotState{});
    g_cleanupCandidate = {};
    g_cleanupCandidateEntity.store(nullptr, std::memory_order_release);
    g_cleanupSelectionsTotal = 0U;
    g_cleanupClaimsTotal = 0U;
    g_cleanupGuardedTotal = 0U;
    g_cleanupStaleTotal = 0U;
    g_cleanupDeletedTotal = 0U;
    g_cleanupDeleteUnconfirmedTotal = 0U;
    g_renderMarksTotal = 0U;
    g_atomicRenderMarksTotal = 0U;
    g_atomicProbeAttemptsTotal = 0U;
    g_atomicProbeNoAtomicTotal = 0U;
    g_atomicProbePipelineFailTotal = 0U;
    g_atomicProbeClumpMismatchTotal = 0U;
    g_samples = 0U;
    g_birthsTotal = 0U;
    g_deathsTotal = 0U;
    g_directorBaseTarget.store(0, std::memory_order_relaxed);
    g_directorEffectiveTarget.store(0, std::memory_order_relaxed);
    g_directorWantedLevel.store(0U, std::memory_order_relaxed);
}

void PublishInvalidLocked(std::uint64_t frameSerial) {
    // Do not leave stale live counts visible after a failed pool fingerprint.
    ClearTrackingLocked(false);
    Snapshot next{};
    next.revision = ++g_revision;
    next.frameSerial = frameSerial;
    next.configured = g_configured;
    g_snapshot = next;
}

void StartPoolEpochLocked(const void* pool, const PoolView& view) {
    g_poolIdentity = pool;
    g_objectsIdentity = view.objects;
    g_slotFlagsIdentity = view.slotFlags;
    g_hasBaseline = false;
    g_previousSlotFlags.fill(kPoolSlotFreeBit);
    g_lifecycle.fill(LifecycleSlotState{});
    g_cleanupCandidate = {};
    g_cleanupCandidateEntity.store(nullptr, std::memory_order_release);
    g_cleanupSelectionsTotal = 0U;
    g_cleanupClaimsTotal = 0U;
    g_cleanupGuardedTotal = 0U;
    g_cleanupStaleTotal = 0U;
    g_cleanupDeletedTotal = 0U;
    g_cleanupDeleteUnconfirmedTotal = 0U;
    g_renderMarksTotal = 0U;
    g_atomicRenderMarksTotal = 0U;
    g_atomicProbeAttemptsTotal = 0U;
    g_atomicProbeNoAtomicTotal = 0U;
    g_atomicProbePipelineFailTotal = 0U;
    g_atomicProbeClumpMismatchTotal = 0U;
    g_samples = 0U;
    g_birthsTotal = 0U;
    g_deathsTotal = 0U;
}

bool ReadVehiclePosition(const std::byte* vehicle, float& x, float& y,
                         float& z) {
    const auto* source = vehicle + kEntityPlacementPositionOffset;
    const auto* matrix = ReadUnaligned<const std::byte*>(
        vehicle + kEntityMatrixOffset);
    const auto matrixAddress = reinterpret_cast<std::uintptr_t>(matrix);
    if (matrix && (matrixAddress % alignof(void*)) == 0U &&
        CanReadThrough(matrixAddress, kMatrixPositionOffset + sizeof(float) * 3U)) {
        source = matrix + kMatrixPositionOffset;
    }
    x = ReadUnaligned<float>(source + 0U);
    y = ReadUnaligned<float>(source + sizeof(float));
    z = ReadUnaligned<float>(source + sizeof(float) * 2U);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool ResolveVehicleLocked(const void* vehicle, const PoolView& view,
                          std::size_t& slot, std::uint8_t& generation) {
    if (!vehicle || !view.objects || !view.slotFlags) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(vehicle);
    const auto first = reinterpret_cast<std::uintptr_t>(view.objects);
    constexpr std::size_t kPoolBytes = kPoolCapacity * kVehicleStride;
    if (address < first || address - first >= kPoolBytes) return false;
    const std::uintptr_t delta = address - first;
    if (delta % kVehicleStride != 0U) return false;
    slot = static_cast<std::size_t>(delta / kVehicleStride);
    if (slot >= kPoolCapacity) return false;
    const std::uint8_t flags = view.slotFlags[slot];
    if ((flags & kPoolSlotFreeBit) != 0U) return false;
    generation = flags & kPoolGenerationMask;
    return true;
}

bool ReadCurrentPoolLocked(PoolView& view) {
    if (!g_configured || !g_poolGlobal) return false;
    const void* pool = *g_poolGlobal;
    return pool && pool == g_poolIdentity && ReadPoolView(pool, view) &&
           view.objects == g_objectsIdentity &&
           view.slotFlags == g_slotFlagsIdentity;
}

void BeginLifecycleGenerationLocked(std::size_t slot,
                                    std::uint8_t generation,
                                    std::uint32_t nowMs) {
    auto& state = g_lifecycle[slot];
    if (state.generationValid && state.generation == generation) return;
    state = {};
    state.generation = generation;
    state.generationValid = true;
    // Fresh generations must earn the same minimum age as an observed terminal
    // transition; never inherit an old slot's immediately eligible timestamp.
    state.terminalSinceMs = nowMs;
}

bool IsSupportedCleanupGenerationLocked(
    std::size_t slot, const std::byte* vehicle, std::uint8_t generation,
    CleanupKind kind, std::uint32_t nowMs) {
    auto& state = g_lifecycle[slot];
    if (!state.generationValid || state.generation != generation ||
        state.cleanupSuppressed) {
        return false;
    }
    const std::uint8_t createdBy =
        ReadUnaligned<std::uint8_t>(vehicle + kVehicleCreatedByOffset);
    if (createdBy != kCreatedByRandom) return false;
    const int modelId = static_cast<int>(ReadUnaligned<std::int16_t>(
        vehicle + kEntityModelIdOffset));
    const std::uint8_t status = static_cast<std::uint8_t>(
        ReadUnaligned<std::uint8_t>(vehicle + kEntityTypeStatusOffset) >>
        kEntityStatusShift);
    const bool terminal =
        (ReadUnaligned<std::uint8_t>(vehicle + kVehicleFadeFlagsOffset) &
         kFadeOutBit) != 0U ||
        status == kStatusAbandoned || status == kStatusWrecked;
    if (!terminal) return false;
    if (kind == CleanupKind::ResponseRoad) {
        if (!state.responseProvenance ||
            !IsPoliceRoadVehicleModel(modelId) ||
            !IsRoadVehicleModel(modelId)) {
            return false;
        }
    } else if (kind == CleanupKind::RetiredHelicopter) {
        if (!state.retiredHelicopter ||
            (modelId != 488 && modelId != 497)) {
            return false;
        }
    } else {
        return false;
    }
    const std::uint32_t neededUntil = ReadUnaligned<std::uint32_t>(
        vehicle + kVehicleNeededUntilOffset);
    if (ObserveExplicitNeededLocked(state, nowMs, neededUntil)) return false;
    const bool renderWitnessAvailable =
        g_renderWitnessAvailable.load(std::memory_order_relaxed);
    // A stamped generation uses the exact headset grace. An unstamped one must
    // be decided by the caller's hard radius + HMD cone, otherwise genuinely
    // unseen debris would be retained forever. With no producer at all, retain
    // the complete stock deadline as the conservative fallback.
    if (renderWitnessAvailable) {
        const bool entryGraceExpired = !state.renderWitnessValid ||
            RenderWitnessGraceExpiredLocked(
                state, nowMs, kCleanupRecentRenderGraceMs);
        const bool atomicWitnessAvailable =
            g_atomicRenderWitnessAvailable.load(std::memory_order_relaxed);
        const bool atomicGraceExpired = !atomicWitnessAvailable ||
            !state.atomicRenderWitnessValid ||
            ElapsedMsClamped(nowMs, state.lastAtomicRenderedMs) >=
                kCleanupRecentRenderGraceMs;
        return entryGraceExpired && atomicGraceExpired;
    }
    return DeadlineReached(nowMs, neededUntil);
}

bool TicketMatchesLocked(const CleanupTicket& ticket, std::uint32_t nowMs) {
    if (!ticket.valid || ticket.slot >= kPoolCapacity) return false;
    PoolView view{};
    if (!ReadCurrentPoolLocked(view)) return false;
    const std::uint8_t flags = view.slotFlags[ticket.slot];
    if ((flags & kPoolSlotFreeBit) != 0U ||
        (flags & kPoolGenerationMask) != ticket.generation) {
        return false;
    }
    const auto* vehicle = view.objects + ticket.slot * kVehicleStride;
    if (vehicle != ticket.entity) return false;
    const int modelId = static_cast<int>(ReadUnaligned<std::int16_t>(
        vehicle + kEntityModelIdOffset));
    return modelId == ticket.modelId &&
        IsSupportedCleanupGenerationLocked(
            ticket.slot, vehicle, ticket.generation, ticket.kind, nowMs);
}

void ScanLocked(std::uint64_t frameSerial, std::uint32_t nowMs,
                const Viewpoint* viewpoint) {
    if (!g_configured || !g_poolGlobal) {
        PublishInvalidLocked(frameSerial);
        return;
    }

    // Configure fingerprints the global's address. Its value legitimately stays
    // null until CPools::Initialise, so null is an invalid sample, not an error.
    const void* pool = *g_poolGlobal;
    PoolView view{};
    if (!pool || !ReadPoolView(pool, view)) {
        PublishInvalidLocked(frameSerial);
        return;
    }

    if (pool != g_poolIdentity || view.objects != g_objectsIdentity ||
        view.slotFlags != g_slotFlagsIdentity) {
        // Pool reconstruction is a new epoch. Never turn allocator replacement
        // into fictitious births/deaths by comparing unrelated slot maps.
        StartPoolEpochLocked(pool, view);
    }

    std::array<std::uint8_t, kPoolCapacity> currentSlotFlags{};
    Snapshot next{};
    CleanupTicket oldestCleanup{};
    std::uint32_t oldestCleanupAgeMs = 0U;
    std::uint32_t oldestWitnessProbeAgeMs = 0U;
    int selectedWitnessProbePriority = -1;
    float forwardX = 0.0f;
    float forwardY = 0.0f;
    float cleanupForwardX = 0.0f;
    float cleanupForwardY = 0.0f;
    bool cleanupViewValid = false;
    float witnessForwardX = 0.0f;
    float witnessForwardY = 0.0f;
    float witnessForwardZ = 0.0f;
    bool witnessViewValid = false;
    if (viewpoint && viewpoint->interestValid &&
        std::isfinite(viewpoint->interestX) &&
        std::isfinite(viewpoint->interestY) &&
        std::isfinite(viewpoint->interestZ) &&
        std::isfinite(viewpoint->forwardX) &&
        std::isfinite(viewpoint->forwardY)) {
        const float forwardLengthSq = viewpoint->forwardX * viewpoint->forwardX +
            viewpoint->forwardY * viewpoint->forwardY;
        if (forwardLengthSq >= 0.25f && forwardLengthSq <= 4.0f) {
            const float inverseLength = 1.0f / std::sqrt(forwardLengthSq);
            forwardX = viewpoint->forwardX * inverseLength;
            forwardY = viewpoint->forwardY * inverseLength;
            next.localViewValid = true;
        }
    }
    if (viewpoint && viewpoint->headsetValid &&
        std::isfinite(viewpoint->headsetForwardX) &&
        std::isfinite(viewpoint->headsetForwardY)) {
        const float headsetLengthSq =
            viewpoint->headsetForwardX * viewpoint->headsetForwardX +
            viewpoint->headsetForwardY * viewpoint->headsetForwardY;
        if (headsetLengthSq >= 0.25f && headsetLengthSq <= 4.0f) {
            const float inverseLength = 1.0f / std::sqrt(headsetLengthSq);
            cleanupForwardX = viewpoint->headsetForwardX * inverseLength;
            cleanupForwardY = viewpoint->headsetForwardY * inverseLength;
            cleanupViewValid = viewpoint->valid &&
                std::isfinite(viewpoint->x) &&
                std::isfinite(viewpoint->y) &&
                std::isfinite(viewpoint->z);
        }
    }
    if (viewpoint && viewpoint->headsetValid && viewpoint->valid &&
        std::isfinite(viewpoint->x) && std::isfinite(viewpoint->y) &&
        std::isfinite(viewpoint->z) &&
        std::isfinite(viewpoint->headsetForwardX) &&
        std::isfinite(viewpoint->headsetForwardY) &&
        std::isfinite(viewpoint->headsetForwardZ)) {
        const float headsetLengthSq3d =
            viewpoint->headsetForwardX * viewpoint->headsetForwardX +
            viewpoint->headsetForwardY * viewpoint->headsetForwardY +
            viewpoint->headsetForwardZ * viewpoint->headsetForwardZ;
        if (headsetLengthSq3d >= 0.25f && headsetLengthSq3d <= 4.0f) {
            const float inverseLength = 1.0f / std::sqrt(headsetLengthSq3d);
            witnessForwardX = viewpoint->headsetForwardX * inverseLength;
            witnessForwardY = viewpoint->headsetForwardY * inverseLength;
            witnessForwardZ = viewpoint->headsetForwardZ * inverseLength;
            witnessViewValid = true;
        }
    }
    const bool renderWitnessAvailable =
        g_renderWitnessAvailable.load(std::memory_order_relaxed);
    const bool atomicRenderWitnessAvailable =
        g_atomicRenderWitnessAvailable.load(std::memory_order_relaxed);
    if (!cleanupViewValid && next.localViewValid &&
        !renderWitnessAvailable) {
        cleanupForwardX = forwardX;
        cleanupForwardY = forwardY;
        cleanupViewValid = true;
    }
    next.cleanupRenderWitnessAvailable = renderWitnessAvailable;
    next.cleanupAtomicWitnessAvailable = atomicRenderWitnessAvailable;
    for (std::size_t slot = 0; slot < kPoolCapacity; ++slot) {
        const std::uint8_t slotFlags = view.slotFlags[slot];
        currentSlotFlags[slot] = slotFlags;
        if ((slotFlags & kPoolSlotFreeBit) != 0U) {
            g_lifecycle[slot] = {};
            continue;
        }

        const std::uint8_t previousSlotFlags = g_previousSlotFlags[slot];
        const bool wasOccupied =
            (previousSlotFlags & kPoolSlotFreeBit) == 0U;
        const std::uint8_t previousGeneration =
            previousSlotFlags & kPoolGenerationMask;
        const std::uint8_t currentGeneration =
            slotFlags & kPoolGenerationMask;
        const bool lifecycleAlreadyStamped =
            g_lifecycle[slot].generationValid &&
            g_lifecycle[slot].generation == currentGeneration;
        if ((!g_hasBaseline || !wasOccupied ||
             previousGeneration != currentGeneration) &&
            !lifecycleAlreadyStamped) {
            g_lifecycle[slot] = {};
        }
        BeginLifecycleGenerationLocked(slot, currentGeneration, nowMs);
        auto& lifecycle = g_lifecycle[slot];
        if (lifecycle.renderWitnessValid) ++next.cleanupRenderStamped;
        if (lifecycle.atomicRenderWitnessValid) ++next.cleanupAtomicStamped;

        ++next.live;
        const auto* vehicle = view.objects + slot * kVehicleStride;
        const std::uint32_t neededUntil = ReadUnaligned<std::uint32_t>(
            vehicle + kVehicleNeededUntilOffset);
        const bool explicitNeededActive = ObserveExplicitNeededLocked(
            lifecycle, nowMs, neededUntil);
        const std::uint8_t createdBy =
            ReadUnaligned<std::uint8_t>(vehicle + kVehicleCreatedByOffset);
        switch (createdBy) {
            case kCreatedByRandom:    ++next.random; break;
            case kCreatedByMission:   ++next.mission; break;
            case kCreatedByParked:    ++next.parked; break;
            case kCreatedByPermanent: ++next.permanent; break;
            default:                  ++next.unknown; break;
        }

        const std::uint8_t lawFlags =
            ReadUnaligned<std::uint8_t>(vehicle + kVehicleLawFlagsOffset);
        const bool lawVehicle = (lawFlags & kLawEnforcerBit) != 0U;
        if (lawVehicle) {
            ++next.law;
        }
        const int modelId = static_cast<int>(ReadUnaligned<std::int16_t>(
            vehicle + kEntityModelIdOffset));
        const bool roadVehicle = IsRoadVehicleModel(modelId);
        const bool policeRoadModel = IsPoliceRoadVehicleModel(modelId);
        const bool helicopter = IsHelicopterModel(modelId);
        const bool aircraft = IsAircraftModel(modelId);
        const bool responseWitness = roadVehicle &&
            createdBy == kCreatedByRandom &&
            (lawVehicle || policeRoadModel);
        if (responseWitness) {
            lifecycle.responseProvenance = true;
        }
        const bool responseVehicle = roadVehicle &&
            lifecycle.responseProvenance;
        // Preserve the old mutable-axis witness for diagnostics, but never let
        // a response generation close civilian demand after retail clears its
        // law bit or reclassifies it as RANDOM_VEHICLE.
        const bool legacyRoadAmbient = roadVehicle &&
            createdBy == kCreatedByRandom && !lawVehicle;
        const bool roadAmbient = legacyRoadAmbient && !responseVehicle;
        const std::uint8_t entityStatus = static_cast<std::uint8_t>(
            ReadUnaligned<std::uint8_t>(
                vehicle + kEntityTypeStatusOffset) >> kEntityStatusShift);
        const bool abandoned = entityStatus == kStatusAbandoned;
        const bool wrecked = entityStatus == kStatusWrecked;
        const bool fading =
            (ReadUnaligned<std::uint8_t>(vehicle + kVehicleFadeFlagsOffset) &
             kFadeOutBit) != 0U;
        const bool driverless =
            ReadUnaligned<const void*>(vehicle + kVehicleDriverOffset) == nullptr;
        const float moveX = ReadUnaligned<float>(
            vehicle + kPhysicalMoveSpeedOffset);
        const float moveY = ReadUnaligned<float>(
            vehicle + kPhysicalMoveSpeedOffset + sizeof(float));
        const float moveSpeedSq = moveX * moveX + moveY * moveY;
        const bool moving = std::isfinite(moveSpeedSq) &&
            moveSpeedSq > kMovingSpeedSq;
        // Instantaneous zero speed can be a traffic light, so movement remains
        // an independent witness. Effective means occupied, alive and not
        // already in GTA's fade-out lifecycle; route-progress/stall history can
        // refine this later without making red lights disappear from demand.
        const bool effective = roadAmbient && !driverless && !fading &&
            !abandoned && !wrecked;
        const bool terminal = roadAmbient &&
            (fading || abandoned || wrecked);
        const bool responseAiStatus = entityStatus == kStatusSimple ||
            entityStatus == kStatusPhysics;
        const bool responseEffective = responseVehicle &&
            createdBy == kCreatedByRandom && responseAiStatus && !driverless &&
            !fading && !abandoned && !wrecked;
        const bool responseTerminal = responseVehicle &&
            (fading || abandoned || wrecked);
        const bool retiredHelicopter = lifecycle.retiredHelicopter &&
            (modelId == 488 || modelId == 497) &&
            createdBy == kCreatedByRandom;
        const bool supportedResponseTerminal = responseTerminal &&
            policeRoadModel && createdBy == kCreatedByRandom;
        const bool cleanupTerminal = supportedResponseTerminal ||
            (retiredHelicopter && (fading || abandoned || wrecked));
        if (cleanupTerminal) {
            if (!lifecycle.terminal) {
                lifecycle.terminal = true;
                lifecycle.terminalSinceMs = nowMs;
            }
            ++next.cleanupTerminal;
            if (retiredHelicopter) ++next.cleanupRetiredHelicopters;

            std::uint32_t terminalAgeMs =
                ElapsedMsClamped(nowMs, lifecycle.terminalSinceMs);
            if (nowMs != 0U && lifecycle.terminalSinceMs != 0U &&
                static_cast<std::int32_t>(
                    nowMs - lifecycle.terminalSinceMs) < 0) {
                // CTimer may move backwards across a load.  Rebase this exact
                // generation instead of turning the rollback into ~49 days of
                // terminal age or retaining an obsolete retry deadline.
                lifecycle.terminalSinceMs = nowMs;
                lifecycle.retryAfterMs = 0U;
                terminalAgeMs = 0U;
            }
            const bool oldEnough = nowMs != 0U &&
                terminalAgeMs >= kCleanupMinimumTerminalAgeMs;
            const bool renderGraceExpired = DeadlineReached(nowMs, neededUntil);
            const bool generationWitnessed = renderWitnessAvailable &&
                lifecycle.renderWitnessValid;
            const std::uint32_t lastRenderedAgeMs =
                lifecycle.renderWitnessValid
                    ? ElapsedMsClamped(nowMs, lifecycle.lastRenderedMs) : 0U;
            const bool actualRenderGraceExpired = generationWitnessed &&
                RenderWitnessGraceExpiredLocked(
                    lifecycle, nowMs, kCleanupRecentRenderGraceMs);
            const bool generationAtomicWitnessed =
                atomicRenderWitnessAvailable &&
                lifecycle.atomicRenderWitnessValid;
            const std::uint32_t lastAtomicRenderedAgeMs =
                lifecycle.atomicRenderWitnessValid
                    ? ElapsedMsClamped(nowMs, lifecycle.lastAtomicRenderedMs)
                    : 0U;
            const bool atomicRenderRecent = generationAtomicWitnessed &&
                lastAtomicRenderedAgeMs < kCleanupRecentRenderGraceMs;
            const bool atomicRenderGraceExpired =
                !atomicRenderWitnessAvailable ||
                !generationAtomicWitnessed || !atomicRenderRecent;
            const bool authoritativeRenderGraceExpired =
                (generationWitnessed ? actualRenderGraceExpired
                    : (renderWitnessAvailable ? true : renderGraceExpired)) &&
                atomicRenderGraceExpired;
            const bool retryReady = DeadlineReached(nowMs,
                                                    lifecycle.retryAfterMs);

            float vehicleX = 0.0F;
            float vehicleY = 0.0F;
            float vehicleZ = 0.0F;
            bool outsideHeadsetField = false;
            bool outsideRenderWitnessField = false;
            float distance = 0.0F;
            if (cleanupViewValid &&
                ReadVehiclePosition(vehicle, vehicleX, vehicleY, vehicleZ)) {
                const float dx = vehicleX - viewpoint->x;
                const float dy = vehicleY - viewpoint->y;
                const float dz = vehicleZ - viewpoint->z;
                const float horizontalSq = dx * dx + dy * dy;
                const float distanceSq = horizontalSq + dz * dz;
                distance = std::sqrt(std::max(0.0F, distanceSq));
                const float horizontalDistance =
                    std::sqrt(std::max(0.0F, horizontalSq));
                const float dot = dx * cleanupForwardX +
                    dy * cleanupForwardY;
                outsideHeadsetField = distance > kCleanupNearRadiusM &&
                    dot < kCleanupConeCosHalfAngle * horizontalDistance;
            }
            const bool retiredDistanceReady = !retiredHelicopter ||
                distance > kCleanupRetiredHelicopterRadiusM;
            const bool positionValid = cleanupViewValid &&
                std::isfinite(distance) && distance > 0.0F;
            const bool outsideNearRadius = positionValid &&
                distance > kCleanupNearRadiusM;
            CleanupViewZone witnessZone = CleanupViewZone::Invalid;
            if (positionValid) {
                if (!outsideNearRadius) {
                    witnessZone = CleanupViewZone::Near45;
                } else if (!outsideHeadsetField) {
                    witnessZone = CleanupViewZone::CoreCone;
                } else if (witnessViewValid) {
                    const float witnessDot =
                        (vehicleX - viewpoint->x) * witnessForwardX +
                        (vehicleY - viewpoint->y) * witnessForwardY +
                        (vehicleZ - viewpoint->z) * witnessForwardZ;
                    outsideRenderWitnessField = witnessDot <
                        kRenderWitnessConeCosHalfAngle * distance;
                    witnessZone = outsideRenderWitnessField
                        ? CleanupViewZone::OutsideWitness
                        : CleanupViewZone::WitnessFringe;
                }
            }
            switch (witnessZone) {
                case CleanupViewZone::Near45:
                    ++next.cleanupTerminalZoneNear45; break;
                case CleanupViewZone::CoreCone:
                    ++next.cleanupTerminalZoneCoreCone; break;
                case CleanupViewZone::WitnessFringe:
                    ++next.cleanupTerminalZoneWitnessFringe; break;
                case CleanupViewZone::OutsideWitness:
                    ++next.cleanupTerminalZoneOutsideWitness; break;
                case CleanupViewZone::Invalid:
                default:
                    ++next.cleanupTerminalZoneInvalid; break;
            }
            // Current headset geometry remains authoritative even after a pool
            // generation has render history. Otherwise a turn towards a stale
            // wreck could race the next eye render and delete it in view.
            const bool outsideCurrentField = retiredHelicopter
                ? outsideHeadsetField
                : witnessViewValid && outsideRenderWitnessField;
            const bool visibilitySafe = outsideNearRadius &&
                outsideCurrentField;
            const bool eligible = oldEnough && !explicitNeededActive &&
                authoritativeRenderGraceExpired && retryReady &&
                !lifecycle.cleanupSuppressed && visibilitySafe &&
                retiredDistanceReady;
            const bool blockedByRecent = oldEnough &&
                !explicitNeededActive && !authoritativeRenderGraceExpired;
            if (blockedByRecent) {
                if (atomicRenderRecent) {
                    ++next.cleanupRecentAtomicRecent;
                } else if (!atomicRenderWitnessAvailable) {
                    ++next.cleanupRecentAtomicUnavailable;
                } else {
                    ++next.cleanupRecentEntryOnly;
                    if (generationAtomicWitnessed) {
                        ++next.cleanupRecentAtomicStale;
                    } else {
                        ++next.cleanupRecentAtomicNever;
                    }
                }
            }
            const bool suspiciousWitness = blockedByRecent &&
                atomicRenderWitnessAvailable && !atomicRenderRecent;
            const int witnessProbePriority = suspiciousWitness
                ? (generationAtomicWitnessed ? 2 : 3) : 0;
            if (!next.cleanupWitnessProbe.valid ||
                witnessProbePriority > selectedWitnessProbePriority ||
                (witnessProbePriority == selectedWitnessProbePriority &&
                 terminalAgeMs > oldestWitnessProbeAgeMs)) {
                std::uint8_t stateMask = 0U;
                if (fading) stateMask |= 1U << 0U;
                if (abandoned) stateMask |= 1U << 1U;
                if (wrecked) stateMask |= 1U << 2U;
                if (driverless) stateMask |= 1U << 3U;
                next.cleanupWitnessProbe = {
                    .slot = static_cast<std::uint16_t>(slot),
                    .generation = currentGeneration,
                    .modelId = static_cast<std::int16_t>(modelId),
                    .zone = witnessZone,
                    .entryZone = lifecycle.lastEntryZone,
                    .stateMask = stateMask,
                    .terminalAgeMs = terminalAgeMs,
                    .entryAgeMs = lastRenderedAgeMs,
                    .atomicAgeMs = lastAtomicRenderedAgeMs,
                    .distanceM = distance,
                    .entryDistanceM = lifecycle.lastEntryDistanceM,
                    .entryStamped = generationWitnessed,
                    .entryRecent = generationWitnessed &&
                        !actualRenderGraceExpired,
                    .atomicStamped = generationAtomicWitnessed,
                    .atomicRecent = atomicRenderRecent,
                    .suspicious = suspiciousWitness,
                    .valid = true,
                };
                oldestWitnessProbeAgeMs = terminalAgeMs;
                selectedWitnessProbePriority = witnessProbePriority;
            }
            if (!oldEnough) {
                ++next.cleanupBlockedYoung;
            } else if (explicitNeededActive) {
                ++next.cleanupBlockedExplicitNeeded;
            } else if (!retryReady) {
                ++next.cleanupBlockedRetry;
            } else if (lifecycle.cleanupSuppressed) {
                ++next.cleanupBlockedSuppressed;
            } else if (!positionValid) {
                ++next.cleanupBlockedViewInvalid;
            } else if (!outsideNearRadius) {
                ++next.cleanupBlockedNear;
            } else if (!outsideCurrentField) {
                ++next.cleanupBlockedConeFallback;
            } else if (!retiredDistanceReady) {
                ++next.cleanupBlockedRetiredDistance;
            } else if (!authoritativeRenderGraceExpired) {
                ++next.cleanupBlockedRecentRender;
            }
            if (eligible) {
                ++next.cleanupEligible;
                if (!oldestCleanup.valid ||
                    terminalAgeMs > oldestCleanupAgeMs) {
                    oldestCleanup = {
                        .entity = const_cast<std::byte*>(vehicle),
                        .slot = static_cast<std::uint16_t>(slot),
                        .generation = currentGeneration,
                        .kind = retiredHelicopter
                            ? CleanupKind::RetiredHelicopter
                            : CleanupKind::ResponseRoad,
                        .modelId = static_cast<std::int16_t>(modelId),
                        .terminalAgeMs = terminalAgeMs,
                        .lastRenderedAgeMs = lastRenderedAgeMs,
                        .distanceM = distance,
                        .renderWitnessBacked = generationWitnessed,
                        .valid = true,
                    };
                    oldestCleanupAgeMs = terminalAgeMs;
                }
            }
        } else {
            lifecycle.terminal = false;
            lifecycle.terminalSinceMs = nowMs;
        }
        if (roadVehicle) {
            ++next.road;
            if (roadAmbient) ++next.roadRandom;
            if (lawVehicle) {
                ++next.roadLaw;
                if (responseEffective) ++next.roadLawEffective;
            }
        }
        if (roadAmbient) {
            if (effective) ++next.roadRandomEffective;
            if (moving) ++next.roadRandomMoving;
            if (driverless) ++next.roadRandomDriverless;
            if (fading) ++next.roadRandomFading;
            if (abandoned) ++next.roadRandomAbandoned;
            if (wrecked) ++next.roadRandomWrecked;
            if (terminal) ++next.roadRandomTerminal;
        }
        if (responseVehicle) {
            ++next.roadResponse;
            if (responseEffective) ++next.roadResponseEffective;
            if (driverless) ++next.roadResponseDriverless;
            if (fading) ++next.roadResponseFading;
            if (abandoned) ++next.roadResponseAbandoned;
            if (wrecked) ++next.roadResponseWrecked;
            if (responseTerminal) ++next.roadResponseTerminal;
        }
        if (roadVehicle && policeRoadModel) {
            ++next.roadPoliceModel;
            if (lawVehicle) ++next.roadPoliceModelLaw;
            if (legacyRoadAmbient) ++next.roadPoliceModelAmbient;
            if (modelId == 523) {
                ++next.roadCopBike;
                if (lawVehicle) ++next.roadCopBikeLaw;
                if (legacyRoadAmbient) ++next.roadCopBikeAmbient;
            }
        }
        if (aircraft) ++next.aircraft;
        if (helicopter) ++next.helicopters;
        if (lawVehicle && !roadVehicle) ++next.nonRoadLaw;

        if (next.localViewValid) {
            float vehicleX = 0.0f;
            float vehicleY = 0.0f;
            float vehicleZ = 0.0f;
            if (ReadVehiclePosition(vehicle, vehicleX, vehicleY, vehicleZ)) {
                const float dx = vehicleX - viewpoint->interestX;
                const float dy = vehicleY - viewpoint->interestY;
                const float dz = vehicleZ - viewpoint->interestZ;
                const float distanceSq = dx * dx + dy * dy;
                const bool heightCompatible = std::fabs(dz) <= 25.0f;
                const bool near = heightCompatible && distanceSq <= 90.0f * 90.0f;
                const float along = dx * forwardX + dy * forwardY;
                const float lateral = std::fabs(-dx * forwardY + dy * forwardX);
                const bool forward = heightCompatible && along >= 60.0f &&
                    along <= 240.0f && lateral <= 90.0f;
                const bool useful = near || forward;
                if (lawVehicle) {
                    if (useful) ++next.localLawUseful;
                } else if (createdBy == kCreatedByRandom && !responseVehicle) {
                    if (near) ++next.localRandomNear;
                    if (forward) ++next.localRandomForward;
                    if (useful) ++next.localRandomUseful;
                }
                if (roadVehicle && lawVehicle) {
                    if (useful) ++next.localRoadLawUseful;
                } else if (roadAmbient) {
                    if (near) ++next.localRoadRandomNear;
                    if (forward) ++next.localRoadRandomForward;
                    if (useful) ++next.localRoadRandomUseful;
                }
                if (effective) {
                    if (near) ++next.localRoadRandomEffectiveNear;
                    if (forward) ++next.localRoadRandomEffectiveForward;
                    if (useful) ++next.localRoadRandomEffectiveUseful;
                }
            }
        }
    }

    std::uint64_t birthsDelta = 0U;
    std::uint64_t deathsDelta = 0U;
    if (g_hasBaseline) {
        for (std::size_t slot = 0; slot < kPoolCapacity; ++slot) {
            const std::uint8_t previous = g_previousSlotFlags[slot];
            const std::uint8_t current = currentSlotFlags[slot];
            const bool wasOccupied = (previous & kPoolSlotFreeBit) == 0U;
            const bool isOccupied = (current & kPoolSlotFreeBit) == 0U;
            const std::uint8_t previousGeneration =
                previous & kPoolGenerationMask;
            const std::uint8_t currentGeneration =
                current & kPoolGenerationMask;

            // Retail increments the seven-bit generation on allocation, not on
            // deletion. Modulo subtraction therefore recovers births even when
            // a slot is occupied at both samples (or completed a full lifetime
            // while free at both). Occupancy balance then yields deaths.
            std::uint32_t slotBirths =
                (currentGeneration - previousGeneration) &
                kPoolGenerationMask;
            if (!wasOccupied && isOccupied && slotBirths == 0U) {
                // NewAt(ref) may install an explicit generation equal to the old
                // one. The occupancy edge is still conclusive evidence of birth.
                slotBirths = 1U;
            }
            const std::int32_t slotDeaths =
                static_cast<std::int32_t>(slotBirths) +
                (wasOccupied ? 1 : 0) - (isOccupied ? 1 : 0);
            if (slotDeaths < 0) {
                // An impossible allocator transition means this map is not the
                // fingerprinted layout. Re-baseline rather than publish guesses.
                StartPoolEpochLocked(pool, view);
                currentSlotFlags.fill(kPoolSlotFreeBit);
                PublishInvalidLocked(frameSerial);
                return;
            }
            birthsDelta += slotBirths;
            deathsDelta += static_cast<std::uint32_t>(slotDeaths);
        }
    }

    g_previousSlotFlags = currentSlotFlags;
    if (g_hasBaseline) {
        g_birthsTotal += birthsDelta;
        g_deathsTotal += deathsDelta;
    } else {
        birthsDelta = 0U;
        deathsDelta = 0U;
        g_hasBaseline = true;
    }
    ++g_samples;

    next.cleanupOverBudget =
        next.cleanupTerminal > kCleanupDebrisBudget;
    if (next.cleanupOverBudget && oldestCleanup.valid) {
        const bool newlySelected =
            !SameCleanupIdentity(g_cleanupCandidate, oldestCleanup);
        g_cleanupCandidate = oldestCleanup;
        g_cleanupCandidateEntity.store(
            oldestCleanup.entity, std::memory_order_release);
        if (newlySelected) ++g_cleanupSelectionsTotal;
    } else {
        g_cleanupCandidate = {};
        g_cleanupCandidateEntity.store(nullptr, std::memory_order_release);
    }
    next.cleanupCandidatePending = g_cleanupCandidate.valid;
    next.cleanupCandidateKind = g_cleanupCandidate.kind;
    next.cleanupCandidateAgeMs = g_cleanupCandidate.terminalAgeMs;
    next.cleanupCandidateDistanceM = g_cleanupCandidate.distanceM;

    next.revision = ++g_revision;
    next.frameSerial = frameSerial;
    next.samples = g_samples;
    next.configured = true;
    next.valid = true;
    next.birthsTotal = g_birthsTotal;
    next.deathsTotal = g_deathsTotal;
    next.churnTotal = g_birthsTotal + g_deathsTotal;
    next.birthsDelta = static_cast<std::uint32_t>(birthsDelta);
    next.deathsDelta = static_cast<std::uint32_t>(deathsDelta);
    next.churnDelta = static_cast<std::uint32_t>(birthsDelta + deathsDelta);
    g_snapshot = next;
}

}  // namespace

bool Configure(void** poolGlobal, std::uintptr_t loadBase) {
    std::lock_guard<std::mutex> lock(g_mutex);

    bool fingerprintMatches = false;
    if (poolGlobal && loadBase != 0U &&
        loadBase <= std::numeric_limits<std::uintptr_t>::max() -
                        kVehiclePoolGlobalOffset) {
        fingerprintMatches =
            reinterpret_cast<std::uintptr_t>(poolGlobal) ==
            loadBase + kVehiclePoolGlobalOffset;
    }

    g_poolGlobal = fingerprintMatches ? poolGlobal : nullptr;
    g_configured = fingerprintMatches;
    ClearTrackingLocked(true);

    Snapshot next{};
    next.revision = ++g_revision;
    next.configured = g_configured;
    g_snapshot = next;
    return fingerprintMatches;
}

void Reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ClearTrackingLocked(true);

    Snapshot next{};
    next.revision = ++g_revision;
    next.configured = g_configured;
    g_snapshot = next;
}

void SampleIfDue(std::uint64_t frameSerial, std::uint32_t nowMs,
                 const Viewpoint* viewpoint) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_configured) {
        return;
    }

    if (g_cadenceArmed) {
        if (frameSerial == g_lastSampleFrame) {
            return;
        }
        if (frameSerial > g_lastSampleFrame &&
            frameSerial - g_lastSampleFrame < kSamplePeriodFrames) {
            return;
        }
        if (frameSerial < g_lastSampleFrame) {
            // A restarted frame clock defines a fresh measurement epoch.
            ClearTrackingLocked(false);
        }
    }

    g_cadenceArmed = true;
    g_lastSampleFrame = frameSerial;
    ScanLocked(frameSerial, nowMs, viewpoint);
}

bool GetVehicleIdentity(const void* vehicle, VehicleIdentity* identity) {
    if (identity) *identity = {};
    std::lock_guard<std::mutex> lock(g_mutex);
    PoolView view{};
    if (!identity || !ReadCurrentPoolLocked(view)) return false;
    std::size_t slot = 0U;
    std::uint8_t generation = 0U;
    if (!ResolveVehicleLocked(vehicle, view, slot, generation)) return false;
    *identity = {
        .slot = static_cast<std::uint16_t>(slot),
        .generation = generation,
        .valid = true,
    };
    return true;
}

bool MarkRetiredHelicopter(void* vehicle, std::uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    PoolView view{};
    if (!ReadCurrentPoolLocked(view)) return false;
    std::size_t slot = 0U;
    std::uint8_t generation = 0U;
    if (!ResolveVehicleLocked(vehicle, view, slot, generation)) return false;
    const auto* bytes = view.objects + slot * kVehicleStride;
    const int modelId = static_cast<int>(ReadUnaligned<std::int16_t>(
        bytes + kEntityModelIdOffset));
    if (modelId != 488 && modelId != 497) return false;
    BeginLifecycleGenerationLocked(slot, generation, nowMs);
    auto& state = g_lifecycle[slot];
    state.retiredHelicopter = true;
    state.terminal = true;
    state.terminalSinceMs = nowMs;
    state.retryAfterMs = nowMs;
    return true;
}

void SetRenderWitnessAvailable(bool available) {
    g_renderWitnessAvailable.store(available, std::memory_order_release);
}

bool IsRenderWitnessAvailable() {
    return g_renderWitnessAvailable.load(std::memory_order_acquire);
}

void SetAtomicRenderWitnessAvailable(bool available) {
    g_atomicRenderWitnessAvailable.store(available,
                                          std::memory_order_release);
}

bool IsAtomicRenderWitnessAvailable() {
    return g_atomicRenderWitnessAvailable.load(std::memory_order_acquire);
}

void ObserveVehicleExplicitNeeded(void* vehicle, std::uint32_t nowMs,
                                  std::uint32_t neededUntilMs) {
    if (!vehicle || nowMs == 0U || neededUntilMs == 0U ||
        static_cast<std::int32_t>(neededUntilMs - nowMs) <=
            static_cast<std::int32_t>(kStockRenderHeartbeatMs)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    PoolView view{};
    if (!ReadCurrentPoolLocked(view)) return;
    std::size_t slot = 0U;
    std::uint8_t generation = 0U;
    if (!ResolveVehicleLocked(vehicle, view, slot, generation)) return;
    BeginLifecycleGenerationLocked(slot, generation, nowMs);
    ObserveExplicitNeededLocked(
        g_lifecycle[slot], nowMs, neededUntilMs);
}

void MarkVehicleRendered(void* vehicle, std::uint32_t nowMs,
                         RenderEntryZone zone, float distanceM,
                         AtomicRenderProbe* atomicProbe) {
    if (atomicProbe) *atomicProbe = {};
    if (!vehicle || nowMs == 0U || !IsRenderWitnessAvailable()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    PoolView view{};
    if (!ReadCurrentPoolLocked(view)) return;
    std::size_t slot = 0U;
    std::uint8_t generation = 0U;
    if (!ResolveVehicleLocked(vehicle, view, slot, generation)) return;
    BeginLifecycleGenerationLocked(slot, generation, nowMs);
    auto& state = g_lifecycle[slot];
    state.renderWitnessValid = true;
    state.lastRenderedMs = nowMs;
    state.lastEntryZone = zone;
    state.lastEntryDistanceM =
        std::isfinite(distanceM) && distanceM >= 0.0F ? distanceM : 0.0F;
    ++g_renderMarksTotal;
    constexpr std::uint32_t kAtomicProbeCadenceMs = 250U;
    const std::int32_t atomicProbeDelta = static_cast<std::int32_t>(
        nowMs - state.lastAtomicProbeMs);
    const bool atomicProbeDue = state.lastAtomicProbeMs == 0U ||
        atomicProbeDelta < 0 ||
        atomicProbeDelta >= static_cast<std::int32_t>(
            kAtomicProbeCadenceMs);
    if (atomicProbe && state.terminal && atomicProbeDue &&
        g_atomicRenderWitnessAvailable.load(std::memory_order_relaxed)) {
        state.lastAtomicProbeMs = nowMs;
        *atomicProbe = {
            .entity = vehicle,
            .slot = static_cast<std::uint16_t>(slot),
            .generation = generation,
            .valid = true,
        };
    }
}

void CompleteVehicleAtomicProbe(const AtomicRenderProbe& probe,
                                std::uint32_t nowMs,
                                AtomicProbeOutcome outcome) {
    if (!probe.valid || !probe.entity || nowMs == 0U ||
        !IsAtomicRenderWitnessAvailable()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    PoolView view{};
    if (!ReadCurrentPoolLocked(view) || probe.slot >= kPoolCapacity) return;
    const std::uint8_t flags = view.slotFlags[probe.slot];
    if ((flags & kPoolSlotFreeBit) != 0U ||
        (flags & kPoolGenerationMask) != probe.generation ||
        view.objects + probe.slot * kVehicleStride != probe.entity) {
        return;
    }
    auto& state = g_lifecycle[probe.slot];
    if (!state.generationValid || state.generation != probe.generation ||
        !state.terminal) {
        return;
    }
    ++g_atomicProbeAttemptsTotal;
    switch (outcome) {
        case AtomicProbeOutcome::Submitted:
            state.atomicRenderWitnessValid = true;
            state.lastAtomicRenderedMs = nowMs;
            ++g_atomicRenderMarksTotal;
            break;
        case AtomicProbeOutcome::PipelineFailure:
            ++g_atomicProbePipelineFailTotal;
            break;
        case AtomicProbeOutcome::ClumpMismatch:
            ++g_atomicProbeClumpMismatchTotal;
            break;
        case AtomicProbeOutcome::NoAtomic:
        default:
            ++g_atomicProbeNoAtomicTotal;
            break;
    }
}

bool GetVehicleRenderWitnessStatus(const void* vehicle, std::uint32_t nowMs,
                                   std::uint32_t graceMs,
                                   RenderWitnessStatus* status) {
    if (status) *status = {};
    if (!vehicle || !status || nowMs == 0U || graceMs == 0U) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    PoolView view{};
    if (!ReadCurrentPoolLocked(view)) return false;
    std::size_t slot = 0U;
    std::uint8_t generation = 0U;
    if (!ResolveVehicleLocked(vehicle, view, slot, generation)) return false;
    BeginLifecycleGenerationLocked(slot, generation, nowMs);
    auto& state = g_lifecycle[slot];
    const auto* bytes = view.objects + slot * kVehicleStride;
    const std::uint32_t neededUntil = ReadUnaligned<std::uint32_t>(
        bytes + kVehicleNeededUntilOffset);
    status->available =
        g_renderWitnessAvailable.load(std::memory_order_relaxed);
    status->atomicAvailable =
        g_atomicRenderWitnessAvailable.load(std::memory_order_relaxed);
    status->explicitNeeded =
        ObserveExplicitNeededLocked(state, nowMs, neededUntil);
    status->stamped = status->available && state.renderWitnessValid;
    if (status->stamped) {
        status->ageMs = ElapsedMsClamped(nowMs, state.lastRenderedMs);
        status->recent = status->ageMs < graceMs;
    }
    status->atomicStamped = status->atomicAvailable &&
        state.atomicRenderWitnessValid;
    if (status->atomicStamped) {
        status->atomicAgeMs = ElapsedMsClamped(
            nowMs, state.lastAtomicRenderedMs);
        status->atomicRecent = status->atomicAgeMs < graceMs;
    }
    return true;
}

bool IsCleanupCandidateEntity(const void* vehicle) {
    return vehicle && g_cleanupCandidateEntity.load(
        std::memory_order_acquire) == vehicle;
}

bool ClaimCleanupTicket(void* vehicle, std::uint32_t nowMs,
                        CleanupTicket* ticket) {
    if (ticket) *ticket = {};
    if (!ticket || !IsCleanupCandidateEntity(vehicle)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cleanupCandidate.entity != vehicle ||
        !TicketMatchesLocked(g_cleanupCandidate, nowMs)) {
        ++g_cleanupStaleTotal;
        g_cleanupCandidate = {};
        g_cleanupCandidateEntity.store(nullptr, std::memory_order_release);
        return false;
    }
    *ticket = g_cleanupCandidate;
    ++g_cleanupClaimsTotal;
    g_cleanupCandidate = {};
    g_cleanupCandidateEntity.store(nullptr, std::memory_order_release);
    return true;
}

bool RevalidateCleanupTicket(const CleanupTicket& ticket,
                             std::uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return TicketMatchesLocked(ticket, nowMs);
}

void CompleteCleanupTicket(const CleanupTicket& ticket, std::uint32_t nowMs,
                           CleanupOutcome outcome) {
    if (!ticket.valid || ticket.slot >= kPoolCapacity) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (outcome == CleanupOutcome::Deleted) {
        PoolView view{};
        const bool poolReadable = ReadCurrentPoolLocked(view);
        bool stillSameGeneration = false;
        if (poolReadable) {
            const std::uint8_t flags = view.slotFlags[ticket.slot];
            stillSameGeneration =
                (flags & kPoolSlotFreeBit) == 0U &&
                (flags & kPoolGenerationMask) == ticket.generation &&
                view.objects + ticket.slot * kVehicleStride == ticket.entity;
        }
        if (!poolReadable || stillSameGeneration) {
            ++g_cleanupDeleteUnconfirmedTotal;
            auto& state = g_lifecycle[ticket.slot];
            if (state.generationValid &&
                state.generation == ticket.generation) {
                // A deleting destructor is expected to free its pool slot. If
                // that invariant ever fails, never invoke a destructor twice on
                // the same possibly-partially-destroyed generation.
                state.cleanupSuppressed = true;
                state.retryAfterMs = std::numeric_limits<std::uint32_t>::max();
            }
        } else {
            ++g_cleanupDeletedTotal;
        }
        return;
    }

    if (outcome == CleanupOutcome::Stale) {
        ++g_cleanupStaleTotal;
    } else {
        ++g_cleanupGuardedTotal;
    }
    auto& state = g_lifecycle[ticket.slot];
    if (state.generationValid && state.generation == ticket.generation) {
        state.retryAfterMs = nowMs +
            (outcome == CleanupOutcome::Stale
                 ? kCleanupStaleRetryMs : kCleanupGuardRetryMs);
    }
}

void PublishDirectorDecision(int baseTarget, int effectiveTarget,
                             std::uint32_t wantedLevel) {
    g_directorBaseTarget.store(baseTarget, std::memory_order_relaxed);
    g_directorEffectiveTarget.store(effectiveTarget,
                                    std::memory_order_relaxed);
    g_directorWantedLevel.store(wantedLevel, std::memory_order_relaxed);
}

Snapshot GetSnapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Snapshot result = g_snapshot;
    result.directorBaseTarget =
        g_directorBaseTarget.load(std::memory_order_relaxed);
    result.directorEffectiveTarget =
        g_directorEffectiveTarget.load(std::memory_order_relaxed);
    result.directorWantedLevel =
        g_directorWantedLevel.load(std::memory_order_relaxed);
    result.cleanupCandidatePending = g_cleanupCandidate.valid &&
        g_cleanupCandidate.entity != nullptr &&
        g_cleanupCandidateEntity.load(std::memory_order_acquire) ==
            g_cleanupCandidate.entity;
    result.cleanupCandidateKind = result.cleanupCandidatePending
        ? g_cleanupCandidate.kind : CleanupKind::None;
    result.cleanupCandidateAgeMs = result.cleanupCandidatePending
        ? g_cleanupCandidate.terminalAgeMs : 0U;
    result.cleanupCandidateLastRenderedAgeMs = result.cleanupCandidatePending
        ? g_cleanupCandidate.lastRenderedAgeMs : 0U;
    result.cleanupCandidateDistanceM = result.cleanupCandidatePending
        ? g_cleanupCandidate.distanceM : 0.0F;
    result.cleanupCandidateRenderWitnessBacked =
        result.cleanupCandidatePending &&
        g_cleanupCandidate.renderWitnessBacked;
    result.cleanupRenderMarksTotal = g_renderMarksTotal;
    result.cleanupAtomicMarksTotal = g_atomicRenderMarksTotal;
    result.cleanupAtomicProbeAttemptsTotal = g_atomicProbeAttemptsTotal;
    result.cleanupAtomicProbeNoAtomicTotal = g_atomicProbeNoAtomicTotal;
    result.cleanupAtomicProbePipelineFailTotal =
        g_atomicProbePipelineFailTotal;
    result.cleanupAtomicProbeClumpMismatchTotal =
        g_atomicProbeClumpMismatchTotal;
    result.cleanupSelectionsTotal = g_cleanupSelectionsTotal;
    result.cleanupClaimsTotal = g_cleanupClaimsTotal;
    result.cleanupGuardedTotal = g_cleanupGuardedTotal;
    result.cleanupStaleTotal = g_cleanupStaleTotal;
    result.cleanupDeletedTotal = g_cleanupDeletedTotal;
    result.cleanupDeleteUnconfirmedTotal =
        g_cleanupDeleteUnconfirmedTotal;
    return result;
}

}  // namespace savr::traffic_census
