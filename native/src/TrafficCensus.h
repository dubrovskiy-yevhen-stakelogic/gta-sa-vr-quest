#pragma once

#include <cstdint>

namespace savr::traffic_census {

struct Viewpoint {
    // Physical render-camera origin. Cleanup and render-witness guards stay
    // camera-relative even when road demand follows a fast player vehicle.
    float x{};
    float y{};
    float z{};
    // Gameplay/road-demand heading. At speed this may be the player vehicle's
    // velocity rather than the independently rotating HMD/chase camera.
    float forwardX{};
    float forwardY{};
    // Cleanup needs the physical headset heading so a turned head cannot
    // inherit the mono game camera's visibility cone.
    float headsetForwardX{};
    float headsetForwardY{};
    float headsetForwardZ{};
    bool valid{};
    bool headsetValid{};
    // Separate road-interest origin. Keeping it distinct prevents a forced
    // chase camera (Rhino/third-person) from moving the traffic corridor away
    // from the player vehicle while preserving camera-relative cleanup.
    float interestX{};
    float interestY{};
    float interestZ{};
    bool interestValid{};
};

struct VehicleIdentity {
    std::uint16_t slot{};
    std::uint8_t generation{};
    bool valid{};
};

// Generation-local interpretation of GTA's overloaded +0x658 deadline. A
// three-second CAutomobile::Render heartbeat may be replaced by the headset
// witness, while longer CSetPiece retention remains authoritative.
struct RenderWitnessStatus {
    bool available{};
    bool stamped{};
    bool recent{};
    bool explicitNeeded{};
    std::uint32_t ageMs{};
    // Diagnostic-only second stage. `stamped/recent/ageMs` remain the
    // authoritative RenderOneNonRoad-entry witness used by cleanup; these
    // fields report whether that same generation reached the final common
    // AtomicDefaultRender CPU submission callback.
    bool atomicAvailable{};
    bool atomicStamped{};
    bool atomicRecent{};
    std::uint32_t atomicAgeMs{};
};

enum class CleanupKind : std::uint8_t {
    None = 0,
    ResponseRoad = 1,
    RetiredHelicopter = 2,
};

// Position of a terminal cleanup generation in the physical-headset field.
// CoreCone is the existing 120-degree cleanup cone. WitnessFringe is the
// additional 60..70-degree half-angle admitted by the 140-degree render
// witness. Keeping the two bands separate exposes a cone-policy mismatch.
enum class CleanupViewZone : std::uint8_t {
    Invalid = 0,
    // Historical telemetry identifier; the active inner sphere is now 15 m.
    Near45 = 1,
    CoreCone = 2,
    WitnessFringe = 3,
    OutsideWitness = 4,
};

enum class RenderEntryZone : std::uint8_t {
    Unknown = 0,
    // Historical telemetry identifier; the active inner sphere is now 15 m.
    Near45 = 1,
    HmdCone = 2,
    FailClosed = 3,
};

// A short-lived request to publish one owning-clump atomic timestamp. It is
// issued under the existing entry-witness lock only for a terminal generation
// whose atomic sample is absent or at least 250 ms old, then revalidated after
// the entity render returns. A recent successful submission vetoes cleanup.
struct AtomicRenderProbe {
    void* entity{};
    std::uint16_t slot{};
    std::uint8_t generation{};
    bool valid{};
};

enum class AtomicProbeOutcome : std::uint8_t {
    NoAtomic = 0,
    Submitted = 1,
    PipelineFailure = 2,
    ClumpMismatch = 3,
};

// One bounded, oldest-terminal diagnostic sample. This is copied into the
// aggregate snapshot instead of emitting one log line per pooled vehicle.
struct CleanupWitnessProbe {
    std::uint16_t slot{};
    std::uint8_t generation{};
    std::int16_t modelId{-1};
    CleanupViewZone zone{CleanupViewZone::Invalid};
    RenderEntryZone entryZone{RenderEntryZone::Unknown};
    // bit0=fading, bit1=abandoned, bit2=wrecked, bit3=driverless
    std::uint8_t stateMask{};
    std::uint32_t terminalAgeMs{};
    std::uint32_t entryAgeMs{};
    std::uint32_t atomicAgeMs{};
    float distanceM{};
    float entryDistanceM{};
    bool entryStamped{};
    bool entryRecent{};
    bool atomicStamped{};
    bool atomicRecent{};
    bool suspicious{};
    bool valid{};
};

// A cleanup ticket is valid only for one occupied generation of one retail
// vehicle-pool slot.  Callers must claim it, run their safety guards, and then
// revalidate it immediately before CWorld::Remove.
struct CleanupTicket {
    void* entity{};
    std::uint16_t slot{};
    std::uint8_t generation{};
    CleanupKind kind{CleanupKind::None};
    std::int16_t modelId{-1};
    std::uint32_t terminalAgeMs{};
    std::uint32_t lastRenderedAgeMs{};
    float distanceM{};
    bool renderWitnessBacked{};
    bool valid{};
};

enum class CleanupOutcome : std::uint8_t {
    Guarded = 0,
    Stale = 1,
    Deleted = 2,
};

// A read-only census of the retail 2.11 vehicle pool. Counts describe the most
// recent successful scan; callers must check valid before consuming them.
struct Snapshot {
    std::uint64_t revision{};
    std::uint64_t frameSerial{};
    std::uint64_t samples{};
    bool configured{};
    bool valid{};

    std::uint32_t live{};
    std::uint32_t random{};
    std::uint32_t mission{};
    std::uint32_t parked{};
    std::uint32_t permanent{};
    std::uint32_t unknown{};
    std::uint32_t law{};

    // Functional traffic classes. The stock created-by counters above overlap:
    // a law-enforcer may also be RANDOM_VEHICLE, and retail can later clear the
    // law bit while leaving the response model alive as RANDOM_VEHICLE.
    // `roadRandom` excludes generation-stamped response provenance and therefore
    // means raw ambient road civilians, which is the only count the civilian
    // director may consume. The state aggregates may overlap and explain why a
    // raw occupant is not useful traffic.
    std::uint32_t road{};
    std::uint32_t roadRandom{};
    std::uint32_t roadLaw{};
    std::uint32_t roadLawEffective{};
    std::uint32_t roadRandomEffective{};
    std::uint32_t roadRandomMoving{};
    std::uint32_t roadRandomDriverless{};
    std::uint32_t roadRandomFading{};
    std::uint32_t roadRandomAbandoned{};
    std::uint32_t roadRandomWrecked{};
    std::uint32_t roadRandomTerminal{};
    std::uint32_t roadResponse{};
    std::uint32_t roadResponseEffective{};
    std::uint32_t roadResponseDriverless{};
    std::uint32_t roadResponseFading{};
    std::uint32_t roadResponseAbandoned{};
    std::uint32_t roadResponseWrecked{};
    std::uint32_t roadResponseTerminal{};
    std::uint32_t roadPoliceModel{};
    std::uint32_t roadPoliceModelLaw{};
    std::uint32_t roadPoliceModelAmbient{};
    std::uint32_t roadCopBike{};
    std::uint32_t roadCopBikeLaw{};
    std::uint32_t roadCopBikeAmbient{};
    std::uint32_t aircraft{};
    std::uint32_t helicopters{};
    std::uint32_t nonRoadLaw{};

    // Bounded terminal-response scavenger.  It deliberately excludes ordinary
    // civilian traffic and acts only when more than eight supported terminal
    // response generations remain in the physical pool.
    std::uint32_t cleanupTerminal{};
    std::uint32_t cleanupRetiredHelicopters{};
    std::uint32_t cleanupEligible{};
    // Mutually exclusive first-failure buckets. Their sum plus cleanupEligible
    // is cleanupTerminal, making an `eligible=0` capture actionable without
    // per-vehicle spam.
    std::uint32_t cleanupBlockedYoung{};
    std::uint32_t cleanupBlockedExplicitNeeded{};
    std::uint32_t cleanupBlockedRecentRender{};
    std::uint32_t cleanupBlockedRetry{};
    std::uint32_t cleanupBlockedSuppressed{};
    std::uint32_t cleanupBlockedViewInvalid{};
    std::uint32_t cleanupBlockedNear{};
    std::uint32_t cleanupBlockedConeFallback{};
    std::uint32_t cleanupBlockedRetiredDistance{};
    bool cleanupOverBudget{};
    bool cleanupCandidatePending{};
    CleanupKind cleanupCandidateKind{CleanupKind::None};
    std::uint32_t cleanupCandidateAgeMs{};
    std::uint32_t cleanupCandidateLastRenderedAgeMs{};
    float cleanupCandidateDistanceM{};
    bool cleanupCandidateRenderWitnessBacked{};
    bool cleanupRenderWitnessAvailable{};
    std::uint32_t cleanupRenderStamped{};
    std::uint64_t cleanupRenderMarksTotal{};
    bool cleanupAtomicWitnessAvailable{};
    std::uint32_t cleanupAtomicStamped{};
    std::uint64_t cleanupAtomicMarksTotal{};
    std::uint64_t cleanupAtomicProbeAttemptsTotal{};
    std::uint64_t cleanupAtomicProbeNoAtomicTotal{};
    std::uint64_t cleanupAtomicProbePipelineFailTotal{};
    std::uint64_t cleanupAtomicProbeClumpMismatchTotal{};
    // All supported terminal generations, split by physical-headset zone.
    // These five buckets sum to cleanupTerminal.
    std::uint32_t cleanupTerminalZoneInvalid{};
    std::uint32_t cleanupTerminalZoneNear45{};
    std::uint32_t cleanupTerminalZoneCoreCone{};
    std::uint32_t cleanupTerminalZoneWitnessFringe{};
    std::uint32_t cleanupTerminalZoneOutsideWitness{};
    // Diagnostic split of cleanupBlockedRecentRender. `atomicRecent` plus
    // `entryOnly` plus `atomicUnavailable` equals the entry-stage recent
    // blocker count. `entryOnly` is further split into never/stale atomic
    // submission.
    std::uint32_t cleanupRecentAtomicRecent{};
    std::uint32_t cleanupRecentEntryOnly{};
    std::uint32_t cleanupRecentAtomicUnavailable{};
    std::uint32_t cleanupRecentAtomicNever{};
    std::uint32_t cleanupRecentAtomicStale{};
    CleanupWitnessProbe cleanupWitnessProbe{};
    std::uint64_t cleanupSelectionsTotal{};
    std::uint64_t cleanupClaimsTotal{};
    std::uint64_t cleanupGuardedTotal{};
    std::uint64_t cleanupStaleTotal{};
    std::uint64_t cleanupDeletedTotal{};
    std::uint64_t cleanupDeleteUnconfirmedTotal{};

    // Random civilian vehicles that can currently contribute to the player's
    // view. `useful` is the union of the omnidirectional near circle and the
    // forward road corridor, so a vehicle is never counted twice.
    bool localViewValid{};
    std::uint32_t localRandomNear{};
    std::uint32_t localRandomForward{};
    std::uint32_t localRandomUseful{};
    std::uint32_t localLawUseful{};
    std::uint32_t localRoadRandomNear{};
    std::uint32_t localRoadRandomForward{};
    std::uint32_t localRoadRandomUseful{};
    std::uint32_t localRoadLawUseful{};
    std::uint32_t localRoadRandomEffectiveNear{};
    std::uint32_t localRoadRandomEffectiveForward{};
    std::uint32_t localRoadRandomEffectiveUseful{};

    int directorBaseTarget{};
    int directorEffectiveTarget{};
    std::uint32_t directorWantedLevel{};

    // A generation change can contain a complete lifetime between samples.
    // Totals start at zero when a baseline is established; current occupants
    // are not reported as births merely because the census started late.
    std::uint64_t birthsTotal{};
    std::uint64_t deathsTotal{};
    std::uint64_t churnTotal{};
    std::uint32_t birthsDelta{};
    std::uint32_t deathsDelta{};
    std::uint32_t churnDelta{};
};

// Accepts the address of CPools::ms_pVehiclePool. The configuration is enabled
// only when that address is exactly loadBase + the retail 2.11 symbol offset;
// a mismatch clears the census and returns false.
bool Configure(void** poolGlobal, std::uintptr_t loadBase);

// Clears the baseline, totals, and cadence while retaining a successfully
// fingerprinted Configure() binding. The next call samples immediately.
void Reset();

// GameThread-only producer. Duplicate frame serials are ignored and the pool is
// scanned at most once per 16 distinct game frames.
void SampleIfDue(std::uint64_t frameSerial, std::uint32_t nowMs,
                 const Viewpoint* viewpoint);

// GameThread lifecycle helpers.  Every operation validates the exact retail
// pool, slot address and seven-bit generation; no long-lived decision is keyed
// by a reusable object address alone.
bool GetVehicleIdentity(const void* vehicle, VehicleIdentity* identity);
bool MarkRetiredHelicopter(void* vehicle, std::uint32_t nowMs);

// Guarded first-eye entity submission witness, modelled after the Vice City VR
// director. The caller filters SA's mono-camera render-list leftovers against
// the physical HMD field. The exact pool generation owns the timestamp, so slot
// reuse cannot make a fresh vehicle look old. A stamped generation uses the
// exact grace; an unstamped one retains the hard-radius/HMD-cone fallback.
void SetRenderWitnessAvailable(bool available);
bool IsRenderWitnessAvailable();
void SetAtomicRenderWitnessAvailable(bool available);
bool IsAtomicRenderWitnessAvailable();
// Called only for a pre-render deadline whose signed future exceeds GTA's
// exact three-second CAutomobile heartbeat. The cheap threshold remains at the
// caller so ordinary vehicles do not take a second mutex.
void ObserveVehicleExplicitNeeded(void* vehicle, std::uint32_t nowMs,
                                  std::uint32_t neededUntilMs);
void MarkVehicleRendered(void* vehicle, std::uint32_t nowMs,
                         RenderEntryZone zone, float distanceM,
                         AtomicRenderProbe* atomicProbe);
// The caller invokes this after the eye-zero entity render only if a successful,
// owning-clump AtomicDefaultRender callback was observed. The resulting recent
// timestamp is an additional cleanup veto; failures never fabricate visibility.
void CompleteVehicleAtomicProbe(const AtomicRenderProbe& probe,
                                std::uint32_t nowMs,
                                AtomicProbeOutcome outcome);
// Returns false when the exact live pool generation cannot be resolved. The
// returned status is fail-closed data for both the direct scavenger and the
// stock lifecycle wrapper; callers still own distance/cone safety.
bool GetVehicleRenderWitnessStatus(const void* vehicle, std::uint32_t nowMs,
                                   std::uint32_t graceMs,
                                   RenderWitnessStatus* status);

// The cheap predicate is lock-free and is intended for the hot stock vehicle
// loop.  Only the exact candidate enters the mutex-backed claim/revalidation.
bool IsCleanupCandidateEntity(const void* vehicle);
bool ClaimCleanupTicket(void* vehicle, std::uint32_t nowMs,
                        CleanupTicket* ticket);
bool RevalidateCleanupTicket(const CleanupTicket& ticket,
                             std::uint32_t nowMs);
void CompleteCleanupTicket(const CleanupTicket& ticket, std::uint32_t nowMs,
                           CleanupOutcome outcome);

// GameThread producer used by the generation gate. These values are folded
// into GetSnapshot without forcing another vehicle-pool scan.
void PublishDirectorDecision(int baseTarget, int effectiveTarget,
                             std::uint32_t wantedLevel);

// Thread-safe copy for telemetry/HUD consumers.
Snapshot GetSnapshot();

// Catalogue helpers shared with the lifecycle hook. They deliberately classify
// by model id instead of unverified Android CVehicle subclass offsets.
bool IsRoadVehicleModel(int modelId);
bool IsPoliceRoadVehicleModel(int modelId);
bool IsHelicopterModel(int modelId);
bool IsAircraftModel(int modelId);

}  // namespace savr::traffic_census
