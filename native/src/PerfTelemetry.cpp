#include "PerfTelemetry.h"

#include "Log.h"
#include "TrafficCensus.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace savr::perf {
namespace {

constexpr int kMaxWindowSamples = 256;
constexpr const char* kGameCsvPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/savr_game_perf_v2.csv";
constexpr const char* kXrCsvPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/savr_xr_perf_v2.csv";

struct DebugStatsAtomic {
    std::atomic<std::uint64_t> revision{0};
    std::atomic<bool> gameValid{false};
    std::atomic<double> callbackHz{0.0};
    std::atomic<double> renderedHz{0.0};
    std::atomic<double> frameWallMs{0.0};
    std::atomic<double> frameCpuMs{0.0};
    std::atomic<double> frameBlockedMs{0.0};
    std::atomic<double> renderWallMs{0.0};
    std::atomic<double> renderCpuMs{0.0};
    std::atomic<double> renderBlockedMs{0.0};
    std::atomic<double> recordWallMs{0.0};
    std::atomic<double> recordCpuMs{0.0};
    std::atomic<double> enginePreWallMs{0.0};
    std::atomic<double> enginePreCpuMs{0.0};
    std::atomic<bool> enginePreBreakdownValid{false};
    std::atomic<int> enginePreBreakdownFaults{0};
    std::atomic<double> enginePreBeforeScanWallMs{0.0};
    std::atomic<double> enginePreBeforeScanCpuMs{0.0};
    std::atomic<double> enginePreMainScanWallMs{0.0};
    std::atomic<double> enginePreMainScanCpuMs{0.0};
    std::atomic<double> enginePreStockScanWallMs{0.0};
    std::atomic<double> enginePreStockScanCpuMs{0.0};
    std::atomic<double> enginePreAfterScanWallMs{0.0};
    std::atomic<double> enginePreAfterScanCpuMs{0.0};
    std::atomic<double> enginePreBreakdownResidualWallMs{0.0};
    std::atomic<double> enginePreBreakdownResidualCpuMs{0.0};
    std::atomic<bool> renderQueueWaitHookActive{false};
    std::atomic<double> renderQueueFinishCalls{0.0};
    std::atomic<double> renderQueueFinishWallMs{0.0};
    std::atomic<double> renderQueueFinishCpuMs{0.0};
    std::atomic<double> renderQueueFinishBlockedMs{0.0};
    std::atomic<double> renderQueueFinishMaxWallMs{0.0};
    std::atomic<double> renderQueueFlushCalls{0.0};
    std::atomic<double> renderQueueFlushWallMs{0.0};
    std::atomic<double> renderQueueFlushCpuMs{0.0};
    std::atomic<double> renderQueueFlushBlockedMs{0.0};
    std::atomic<double> renderQueueFlushMaxWallMs{0.0};
    std::atomic<double> renderQueueWaitBeforeScanWallMs{0.0};
    std::atomic<double> renderQueueWaitMainScanWallMs{0.0};
    std::atomic<double> renderQueueWaitAfterScanWallMs{0.0};
    std::atomic<double> renderQueueWaitMaxUsedKiB{0.0};
    std::atomic<double> renderQueueSizedFlushCalls{0.0};
    std::atomic<double> renderQueueNearEndFlushCalls{0.0};
    std::atomic<double> renderQueueExplicitFlushCalls{0.0};
    std::atomic<int> renderQueueWaitClassificationFaults{0};
    std::atomic<bool> renderQueueFinishDeferActive{false};
    std::atomic<double> renderQueueFinishRequestCalls{0.0};
    std::atomic<double> renderQueueFinishDeferredCalls{0.0};
    std::atomic<double> renderQueueFinishDrainCalls{0.0};
    std::atomic<double> renderQueueFinishFallbackCalls{0.0};
    std::atomic<double> renderQueueFinishOverlapWallMs{0.0};
    std::atomic<double> renderQueueFinishDrainWallMs{0.0};
    std::atomic<double> renderQueueFinishDrainCpuMs{0.0};
    std::atomic<double> renderQueueFinishDrainBlockedMs{0.0};
    std::atomic<double> renderQueueFinishDrainMaxWallMs{0.0};
    std::atomic<int> renderQueueFinishPendingDepthMax{0};
    std::atomic<int> renderQueueFinishPendingFaults{0};
    std::atomic<double> stereoPrepareWallMs{0.0};
    std::atomic<double> stereoPrepareCpuMs{0.0};
    std::atomic<double> stereoTailWallMs{0.0};
    std::atomic<double> stereoTailCpuMs{0.0};
    std::atomic<double> enginePostWallMs{0.0};
    std::atomic<double> enginePostCpuMs{0.0};
    std::atomic<double> sceneLeftWallMs{0.0};
    std::atomic<double> sceneRightWallMs{0.0};
    std::atomic<double> sceneLeftCpuMs{0.0};
    std::atomic<double> sceneRightCpuMs{0.0};
    std::atomic<double> entities{0.0};
    std::atomic<double> visibleLods{0.0};
    std::atomic<double> visibleSuperLods{0.0};
    std::atomic<double> streamingRequests{0.0};
    std::atomic<double> streamingPriorityRequests{0.0};
    std::atomic<double> streamingMemoryUsedMiB{0.0};
    std::atomic<double> streamingMemoryAvailableMiB{0.0};
    std::atomic<double> lodScale{0.0};
    std::atomic<double> cameraLodMultiplier{0.0};
    std::atomic<double> cameraGenerationMultiplier{0.0};
    std::atomic<double> cameraFov{0.0};
    std::atomic<double> farClip{0.0};
    std::atomic<bool> carPopulationValid{false};
    std::atomic<double> carRandom{0.0};
    std::atomic<double> carRandomMax{0.0};
    std::atomic<double> carLaw{0.0};
    std::atomic<double> carMission{0.0};
    std::atomic<double> carParked{0.0};
    std::atomic<double> carPermanent{0.0};
    std::atomic<double> carMax{0.0};
    std::atomic<double> carDensity{0.0};
    std::atomic<bool> pedPopulationValid{false};
    std::atomic<double> pedTotal{0.0};
    std::atomic<double> pedTotalMax{0.0};
    std::atomic<double> pedCiv{0.0};
    std::atomic<double> pedGang{0.0};
    std::atomic<double> pedMission{0.0};
    std::atomic<double> pedCarPassenger{0.0};
    std::atomic<double> pedMax{0.0};
    std::atomic<double> pedDensity{0.0};
    std::atomic<bool> lodWitnessValid{false};
    std::atomic<bool> lodHandoffHookActive{false};
    std::atomic<double> linkedEntityTests{0.0};
    std::atomic<double> linkedEntityLoaded{0.0};
    std::atomic<double> linkedResultVisible{0.0};
    std::atomic<double> linkedResultCulled{0.0};
    std::atomic<double> linkedResultStream{0.0};
    std::atomic<double> linkedNearThreshold{0.0};
    std::atomic<bool> lodPrefetchActive{false};
    std::atomic<double> lodPrefetchFactor{0.0};
    std::atomic<double> prefetchBand{0.0};
    std::atomic<double> prefetchStreamMe{0.0};
    std::atomic<double> prefetchRequestCalls{0.0};
    std::atomic<double> prefetchEnqueues{0.0};
    std::atomic<double> prefetchAlreadyPending{0.0};
    std::atomic<double> prefetchThrottled{0.0};
    std::atomic<double> prefetchStateAnomalies{0.0};
    std::atomic<double> linkedNearReady{0.0};
    std::atomic<double> linkedNearMissing{0.0};
    std::atomic<int> handoffModelId{-1};
    std::atomic<double> handoffDistance{0.0};
    std::atomic<double> handoffThreshold{0.0};
    std::atomic<int> handoffResult{-1};
    std::atomic<bool> handoffLoaded{false};
    std::atomic<bool> vehicleLodWitnessValid{false};
    std::atomic<double> vehicleMultiPassM{0.0};
    std::atomic<double> vehicleLod0M{0.0};
    std::atomic<double> vehicleLod1M{0.0};
    std::atomic<double> vehicleBigLod0M{0.0};
    std::atomic<bool> vehicleLodOverrideActive{false};
    std::atomic<double> vehicleLodTargetM{0.0};
    std::atomic<bool> vehicleLodSampleHookActive{false};
    std::atomic<double> vehicleSampleCalls{0.0};
    std::atomic<bool> vehicleNearValid{false};
    std::atomic<double> vehicleNearDistance{0.0};
    std::atomic<double> vehicleNearThreshold{0.0};
    std::atomic<bool> vehicleNearHigh{false};
    std::atomic<bool> streetPropFloorActive{false};
    std::atomic<double> streetPropFloorM{0.0};
    std::atomic<double> unlinkedShortTests{0.0};
    std::atomic<double> streetPropTests{0.0};
    std::atomic<double> streetPropWrites{0.0};
    std::atomic<int> propModelId{-1};
    std::atomic<double> propDistance{0.0};
    std::atomic<double> propStockThreshold{0.0};
    std::atomic<double> propAppliedThreshold{0.0};
    std::atomic<int> propResult{-1};
    std::atomic<bool> propLoaded{false};
    std::atomic<bool> propTargeted{false};
    std::atomic<double> staticTests{0.0};
    std::atomic<double> dynamicTests{0.0};
    std::atomic<double> behindTests{0.0};
    std::atomic<bool> cullAttributionValid{false};
    std::atomic<double> dynamicMatrixCalls{0.0};
    std::atomic<double> dynamicWidenAccepts{0.0};
    std::atomic<double> dynamicFallbackVisible{0.0};
    std::atomic<double> dynamicFallbackCulled{0.0};
    std::atomic<double> dynamicWiden60To80{0.0};
    std::atomic<double> dynamicWidenBehind{0.0};
    std::atomic<double> sphere45Shortcuts{0.0};
    std::atomic<double> staticSafetyRadiusM{0.0};
    std::atomic<double> staticSafetyTests{0.0};
    std::atomic<double> staticSafetyStockVisible{0.0};
    std::atomic<double> staticSafetyAccepts{0.0};
    std::atomic<double> staticSafety45To60{0.0};
    std::atomic<double> staticSafety60To80{0.0};
    std::atomic<double> staticSafetyBuildingAccepts{0.0};
    std::atomic<double> staticSafetyDummyAccepts{0.0};
    std::atomic<bool> nearbyScanActive{false};
    std::atomic<double> nearbyScanRadiusM{0.0};
    std::atomic<double> nearbyScanSectors{0.0};
    std::atomic<double> nearbyScanVisibleAdded{0.0};
    std::atomic<double> nearbyScanWallMs{0.0};
    std::atomic<double> nearbyScanCpuMs{0.0};
    std::atomic<int> cullAttributionFaults{0};
    std::atomic<double> alphaBefore{0.0};
    std::atomic<double> alphaAfterLeft{0.0};
    std::atomic<double> alphaAfterRight{0.0};
    std::atomic<int> dedupeChecks{0};
    std::atomic<int> dedupeHits{0};
    std::atomic<int> dedupeFaults{0};
    std::atomic<int> shadowCalls{0};
    std::atomic<int> shadowEye2Skips{0};
    std::atomic<int> gateSkips{0};
    std::atomic<int> frameLimit{0};
    std::atomic<int> eyeWidth{0};
    std::atomic<int> eyeHeight{0};
    std::atomic<int> renderScalePercent{0};
    std::atomic<bool> alphaHookActive{false};
    std::atomic<bool> shadowHookActive{false};
    std::atomic<double> buildingDetailTests{0.0};
    std::atomic<double> buildingDetailOverrides{0.0};
    std::atomic<int> modelDrawRestoreFaults{0};
    std::atomic<bool> ambientCarGateActive{false};
    std::atomic<int> ambientCarTarget{0};
    std::atomic<double> ambientCarAttempts{0.0};
    std::atomic<double> ambientCarBlocked{0.0};
    std::atomic<double> ambientCarWantedPasses{0.0};
    std::atomic<int> pedAmbientCap{0};
    std::atomic<double> pedAmbientAttempts{0.0};
    std::atomic<double> pedAmbientSuccesses{0.0};
    std::atomic<double> pedAmbientBlocked{0.0};

    std::atomic<bool> presentValid{false};
    std::atomic<double> presentHz{0.0};
    std::atomic<bool> displayValid{false};
    std::atomic<double> displayHz{0.0};
    std::atomic<double> budgetMs{0.0};
    std::atomic<bool> runtimeCpuValid{false};
    std::atomic<double> runtimeCpuMs{0.0};
    std::atomic<bool> runtimeGpuValid{false};
    std::atomic<double> runtimeGpuMs{0.0};
    std::atomic<int> endFailures{0};
    std::atomic<int> ringRaces{0};
    std::atomic<int> fresh{0};
    std::atomic<int> repeated{0};
    std::atomic<int> stereoSyncWaits{0};
    std::atomic<int> stereoSyncRescued{0};
    std::atomic<int> stereoSyncTimeouts{0};
    std::atomic<double> stereoSyncWaitMs{0.0};
    std::atomic<bool> fxaaRequested{false};
    std::atomic<bool> fxaaActive{false};
    std::atomic<int> fxaaDraws{0};
    std::atomic<int> fxaaFallbacks{0};
    std::atomic<int> fxaaErrors{0};
    std::atomic<double> fxaaSubmitWallMs{0.0};

    std::atomic<bool> limiterActive{false};
    std::atomic<bool> limiterRxRestored{false};
    std::atomic<double> outerPacerHz{0.0};
};

DebugStatsAtomic g_debugStats{};
std::mutex g_debugStatsMutex;

// Preserve the just-finished process' canonical summary before the new process
// opens the same path with "w". This runs at most once per stream/process and
// does not add any per-frame I/O.
bool ArchivePreviousCsv(const char* canonicalPath, const char* streamName) {
    struct stat canonicalInfo{};
    if (::stat(canonicalPath, &canonicalInfo) != 0) {
        if (errno != ENOENT) {
            LOGW("[perf.init] cannot stat previous %s CSV %s: %s",
                 streamName, canonicalPath, std::strerror(errno));
            return false;
        }
        return true;
    }

    timespec wall{};
    if (::clock_gettime(CLOCK_REALTIME, &wall) != 0) {
        wall.tv_sec = std::time(nullptr);
        wall.tv_nsec = 0;
    }

    const char* extension = std::strrchr(canonicalPath, '.');
    const int stemLength = extension
        ? static_cast<int>(extension - canonicalPath)
        : static_cast<int>(std::strlen(canonicalPath));
    if (!extension) extension = "";

    char archivePath[512]{};
    bool foundUniquePath = false;
    for (int collision = 0; collision < 100; ++collision) {
        if (collision == 0) {
            std::snprintf(archivePath, sizeof(archivePath),
                          "%.*s.%lld_%03ld.pid%d%s",
                          stemLength, canonicalPath,
                          static_cast<long long>(wall.tv_sec),
                          wall.tv_nsec / 1000000L,
                          static_cast<int>(::getpid()), extension);
        } else {
            std::snprintf(archivePath, sizeof(archivePath),
                          "%.*s.%lld_%03ld.pid%d.%d%s",
                          stemLength, canonicalPath,
                          static_cast<long long>(wall.tv_sec),
                          wall.tv_nsec / 1000000L,
                          static_cast<int>(::getpid()), collision, extension);
        }

        struct stat archiveInfo{};
        if (::stat(archivePath, &archiveInfo) != 0 && errno == ENOENT) {
            foundUniquePath = true;
            break;
        }
    }

    if (!foundUniquePath) {
        LOGW("[perf.init] no unique archive name for previous %s CSV", streamName);
        return false;
    }

    if (std::rename(canonicalPath, archivePath) == 0) {
        LOGI("[perf.init] archived previous %s CSV: %s", streamName, archivePath);
        return true;
    } else {
        LOGW("[perf.init] cannot archive previous %s CSV %s: %s",
             streamName, canonicalPath, std::strerror(errno));
        return false;
    }
}

struct Series {
    std::array<double, kMaxWindowSamples> values{};
    int count{};
    double sum{};
    double maximum{};

    void Add(double value) {
        if (!std::isfinite(value) || value < 0.0) return;
        maximum = std::max(maximum, value);
        if (count >= static_cast<int>(values.size())) return;
        values[count++] = value;
        sum += value;
    }

    double Average() const { return count ? sum / static_cast<double>(count) : 0.0; }

    double P95() const {
        if (count == 0) return 0.0;
        auto copy = values;
        std::sort(copy.begin(), copy.begin() + count);
        const int index = std::clamp(
            static_cast<int>(std::ceil(static_cast<double>(count) * 0.95)) - 1,
            0, count - 1);
        return copy[index];
    }
};

StereoFrameSample g_latestStereo{};
bool g_haveStereo = false;
int g_lastGameStereoSequence = -1;
double g_lastRenderedGameMs = 0.0;
std::atomic<std::uint64_t> g_renderSceneSerial{0};

struct GameWindow {
    double startMs{};
    int callbacks{};
    int renderedCallbacks{};
    int limiterSkips{};
    int freshStereoFrames{};
    int sequenceJumps{};
    int renderSceneCalls{};
    int wrapperSwapCalls{};
    int wrapperSwapFailures{};
    Series callbackPeriod;
    Series callbackWork;
    Series javaDelta;
    Series renderGap;
    Series renderWall;
    Series renderCpu;
    Series renderBlocked;
    Series skipWall;
    Series skipCpu;
    Series skipBlocked;
    Series swapWall;
    Series renderSleepRequested;
    Series renderSleepActual;
    Series skipSleepRequested;
    Series skipSleepActual;
    Series recordWall;
    Series recordCpu;
    Series sceneLeft;
    Series sceneRight;
    Series sceneLeftCpu;
    Series sceneRightCpu;
    Series alphaNodesBefore;
    Series alphaNodesAfterLeft;
    Series alphaNodesAfterRight;
    int alphaDedupeChecks{};
    int alphaDedupeHits{};
    int alphaDedupeFaults{};
    int shadowUpdateCalls{};
    int shadowUpdateSecondEyeSkips{};
    Series skyLeft;
    Series skyRight;
    Series cameraEnd;
    Series engineOutsideRecord;
    Series enginePreWall;
    Series enginePreCpu;
    Series stereoPrepareWall;
    Series stereoPrepareCpu;
    Series stereoTailWall;
    Series stereoTailCpu;
    Series enginePostWall;
    Series enginePostCpu;
    Series phaseWallError;
    Series phaseCpuError;
    int phaseValidFrames{};
    int phaseFaults{};
    Series enginePreBeforeScanWall;
    Series enginePreBeforeScanCpu;
    Series enginePreMainScanWall;
    Series enginePreMainScanCpu;
    Series enginePreStockScanWall;
    Series enginePreStockScanCpu;
    Series enginePreAfterScanWall;
    Series enginePreAfterScanCpu;
    Series enginePreBreakdownResidualWall;
    Series enginePreBreakdownResidualCpu;
    int enginePreBreakdownValidFrames{};
    int enginePreBreakdownFaults{};
    Series renderQueueFinishCalls;
    Series renderQueueFinishWall;
    Series renderQueueFinishCpu;
    Series renderQueueFinishMaxWall;
    Series renderQueueFlushCalls;
    Series renderQueueFlushWall;
    Series renderQueueFlushCpu;
    Series renderQueueFlushMaxWall;
    Series renderQueueSizedFlushCalls;
    Series renderQueueNearEndFlushCalls;
    Series renderQueueExplicitFlushCalls;
    Series renderQueueWaitBeforeScanCalls;
    Series renderQueueWaitBeforeScanWall;
    Series renderQueueWaitBeforeScanCpu;
    Series renderQueueWaitMainScanCalls;
    Series renderQueueWaitMainScanWall;
    Series renderQueueWaitMainScanCpu;
    Series renderQueueWaitAfterScanCalls;
    Series renderQueueWaitAfterScanWall;
    Series renderQueueWaitAfterScanCpu;
    Series renderQueueWaitMaxUsedKiB;
    int renderQueueWaitClassificationFaults{};
    Series renderQueueFinishRequestCalls;
    Series renderQueueFinishDeferredCalls;
    Series renderQueueFinishDrainCalls;
    Series renderQueueFinishFallbackCalls;
    Series renderQueueFinishOverlapWall;
    Series renderQueueFinishDrainWall;
    Series renderQueueFinishDrainCpu;
    Series renderQueueFinishDrainMaxWall;
    Series renderQueueFinishPendingDepthMax;
    int renderQueueFinishPendingFaults{};
    Series visibleEntities;
    Series visibleLods;
    Series visibleSuperLods;
    Series streamingRequests;
    Series streamingPriorityRequests;
    Series streamingMemoryUsedMiB;
    Series streamingMemoryAvailableMiB;
    Series lodScale;
    Series cameraLodMultiplier;
    Series cameraGenerationMultiplier;
    Series cameraFov;
    Series farClip;
    Series carRandom;
    Series carLaw;
    Series carMission;
    Series carParked;
    Series carPermanent;
    Series carMax;
    Series carDensity;
    Series pedTotal;
    Series pedCiv;
    Series pedGang;
    Series pedMission;
    Series pedCarPassenger;
    Series pedMax;
    Series pedDensity;
    Series linkedEntityTests;
    Series linkedEntityLoaded;
    Series linkedResultVisible;
    Series linkedResultCulled;
    Series linkedResultStream;
    Series linkedNearThreshold;
    Series prefetchBand;
    Series prefetchStreamMe;
    Series prefetchRequestCalls;
    Series prefetchEnqueues;
    Series prefetchAlreadyPending;
    Series prefetchThrottled;
    Series prefetchStateAnomalies;
    Series linkedNearReady;
    Series linkedNearMissing;
    bool handoffCandidateValid{};
    int handoffModelId{-1};
    double handoffDistance{};
    double handoffThreshold{};
    double handoffAbsDelta{};
    int handoffResult{-1};
    bool handoffLoaded{};
    Series vehicleMultiPassM;
    Series vehicleLod0M;
    Series vehicleLod1M;
    Series vehicleBigLod0M;
    Series vehicleSampleCalls;
    bool vehicleCandidateValid{};
    double vehicleNearDistance{};
    double vehicleNearThreshold{};
    double vehicleNearAbsDelta{};
    bool vehicleNearHigh{};
    Series unlinkedShortTests;
    Series streetPropTests;
    Series streetPropWrites;
    bool propCandidateValid{};
    int propModelId{-1};
    double propDistance{};
    double propStockThreshold{};
    double propAppliedThreshold{};
    double propAbsDelta{};
    int propResult{-1};
    bool propLoaded{};
    bool propTargeted{};
    Series staticTests;
    Series dynamicTests;
    Series behindTests;
    Series dynamicMatrixCalls;
    Series dynamicWidenAccepts;
    Series dynamicFallbackVisible;
    Series dynamicFallbackCulled;
    Series dynamicWiden60To80;
    Series dynamicWidenBehind;
    Series sphere45Shortcuts;
    Series staticSafetyTests;
    Series staticSafetyStockVisible;
    Series staticSafetyAccepts;
    Series staticSafety45To60;
    Series staticSafety60To80;
    Series staticSafetyBuildingAccepts;
    Series staticSafetyDummyAccepts;
    Series staticSafetyConeRejects;
    Series largeVegetationSafetyTests;
    Series largeVegetationSafetyStockVisible;
    Series largeVegetationSafetyAccepts;
    Series largeVegetationSafetyConeRejects;
    Series roadsideSafetyTests;
    Series roadsideSafetyStockVisible;
    Series roadsideSafetyAccepts;
    Series roadsideSafetyConeRejects;
    Series targetedOcclusionBypasses;
    Series targetedOcclusionTests;
    Series targetedOcclusionStockVisible;
    Series targetedOcclusionStockOccluded;
    Series targetedOcclusionConeBypasses;
    Series targetedOcclusionRadialBypasses;
    Series targetedOcclusionRecentBypasses;
    Series targetedOcclusionStockCulls;
    Series targetedOcclusionInvalidPoseBypasses;
    Series targetedOcclusionCacheHits;
    Series targetedOcclusionCacheMisses;
    Series targetedOcclusionCacheEvictions;
    Series targetedOcclusionCacheOverflows;
    Series targetedOcclusionIdentityResets;
    Series nearbyScanSectors;
    Series nearbyScanVisibleAdded;
    Series nearbyScanStreamingRequests;
    Series nearbyScanConeRejectedSectors;
    Series nearbyScanWallMs;
    Series nearbyScanCpuMs;
    int aircraftLodActiveFrames{};
    Series aircraftAltitudeM;
    Series aircraftOrdinaryMaxSlantM;
    double aircraftHeadForwardZSum{};
    int aircraftHeadForwardZCount{};
    Series aircraftOrdinaryProbes;
    Series aircraftOrdinaryQualified;
    Series aircraftOrdinaryForcedVisible;
    Series aircraftOrdinaryRangePromotions;
    Series aircraftOrdinaryHorizontalRejects;
    Series aircraftOrdinarySlantRejects;
    Series aircraftOrdinaryConeRejects;
    Series aircraftOrdinaryForceLimit;
    Series aircraftOrdinaryCapacity;
    Series aircraftOrdinaryOcclusionBypasses;
    Series aircraftOrdinaryOcclusionUnconsumed;
    Series aircraftOrdinaryInnerSlantRejects;
    Series aircraftOrdinaryUnloadedRootlessForward;
    Series aircraftOrdinaryPrefetchCandidates;
    Series aircraftOrdinaryPrefetchUnique;
    Series aircraftOrdinaryPrefetchRequestCalls;
    Series aircraftOrdinaryPrefetchEnqueues;
    Series aircraftOrdinaryPrefetchAlreadyPending;
    Series aircraftOrdinaryPrefetchQueueBlocked;
    Series aircraftOrdinaryPrefetchBudgetLimited;
    Series aircraftOrdinaryPrefetchStateAnomalies;
    Series aircraftOrdinaryOuterActive;
    Series aircraftOrdinaryOuterRadiusM;
    Series aircraftOrdinaryOuterCandidateSectors;
    Series aircraftOrdinaryOuterScannedSectors;
    Series aircraftOrdinaryOuterConeSkippedSectors;
    Series aircraftOrdinaryOuterSectorLimit;
    Series aircraftOrdinaryOuterProbes;
    Series aircraftOrdinaryOuterQualified;
    Series aircraftOrdinaryOuterUnloaded;
    Series aircraftOrdinaryOuterAdmitted;
    Series aircraftOrdinaryOuterForceStops;
    Series aircraftOrdinaryOuterFaults;
    Series aircraftOrdinaryOuterWallMs;
    Series aircraftOrdinaryOuterCpuMs;
    Series aircraftOrdinaryOuterPhase1InnerPromotions;
    Series aircraftOrdinaryOuterPhase1OuterPromotions;
    Series aircraftOrdinaryOuterPhase1InnerAdmissions;
    Series aircraftOrdinaryOuterPhase1OuterAdmissions;
    Series aircraftOrdinaryOuterPhase1InnerPromotionStops;
    Series aircraftOrdinaryOuterPhase1OuterPromotionStops;
    Series aircraftOrdinaryOuterPhase1InnerAdmissionStops;
    Series aircraftOrdinaryOuterPhase1OuterAdmissionStops;
    Series aircraftOrdinaryPhase2BackfillAdmissions;
    Series aircraftOrdinaryInnerLimit;
    Series aircraftOrdinaryOuterLimit;
    Series aircraftOrdinaryOuterNearLimit;
    Series aircraftOrdinaryOuterFarLimit;
    Series aircraftOrdinaryTotalLimit;
    Series aircraftOrdinaryOuterNearPromotions;
    Series aircraftOrdinaryOuterFarPromotions;
    Series aircraftOrdinaryOuterNearAdmissions;
    Series aircraftOrdinaryOuterFarAdmissions;
    Series aircraftOrdinaryOuterNearPromotionStops;
    Series aircraftOrdinaryOuterFarPromotionStops;
    Series aircraftOrdinaryOuterNearAdmissionStops;
    Series aircraftOrdinaryOuterFarAdmissionStops;
    Series aircraftOrdinaryOuterSessionDisabled;
    Series aircraftOrdinaryOuterRadialCandidates;
    Series aircraftOrdinaryOuterRadialInnerCandidates;
    Series aircraftOrdinaryOuterRadialOuterCandidates;
    Series aircraftOrdinaryOuterRadialSelectedInner;
    Series aircraftOrdinaryOuterRadialSelectedOuter;
    Series aircraftOrdinaryOuterRadialRetainedInner;
    Series aircraftOrdinaryOuterRadialRetainedOuter;
    Series aircraftOrdinaryOuterRadialReplayRequested;
    Series aircraftOrdinaryOuterRadialReplayVisited;
    Series aircraftOrdinaryOuterRadialReplayCompleted;
    Series aircraftOrdinaryOuterRadialReplayMisses;
    Series aircraftOrdinaryOuterRadialReplaySectors;
    Series aircraftOrdinaryOuterRadialReplayVisible;
    Series aircraftOrdinaryOuterRadialReplayCulled;
    Series aircraftOrdinaryOuterRadialReplayStream;
    Series aircraftOrdinaryOuterRadialReplayOther;
    Series aircraftOrdinaryOuterRadialCaptureOverflow;
    Series aircraftOrdinaryOuterRadialSectorOverflow;
    int cullAttributionFaults{};
    Series buildingDetailTests;
    Series buildingDetailOverrides;
    int modelDrawRestoreFaults{};
    Series ambientCarAttempts;
    Series ambientCarBlocked;
    Series ambientCarWantedPasses;
    Series pedAmbientAttempts;
    Series pedAmbientSuccesses;
    Series pedAmbientBlocked;
    double totalThreadCpuMs{};
    GameFrameSample last{};
    StereoFrameSample lastStereo{};
};

GameWindow g_gameWindow{};
FILE* g_gameCsv = nullptr;
bool g_gameCsvAttempted = false;
std::array<char, 64 * 1024> g_gameCsvBuffer{};

FILE* OpenGameCsv() {
    if (g_gameCsv || g_gameCsvAttempted) return g_gameCsv;
    g_gameCsvAttempted = true;
    if (!ArchivePreviousCsv(kGameCsvPath, "game")) {
        LOGW("[perf.init] preserving previous game CSV; current capture disabled");
        return nullptr;
    }
    g_gameCsv = std::fopen(kGameCsvPath, "w");
    if (!g_gameCsv) {
        LOGW("[perf.init] cannot open %s", kGameCsvPath);
        return nullptr;
    }
    std::setvbuf(g_gameCsv, g_gameCsvBuffer.data(), _IOFBF, g_gameCsvBuffer.size());
    std::fprintf(g_gameCsv,
        "window_s,callbacks,callback_hz,rendered,rendered_hz,gate_skips,gate_skip_pct,"
        "stereo_fresh,seq_jumps,render_scene_calls,"
        "cap_fps,gta_fps,timestep,timestep_nc,frame_counter,skip_process,skip_frame,cpu_core,stereo_active,"
        "period_avg,period_p95,period_max,callback_work_avg,callback_work_p95,callback_work_max,"
        "java_delta_ms_avg,render_gap_avg,render_gap_p95,render_gap_max,"
        "render_wall_avg,render_wall_p95,render_wall_max,render_cpu_avg,render_blocked_avg,"
        "skip_wall_avg,skip_wall_p95,skip_wall_max,skip_cpu_avg,skip_blocked_avg,"
        "wrapper_swap_calls,wrapper_swap_failures,swap_avg,swap_max,"
        "sleep_render_requested_avg,sleep_render_actual_avg,"
        "sleep_skip_requested_avg,sleep_skip_actual_avg,total_thread_cpu_pct,"
        "record_wall_avg,record_cpu_avg,scene_left_avg,scene_right_avg,"
        "scene_left_cpu_avg,scene_right_cpu_avg,"
        "alpha_nodes_before_avg,alpha_nodes_after_left_avg,alpha_nodes_after_right_avg,"
        "alpha_dedupe_checks,alpha_dedupe_hits,alpha_dedupe_faults,"
        "shadow_update_calls,shadow_update_eye2_skips,alpha_hook_active,shadow_hook_active,"
        "sky_left_avg,sky_right_avg,camera_end_avg,engine_other_avg,"
        "phase_valid,phase_faults,engine_pre_wall_avg,engine_pre_cpu_avg,"
        "stereo_prepare_wall_avg,stereo_prepare_cpu_avg,"
        "stereo_tail_wall_avg,stereo_tail_cpu_avg,"
        "engine_post_wall_avg,engine_post_cpu_avg,"
        "phase_wall_error_avg,phase_cpu_error_avg,"
        "entities_avg,entities_max,static_tests_avg,dynamic_tests_avg,behind_tests_avg,"
        "eye_w,eye_h,render_scale_pct,"
        "dyn_matrix_calls_avg,dyn_widen_accept_avg,dyn_fallback_visible_avg,"
        "dyn_fallback_culled_avg,dyn_widen_60_80_avg,dyn_widen_behind_avg,"
        "sphere45_shortcut_avg,cull_attribution_faults,"
        "visible_lods_avg,visible_lods_max,visible_super_lods_avg,visible_super_lods_max,"
        "stream_requests_avg,stream_priority_requests_avg,"
        "stream_memory_used_mib_avg,stream_memory_available_mib_avg,"
        "lod_scale_avg,camera_lod_multiplier_avg,camera_generation_multiplier_avg,"
        "camera_fov_avg,far_clip_avg,"
        "car_pop_valid,car_random_avg,car_random_max,car_law_avg,car_mission_avg,"
        "car_parked_avg,car_permanent_avg,car_cap_avg,car_density_avg,"
        "ped_pop_valid,ped_total_avg,ped_total_max,ped_civ_avg,ped_gang_avg,"
        "ped_mission_avg,ped_passenger_avg,ped_cap_avg,ped_density_avg,"
        "lod_witness_valid,"
        "lod_link_hook_active,linked_tests_avg,linked_loaded_avg,"
        "linked_visible_avg,linked_culled_avg,linked_stream_avg,linked_near_avg,"
        "handoff_model_id,handoff_distance,handoff_threshold,handoff_result,handoff_loaded,"
        "vehicle_lod_witness_valid,vehicle_multipass_m,vehicle_lod0_m,"
        "vehicle_lod1_m,vehicle_big_lod0_m,"
        "lod_prefetch_active,lod_prefetch_factor,prefetch_band_avg,"
        "prefetch_enqueue_avg,linked_near_ready_avg,linked_near_missing_avg,"
        "prefetch_streamme_avg,prefetch_request_calls_avg,prefetch_pending_avg,"
        "prefetch_throttled_avg,prefetch_state_anomaly_avg,"
        "vehicle_lod_override_active,vehicle_lod_target_m,"
        "vehicle_lod_sample_hook_active,vehicle_sample_calls_avg,"
        "vehicle_near_valid,vehicle_near_distance,vehicle_near_threshold,vehicle_near_high,"
        "street_prop_floor_active,street_prop_floor_m,unlinked_short_tests_avg,"
        "street_prop_tests_avg,street_prop_writes_avg,prop_model_id,prop_distance,"
        "prop_stock_threshold,prop_applied_threshold,prop_result,prop_loaded,prop_targeted,"
        "vehicle_pool_valid,vehicle_pool_live,vehicle_pool_random,vehicle_pool_mission,"
        "vehicle_pool_parked,vehicle_pool_permanent,vehicle_pool_unknown,vehicle_pool_law,"
        "vehicle_pool_births_window,vehicle_pool_deaths_window,"
        "vehicle_pool_births_total,vehicle_pool_deaths_total,vehicle_pool_samples,"
        "traffic_local_view_valid,traffic_local_random_near,"
        "traffic_local_random_forward,traffic_local_random_useful,"
        "traffic_local_law_useful,traffic_director_base_target,"
        "traffic_director_effective_target,traffic_director_wanted_level,"
        "building_detail_tests_avg,building_detail_overrides_avg,"
        "model_draw_restore_faults,ambient_car_gate_active,"
        "ambient_car_target,ambient_car_attempts_avg,ambient_car_blocked_avg,"
        "ambient_car_wanted_passes_avg,ped_ambient_cap,"
        "ped_ambient_attempts_avg,ped_ambient_successes_avg,"
        "ped_ambient_blocked_avg,static_safety_radius_m,"
        "static_safety_tests_avg,static_safety_stock_visible_avg,"
        "static_safety_accept_avg,static_safety_45_60_avg,"
        "static_safety_60_80_avg,static_safety_building_accept_avg,"
        "static_safety_dummy_accept_avg,static_cull_cone_half_angle_deg,"
        "static_cull_cone_valid,static_safety_cone_reject_avg,"
        "large_veg_safety_radius_m,"
        "large_veg_safety_tests_avg,large_veg_safety_stock_visible_avg,"
        "large_veg_safety_accept_avg,large_veg_safety_cone_reject_avg,"
        "roadside_safety_radius_m,roadside_safety_tests_avg,"
        "roadside_safety_stock_visible_avg,roadside_safety_accept_avg,"
        "roadside_safety_cone_reject_avg,"
        "targeted_occlusion_hook_active,targeted_occlusion_bypass_avg,"
        "targeted_occlusion_tests_avg,targeted_occlusion_stock_visible_avg,"
        "targeted_occlusion_stock_occluded_avg,"
        "targeted_occlusion_cone_bypass_avg,targeted_occlusion_radial_bypass_avg,"
        "targeted_occlusion_recent_bypass_avg,"
        "targeted_occlusion_stock_cull_avg,"
        "targeted_occlusion_invalid_pose_bypass_avg,"
        "targeted_occlusion_cache_hit_avg,targeted_occlusion_cache_miss_avg,"
        "targeted_occlusion_cache_eviction_avg,"
        "targeted_occlusion_cache_overflow_avg,"
        "targeted_occlusion_identity_reset_avg,"
        "nearby_scan_active,"
        "nearby_scan_radius_m,nearby_scan_sectors_avg,"
        "nearby_scan_visible_added_avg,nearby_scan_hmd_heading_active,"
        "nearby_scan_streaming_requests_avg,nearby_scan_wall_ms_avg,"
        "nearby_scan_cpu_ms_avg,"
        "engine_pre_breakdown_valid,engine_pre_breakdown_frames,"
        "engine_pre_breakdown_faults,engine_pre_before_scan_wall_avg,"
        "engine_pre_before_scan_cpu_avg,engine_pre_main_scan_wall_avg,"
        "engine_pre_main_scan_cpu_avg,engine_pre_stock_scan_wall_avg,"
        "engine_pre_stock_scan_cpu_avg,engine_pre_after_scan_wall_avg,"
        "engine_pre_after_scan_cpu_avg,engine_pre_breakdown_wall_residual_avg,"
        "engine_pre_breakdown_cpu_residual_avg,rq_wait_hook_active,"
        "rq_finish_calls_avg,rq_finish_wall_avg,rq_finish_cpu_avg,"
        "rq_finish_blocked_avg,rq_finish_single_max,"
        "rq_flush_calls_avg,rq_flush_wall_avg,rq_flush_cpu_avg,"
        "rq_flush_blocked_avg,rq_flush_single_max,"
        "rq_sized_flush_calls_avg,rq_unsized_flush_calls_avg,"
        "rq_reserved_flush_calls_avg,"
        "rq_wait_before_calls_avg,rq_wait_before_wall_avg,"
        "rq_wait_before_cpu_avg,rq_wait_scan_calls_avg,"
        "rq_wait_scan_wall_avg,rq_wait_scan_cpu_avg,"
        "rq_wait_after_calls_avg,rq_wait_after_wall_avg,"
        "rq_wait_after_cpu_avg,rq_observed_high_water_kib,"
        "rq_wait_classification_faults,"
        "rq_finish_defer_active,rq_finish_request_calls_avg,"
        "rq_finish_deferred_calls_avg,rq_finish_drain_calls_avg,"
        "rq_finish_fallback_calls_avg,rq_finish_overlap_wall_avg,"
        "rq_finish_drain_wall_avg,rq_finish_drain_cpu_avg,"
        "rq_finish_drain_blocked_avg,rq_finish_drain_single_max,"
        "rq_finish_pending_depth_max,rq_finish_pending_faults,"
        "aircraft_lod_active,aircraft_altitude_m_avg,"
        "aircraft_ordinary_max_slant_m_avg,aircraft_head_forward_z_avg,"
        "aircraft_ordinary_probes_avg,aircraft_ordinary_qualified_avg,"
        "aircraft_ordinary_forced_visible_avg,"
        "aircraft_ordinary_range_promotions_avg,"
        "aircraft_ordinary_horizontal_reject_avg,"
        "aircraft_ordinary_slant_reject_avg,"
        "aircraft_ordinary_cone_reject_avg,"
        "aircraft_ordinary_force_limit_avg,aircraft_ordinary_capacity_avg,"
        "aircraft_ordinary_occlusion_bypass_avg,"
        "aircraft_ordinary_occlusion_unconsumed_avg,"
        "aircraft_ordinary_inner_slant_reject_avg,"
        "aircraft_ordinary_unloaded_rootless_forward_avg,"
        "aircraft_ordinary_prefetch_candidates_avg,"
        "aircraft_ordinary_prefetch_unique_avg,"
        "aircraft_ordinary_prefetch_request_calls_avg,"
        "aircraft_ordinary_prefetch_enqueues_avg,"
        "aircraft_ordinary_prefetch_already_pending_avg,"
        "aircraft_ordinary_prefetch_queue_blocked_avg,"
        "aircraft_ordinary_prefetch_budget_limited_avg,"
        "aircraft_ordinary_prefetch_state_anomalies_avg,"
        "aircraft_ordinary_outer_active_avg,"
        "aircraft_ordinary_outer_radius_m_avg,"
        "aircraft_ordinary_outer_candidate_sectors_avg,"
        "aircraft_ordinary_outer_scanned_sectors_avg,"
        "aircraft_ordinary_outer_cone_skipped_sectors_avg,"
        "aircraft_ordinary_outer_sector_limit_avg,"
        "aircraft_ordinary_outer_probes_avg,"
        "aircraft_ordinary_outer_qualified_avg,"
        "aircraft_ordinary_outer_unloaded_avg,"
        "aircraft_ordinary_outer_admitted_avg,"
        "aircraft_ordinary_outer_force_stops_avg,"
        "aircraft_ordinary_outer_faults_avg,"
        "aircraft_ordinary_outer_wall_ms_avg,"
        "aircraft_ordinary_outer_cpu_ms_avg,"
        "aircraft_ordinary_outer_phase1_inner_promotions_avg,"
        "aircraft_ordinary_outer_phase1_outer_promotions_avg,"
        "aircraft_ordinary_outer_phase1_inner_admissions_avg,"
        "aircraft_ordinary_outer_phase1_outer_admissions_avg,"
        "aircraft_ordinary_outer_phase1_inner_promotion_stops_avg,"
        "aircraft_ordinary_outer_phase1_outer_promotion_stops_avg,"
        "aircraft_ordinary_outer_phase1_inner_admission_stops_avg,"
        "aircraft_ordinary_outer_phase1_outer_admission_stops_avg,"
        "aircraft_ordinary_phase2_backfill_admissions_avg,"
        "aircraft_ordinary_inner_limit_avg,"
        "aircraft_ordinary_outer_limit_avg,"
        "aircraft_ordinary_outer_near_limit_avg,"
        "aircraft_ordinary_outer_far_limit_avg,"
        "aircraft_ordinary_total_limit_avg,"
        "aircraft_ordinary_outer_near_promotions_avg,"
        "aircraft_ordinary_outer_far_promotions_avg,"
        "aircraft_ordinary_outer_near_admissions_avg,"
        "aircraft_ordinary_outer_far_admissions_avg,"
        "aircraft_ordinary_outer_near_promotion_stops_avg,"
        "aircraft_ordinary_outer_far_promotion_stops_avg,"
        "aircraft_ordinary_outer_near_admission_stops_avg,"
        "aircraft_ordinary_outer_far_admission_stops_avg,"
        "aircraft_ordinary_outer_session_disabled_avg,"
        "aircraft_ordinary_outer_radial_candidates_avg,"
        "aircraft_ordinary_outer_radial_inner_candidates_avg,"
        "aircraft_ordinary_outer_radial_outer_candidates_avg,"
        "aircraft_ordinary_outer_radial_selected_inner_avg,"
        "aircraft_ordinary_outer_radial_selected_outer_avg,"
        "aircraft_ordinary_outer_radial_retained_inner_avg,"
        "aircraft_ordinary_outer_radial_retained_outer_avg,"
        "aircraft_ordinary_outer_radial_replay_requested_avg,"
        "aircraft_ordinary_outer_radial_replay_visited_avg,"
        "aircraft_ordinary_outer_radial_replay_completed_avg,"
        "aircraft_ordinary_outer_radial_replay_misses_avg,"
        "aircraft_ordinary_outer_radial_replay_sectors_avg,"
        "aircraft_ordinary_outer_radial_replay_visible_avg,"
        "aircraft_ordinary_outer_radial_replay_culled_avg,"
        "aircraft_ordinary_outer_radial_replay_stream_avg,"
        "aircraft_ordinary_outer_radial_replay_other_avg,"
        "aircraft_ordinary_outer_radial_capture_overflow_avg,"
        "aircraft_ordinary_outer_radial_sector_overflow_avg\n");
    LOGI("[perf.init] game summary CSV: %s", kGameCsvPath);
    return g_gameCsv;
}

void EmitGameWindow(double endMs) {
    GameWindow& w = g_gameWindow;
    if (w.callbacks == 0 || w.startMs <= 0.0 || endMs <= w.startMs) return;
    const double seconds = (endMs - w.startMs) / 1000.0;
    const double callbackHz = w.callbacks / seconds;
    const double renderedHz = w.renderedCallbacks / seconds;
    const double skipPct = 100.0 * w.limiterSkips /
        static_cast<double>(std::max(1, w.callbacks));
    const double threadCpuPct = 100.0 * w.totalThreadCpuMs /
        std::max(0.001, endMs - w.startMs);
    const double frameWallMs = w.callbackWork.Average();
    const double frameCpuMs = w.totalThreadCpuMs /
        static_cast<double>(std::max(1, w.callbacks));
    const double frameBlockedMs = std::max(0.0, frameWallMs - frameCpuMs);
    const traffic_census::Snapshot trafficCensus =
        traffic_census::GetSnapshot();
    static bool trafficCensusBaseline = false;
    static std::uint64_t previousTrafficBirths = 0;
    static std::uint64_t previousTrafficDeaths = 0;
    std::uint64_t trafficBirthsWindow = 0;
    std::uint64_t trafficDeathsWindow = 0;
    if (trafficCensus.valid) {
        if (trafficCensusBaseline &&
            trafficCensus.birthsTotal >= previousTrafficBirths &&
            trafficCensus.deathsTotal >= previousTrafficDeaths) {
            trafficBirthsWindow =
                trafficCensus.birthsTotal - previousTrafficBirths;
            trafficDeathsWindow =
                trafficCensus.deathsTotal - previousTrafficDeaths;
        }
        previousTrafficBirths = trafficCensus.birthsTotal;
        previousTrafficDeaths = trafficCensus.deathsTotal;
        trafficCensusBaseline = true;
    } else {
        trafficCensusBaseline = false;
    }

    {
    std::lock_guard<std::mutex> debugLock(g_debugStatsMutex);
    g_debugStats.callbackHz.store(callbackHz, std::memory_order_relaxed);
    g_debugStats.renderedHz.store(renderedHz, std::memory_order_relaxed);
    g_debugStats.frameWallMs.store(frameWallMs, std::memory_order_relaxed);
    g_debugStats.frameCpuMs.store(frameCpuMs, std::memory_order_relaxed);
    g_debugStats.frameBlockedMs.store(frameBlockedMs, std::memory_order_relaxed);
    g_debugStats.renderWallMs.store(w.renderWall.Average(), std::memory_order_relaxed);
    g_debugStats.renderCpuMs.store(w.renderCpu.Average(), std::memory_order_relaxed);
    g_debugStats.renderBlockedMs.store(w.renderBlocked.Average(), std::memory_order_relaxed);
    g_debugStats.recordWallMs.store(w.recordWall.Average(), std::memory_order_relaxed);
    g_debugStats.recordCpuMs.store(w.recordCpu.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreWallMs.store(
        w.enginePreWall.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreCpuMs.store(
        w.enginePreCpu.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreBreakdownValid.store(
        w.enginePreBreakdownValidFrames > 0 &&
            w.enginePreBreakdownFaults == 0,
        std::memory_order_relaxed);
    g_debugStats.enginePreBreakdownFaults.store(
        w.enginePreBreakdownFaults, std::memory_order_relaxed);
    g_debugStats.enginePreBeforeScanWallMs.store(
        w.enginePreBeforeScanWall.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreBeforeScanCpuMs.store(
        w.enginePreBeforeScanCpu.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreMainScanWallMs.store(
        w.enginePreMainScanWall.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreMainScanCpuMs.store(
        w.enginePreMainScanCpu.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreStockScanWallMs.store(
        w.enginePreStockScanWall.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreStockScanCpuMs.store(
        w.enginePreStockScanCpu.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreAfterScanWallMs.store(
        w.enginePreAfterScanWall.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreAfterScanCpuMs.store(
        w.enginePreAfterScanCpu.Average(), std::memory_order_relaxed);
    g_debugStats.enginePreBreakdownResidualWallMs.store(
        w.enginePreBreakdownResidualWall.Average(),
        std::memory_order_relaxed);
    g_debugStats.enginePreBreakdownResidualCpuMs.store(
        w.enginePreBreakdownResidualCpu.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueWaitHookActive.store(
        w.freshStereoFrames > 0 &&
            w.lastStereo.renderQueueWaitHookActive,
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishCalls.store(
        w.renderQueueFinishCalls.Average(), std::memory_order_relaxed);
    g_debugStats.renderQueueFinishWallMs.store(
        w.renderQueueFinishWall.Average(), std::memory_order_relaxed);
    g_debugStats.renderQueueFinishCpuMs.store(
        w.renderQueueFinishCpu.Average(), std::memory_order_relaxed);
    g_debugStats.renderQueueFinishBlockedMs.store(
        std::max(0.0, w.renderQueueFinishWall.Average() -
                          w.renderQueueFinishCpu.Average()),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishMaxWallMs.store(
        w.renderQueueFinishMaxWall.maximum, std::memory_order_relaxed);
    g_debugStats.renderQueueFlushCalls.store(
        w.renderQueueFlushCalls.Average(), std::memory_order_relaxed);
    g_debugStats.renderQueueFlushWallMs.store(
        w.renderQueueFlushWall.Average(), std::memory_order_relaxed);
    g_debugStats.renderQueueFlushCpuMs.store(
        w.renderQueueFlushCpu.Average(), std::memory_order_relaxed);
    g_debugStats.renderQueueFlushBlockedMs.store(
        std::max(0.0, w.renderQueueFlushWall.Average() -
                          w.renderQueueFlushCpu.Average()),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFlushMaxWallMs.store(
        w.renderQueueFlushMaxWall.maximum, std::memory_order_relaxed);
    g_debugStats.renderQueueWaitBeforeScanWallMs.store(
        w.renderQueueWaitBeforeScanWall.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueWaitMainScanWallMs.store(
        w.renderQueueWaitMainScanWall.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueWaitAfterScanWallMs.store(
        w.renderQueueWaitAfterScanWall.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueWaitMaxUsedKiB.store(
        w.renderQueueWaitMaxUsedKiB.maximum, std::memory_order_relaxed);
    g_debugStats.renderQueueSizedFlushCalls.store(
        w.renderQueueSizedFlushCalls.Average(), std::memory_order_relaxed);
    g_debugStats.renderQueueNearEndFlushCalls.store(
        w.renderQueueNearEndFlushCalls.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueExplicitFlushCalls.store(
        w.renderQueueExplicitFlushCalls.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueWaitClassificationFaults.store(
        w.renderQueueWaitClassificationFaults,
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishDeferActive.store(
        w.freshStereoFrames > 0 &&
            w.lastStereo.renderQueueFinishDeferActive,
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishRequestCalls.store(
        w.renderQueueFinishRequestCalls.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishDeferredCalls.store(
        w.renderQueueFinishDeferredCalls.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishDrainCalls.store(
        w.renderQueueFinishDrainCalls.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishFallbackCalls.store(
        w.renderQueueFinishFallbackCalls.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishOverlapWallMs.store(
        w.renderQueueFinishOverlapWall.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishDrainWallMs.store(
        w.renderQueueFinishDrainWall.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishDrainCpuMs.store(
        w.renderQueueFinishDrainCpu.Average(),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishDrainBlockedMs.store(
        std::max(0.0, w.renderQueueFinishDrainWall.Average() -
                          w.renderQueueFinishDrainCpu.Average()),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishDrainMaxWallMs.store(
        w.renderQueueFinishDrainMaxWall.maximum,
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishPendingDepthMax.store(
        static_cast<int>(w.renderQueueFinishPendingDepthMax.maximum),
        std::memory_order_relaxed);
    g_debugStats.renderQueueFinishPendingFaults.store(
        w.renderQueueFinishPendingFaults,
        std::memory_order_relaxed);
    g_debugStats.stereoPrepareWallMs.store(
        w.stereoPrepareWall.Average(), std::memory_order_relaxed);
    g_debugStats.stereoPrepareCpuMs.store(
        w.stereoPrepareCpu.Average(), std::memory_order_relaxed);
    g_debugStats.stereoTailWallMs.store(
        w.stereoTailWall.Average(), std::memory_order_relaxed);
    g_debugStats.stereoTailCpuMs.store(
        w.stereoTailCpu.Average(), std::memory_order_relaxed);
    g_debugStats.enginePostWallMs.store(
        w.enginePostWall.Average(), std::memory_order_relaxed);
    g_debugStats.enginePostCpuMs.store(
        w.enginePostCpu.Average(), std::memory_order_relaxed);
    g_debugStats.sceneLeftWallMs.store(w.sceneLeft.Average(), std::memory_order_relaxed);
    g_debugStats.sceneRightWallMs.store(w.sceneRight.Average(), std::memory_order_relaxed);
    g_debugStats.sceneLeftCpuMs.store(w.sceneLeftCpu.Average(), std::memory_order_relaxed);
    g_debugStats.sceneRightCpuMs.store(w.sceneRightCpu.Average(), std::memory_order_relaxed);
    g_debugStats.entities.store(w.visibleEntities.Average(), std::memory_order_relaxed);
    g_debugStats.visibleLods.store(w.visibleLods.Average(), std::memory_order_relaxed);
    g_debugStats.visibleSuperLods.store(
        w.visibleSuperLods.Average(), std::memory_order_relaxed);
    g_debugStats.streamingRequests.store(
        w.streamingRequests.Average(), std::memory_order_relaxed);
    g_debugStats.streamingPriorityRequests.store(
        w.streamingPriorityRequests.Average(), std::memory_order_relaxed);
    g_debugStats.streamingMemoryUsedMiB.store(
        w.streamingMemoryUsedMiB.Average(), std::memory_order_relaxed);
    g_debugStats.streamingMemoryAvailableMiB.store(
        w.streamingMemoryAvailableMiB.Average(), std::memory_order_relaxed);
    g_debugStats.lodScale.store(w.lodScale.Average(), std::memory_order_relaxed);
    g_debugStats.cameraLodMultiplier.store(
        w.cameraLodMultiplier.Average(), std::memory_order_relaxed);
    g_debugStats.cameraGenerationMultiplier.store(
        w.cameraGenerationMultiplier.Average(), std::memory_order_relaxed);
    g_debugStats.cameraFov.store(w.cameraFov.Average(), std::memory_order_relaxed);
    g_debugStats.farClip.store(w.farClip.Average(), std::memory_order_relaxed);
    g_debugStats.carPopulationValid.store(
        w.freshStereoFrames > 0 && w.lastStereo.carPopulationValid,
        std::memory_order_relaxed);
    g_debugStats.carRandom.store(w.carRandom.Average(), std::memory_order_relaxed);
    g_debugStats.carRandomMax.store(w.carRandom.maximum, std::memory_order_relaxed);
    g_debugStats.carLaw.store(w.carLaw.Average(), std::memory_order_relaxed);
    g_debugStats.carMission.store(w.carMission.Average(), std::memory_order_relaxed);
    g_debugStats.carParked.store(w.carParked.Average(), std::memory_order_relaxed);
    g_debugStats.carPermanent.store(w.carPermanent.Average(), std::memory_order_relaxed);
    g_debugStats.carMax.store(w.carMax.Average(), std::memory_order_relaxed);
    g_debugStats.carDensity.store(w.carDensity.Average(), std::memory_order_relaxed);
    g_debugStats.pedPopulationValid.store(
        w.freshStereoFrames > 0 && w.lastStereo.pedPopulationValid,
        std::memory_order_relaxed);
    g_debugStats.pedTotal.store(w.pedTotal.Average(), std::memory_order_relaxed);
    g_debugStats.pedTotalMax.store(w.pedTotal.maximum, std::memory_order_relaxed);
    g_debugStats.pedCiv.store(w.pedCiv.Average(), std::memory_order_relaxed);
    g_debugStats.pedGang.store(w.pedGang.Average(), std::memory_order_relaxed);
    g_debugStats.pedMission.store(w.pedMission.Average(), std::memory_order_relaxed);
    g_debugStats.pedCarPassenger.store(
        w.pedCarPassenger.Average(), std::memory_order_relaxed);
    g_debugStats.pedMax.store(w.pedMax.Average(), std::memory_order_relaxed);
    g_debugStats.pedDensity.store(w.pedDensity.Average(), std::memory_order_relaxed);
    g_debugStats.lodWitnessValid.store(
        w.freshStereoFrames > 0 && w.lastStereo.lodWitnessValid,
        std::memory_order_relaxed);
    g_debugStats.lodHandoffHookActive.store(
        w.freshStereoFrames > 0 && w.lastStereo.lodHandoffHookActive,
        std::memory_order_relaxed);
    g_debugStats.linkedEntityTests.store(
        w.linkedEntityTests.Average(), std::memory_order_relaxed);
    g_debugStats.linkedEntityLoaded.store(
        w.linkedEntityLoaded.Average(), std::memory_order_relaxed);
    g_debugStats.linkedResultVisible.store(
        w.linkedResultVisible.Average(), std::memory_order_relaxed);
    g_debugStats.linkedResultCulled.store(
        w.linkedResultCulled.Average(), std::memory_order_relaxed);
    g_debugStats.linkedResultStream.store(
        w.linkedResultStream.Average(), std::memory_order_relaxed);
    g_debugStats.linkedNearThreshold.store(
        w.linkedNearThreshold.Average(), std::memory_order_relaxed);
    g_debugStats.lodPrefetchActive.store(
        w.freshStereoFrames > 0 && w.lastStereo.lodPrefetchActive,
        std::memory_order_relaxed);
    g_debugStats.lodPrefetchFactor.store(
        w.freshStereoFrames > 0 ? w.lastStereo.lodPrefetchFactor : 0.0,
        std::memory_order_relaxed);
    g_debugStats.prefetchBand.store(
        w.prefetchBand.Average(), std::memory_order_relaxed);
    g_debugStats.prefetchStreamMe.store(
        w.prefetchStreamMe.Average(), std::memory_order_relaxed);
    g_debugStats.prefetchRequestCalls.store(
        w.prefetchRequestCalls.Average(), std::memory_order_relaxed);
    g_debugStats.prefetchEnqueues.store(
        w.prefetchEnqueues.Average(), std::memory_order_relaxed);
    g_debugStats.prefetchAlreadyPending.store(
        w.prefetchAlreadyPending.Average(), std::memory_order_relaxed);
    g_debugStats.prefetchThrottled.store(
        w.prefetchThrottled.Average(), std::memory_order_relaxed);
    g_debugStats.prefetchStateAnomalies.store(
        w.prefetchStateAnomalies.Average(), std::memory_order_relaxed);
    g_debugStats.linkedNearReady.store(
        w.linkedNearReady.Average(), std::memory_order_relaxed);
    g_debugStats.linkedNearMissing.store(
        w.linkedNearMissing.Average(), std::memory_order_relaxed);
    g_debugStats.handoffModelId.store(
        w.handoffCandidateValid ? w.handoffModelId : -1, std::memory_order_relaxed);
    g_debugStats.handoffDistance.store(
        w.handoffCandidateValid ? w.handoffDistance : 0.0, std::memory_order_relaxed);
    g_debugStats.handoffThreshold.store(
        w.handoffCandidateValid ? w.handoffThreshold : 0.0, std::memory_order_relaxed);
    g_debugStats.handoffResult.store(
        w.handoffCandidateValid ? w.handoffResult : -1, std::memory_order_relaxed);
    g_debugStats.handoffLoaded.store(
        w.handoffCandidateValid && w.handoffLoaded, std::memory_order_relaxed);
    const bool vehicleLodWitnessValid = w.vehicleMultiPassM.count > 0 &&
        w.vehicleLod0M.count > 0 && w.vehicleLod1M.count > 0 &&
        w.vehicleBigLod0M.count > 0;
    g_debugStats.vehicleLodWitnessValid.store(
        vehicleLodWitnessValid, std::memory_order_relaxed);
    g_debugStats.vehicleMultiPassM.store(
        w.vehicleMultiPassM.Average(), std::memory_order_relaxed);
    g_debugStats.vehicleLod0M.store(
        w.vehicleLod0M.Average(), std::memory_order_relaxed);
    g_debugStats.vehicleLod1M.store(
        w.vehicleLod1M.Average(), std::memory_order_relaxed);
    g_debugStats.vehicleBigLod0M.store(
        w.vehicleBigLod0M.Average(), std::memory_order_relaxed);
    g_debugStats.vehicleLodOverrideActive.store(
        w.freshStereoFrames > 0 && w.lastStereo.vehicleLodOverrideActive,
        std::memory_order_relaxed);
    g_debugStats.vehicleLodTargetM.store(
        w.freshStereoFrames > 0 ? w.lastStereo.vehicleLodTargetM : 0.0,
        std::memory_order_relaxed);
    g_debugStats.vehicleLodSampleHookActive.store(
        w.freshStereoFrames > 0 && w.lastStereo.vehicleLodSampleHookActive,
        std::memory_order_relaxed);
    g_debugStats.vehicleSampleCalls.store(
        w.vehicleSampleCalls.Average(), std::memory_order_relaxed);
    g_debugStats.vehicleNearValid.store(
        w.vehicleCandidateValid, std::memory_order_relaxed);
    g_debugStats.vehicleNearDistance.store(
        w.vehicleCandidateValid ? w.vehicleNearDistance : 0.0,
        std::memory_order_relaxed);
    g_debugStats.vehicleNearThreshold.store(
        w.vehicleCandidateValid ? w.vehicleNearThreshold : 0.0,
        std::memory_order_relaxed);
    g_debugStats.vehicleNearHigh.store(
        w.vehicleCandidateValid && w.vehicleNearHigh,
        std::memory_order_relaxed);
    g_debugStats.streetPropFloorActive.store(
        w.freshStereoFrames > 0 && w.lastStereo.streetPropFloorActive,
        std::memory_order_relaxed);
    g_debugStats.streetPropFloorM.store(
        w.freshStereoFrames > 0 ? w.lastStereo.streetPropFloorM : 0.0,
        std::memory_order_relaxed);
    g_debugStats.unlinkedShortTests.store(
        w.unlinkedShortTests.Average(), std::memory_order_relaxed);
    g_debugStats.streetPropTests.store(
        w.streetPropTests.Average(), std::memory_order_relaxed);
    g_debugStats.streetPropWrites.store(
        w.streetPropWrites.Average(), std::memory_order_relaxed);
    g_debugStats.propModelId.store(
        w.propCandidateValid ? w.propModelId : -1, std::memory_order_relaxed);
    g_debugStats.propDistance.store(
        w.propCandidateValid ? w.propDistance : 0.0, std::memory_order_relaxed);
    g_debugStats.propStockThreshold.store(
        w.propCandidateValid ? w.propStockThreshold : 0.0,
        std::memory_order_relaxed);
    g_debugStats.propAppliedThreshold.store(
        w.propCandidateValid ? w.propAppliedThreshold : 0.0,
        std::memory_order_relaxed);
    g_debugStats.propResult.store(
        w.propCandidateValid ? w.propResult : -1, std::memory_order_relaxed);
    g_debugStats.propLoaded.store(
        w.propCandidateValid && w.propLoaded, std::memory_order_relaxed);
    g_debugStats.propTargeted.store(
        w.propCandidateValid && w.propTargeted, std::memory_order_relaxed);
    g_debugStats.staticTests.store(w.staticTests.Average(), std::memory_order_relaxed);
    g_debugStats.dynamicTests.store(w.dynamicTests.Average(), std::memory_order_relaxed);
    g_debugStats.behindTests.store(w.behindTests.Average(), std::memory_order_relaxed);
    g_debugStats.cullAttributionValid.store(
        w.dynamicMatrixCalls.count > 0 || w.staticSafetyTests.count > 0,
        std::memory_order_relaxed);
    g_debugStats.dynamicMatrixCalls.store(
        w.dynamicMatrixCalls.Average(), std::memory_order_relaxed);
    g_debugStats.dynamicWidenAccepts.store(
        w.dynamicWidenAccepts.Average(), std::memory_order_relaxed);
    g_debugStats.dynamicFallbackVisible.store(
        w.dynamicFallbackVisible.Average(), std::memory_order_relaxed);
    g_debugStats.dynamicFallbackCulled.store(
        w.dynamicFallbackCulled.Average(), std::memory_order_relaxed);
    g_debugStats.dynamicWiden60To80.store(
        w.dynamicWiden60To80.Average(), std::memory_order_relaxed);
    g_debugStats.dynamicWidenBehind.store(
        w.dynamicWidenBehind.Average(), std::memory_order_relaxed);
    g_debugStats.sphere45Shortcuts.store(
        w.sphere45Shortcuts.Average(), std::memory_order_relaxed);
    g_debugStats.staticSafetyRadiusM.store(
        w.freshStereoFrames > 0 ? w.lastStereo.staticSafetyRadiusM : 0.0,
        std::memory_order_relaxed);
    g_debugStats.staticSafetyTests.store(
        w.staticSafetyTests.Average(), std::memory_order_relaxed);
    g_debugStats.staticSafetyStockVisible.store(
        w.staticSafetyStockVisible.Average(), std::memory_order_relaxed);
    g_debugStats.staticSafetyAccepts.store(
        w.staticSafetyAccepts.Average(), std::memory_order_relaxed);
    g_debugStats.staticSafety45To60.store(
        w.staticSafety45To60.Average(), std::memory_order_relaxed);
    g_debugStats.staticSafety60To80.store(
        w.staticSafety60To80.Average(), std::memory_order_relaxed);
    g_debugStats.staticSafetyBuildingAccepts.store(
        w.staticSafetyBuildingAccepts.Average(), std::memory_order_relaxed);
    g_debugStats.staticSafetyDummyAccepts.store(
        w.staticSafetyDummyAccepts.Average(), std::memory_order_relaxed);
    g_debugStats.nearbyScanActive.store(
        w.freshStereoFrames > 0 && w.lastStereo.nearbyScanActive,
        std::memory_order_relaxed);
    g_debugStats.nearbyScanRadiusM.store(
        w.freshStereoFrames > 0 ? w.lastStereo.nearbyScanRadiusM : 0.0,
        std::memory_order_relaxed);
    g_debugStats.nearbyScanSectors.store(
        w.nearbyScanSectors.Average(), std::memory_order_relaxed);
    g_debugStats.nearbyScanVisibleAdded.store(
        w.nearbyScanVisibleAdded.Average(), std::memory_order_relaxed);
    g_debugStats.nearbyScanWallMs.store(
        w.nearbyScanWallMs.Average(), std::memory_order_relaxed);
    g_debugStats.nearbyScanCpuMs.store(
        w.nearbyScanCpuMs.Average(), std::memory_order_relaxed);
    g_debugStats.cullAttributionFaults.store(
        w.cullAttributionFaults, std::memory_order_relaxed);
    g_debugStats.alphaBefore.store(w.alphaNodesBefore.Average(), std::memory_order_relaxed);
    g_debugStats.alphaAfterLeft.store(w.alphaNodesAfterLeft.Average(), std::memory_order_relaxed);
    g_debugStats.alphaAfterRight.store(w.alphaNodesAfterRight.Average(), std::memory_order_relaxed);
    g_debugStats.dedupeChecks.store(w.alphaDedupeChecks, std::memory_order_relaxed);
    g_debugStats.dedupeHits.store(w.alphaDedupeHits, std::memory_order_relaxed);
    g_debugStats.dedupeFaults.store(w.alphaDedupeFaults, std::memory_order_relaxed);
    g_debugStats.shadowCalls.store(w.shadowUpdateCalls, std::memory_order_relaxed);
    g_debugStats.shadowEye2Skips.store(
        w.shadowUpdateSecondEyeSkips, std::memory_order_relaxed);
    g_debugStats.gateSkips.store(w.limiterSkips, std::memory_order_relaxed);
    g_debugStats.frameLimit.store(w.last.rsFrameLimit, std::memory_order_relaxed);
    g_debugStats.eyeWidth.store(w.lastStereo.eyeWidth, std::memory_order_relaxed);
    g_debugStats.eyeHeight.store(w.lastStereo.eyeHeight, std::memory_order_relaxed);
    g_debugStats.renderScalePercent.store(
        w.lastStereo.renderScalePercent, std::memory_order_relaxed);
    g_debugStats.alphaHookActive.store(
        w.lastStereo.alphaHookActive, std::memory_order_relaxed);
    g_debugStats.shadowHookActive.store(
        w.lastStereo.shadowHookActive, std::memory_order_relaxed);
    g_debugStats.buildingDetailTests.store(
        w.buildingDetailTests.Average(), std::memory_order_relaxed);
    g_debugStats.buildingDetailOverrides.store(
        w.buildingDetailOverrides.Average(), std::memory_order_relaxed);
    g_debugStats.modelDrawRestoreFaults.store(
        w.modelDrawRestoreFaults, std::memory_order_relaxed);
    g_debugStats.ambientCarGateActive.store(
        w.freshStereoFrames > 0 && w.lastStereo.ambientCarGateActive,
        std::memory_order_relaxed);
    g_debugStats.ambientCarTarget.store(
        w.freshStereoFrames > 0 ? w.lastStereo.ambientCarTarget : 0,
        std::memory_order_relaxed);
    g_debugStats.ambientCarAttempts.store(
        w.ambientCarAttempts.Average(), std::memory_order_relaxed);
    g_debugStats.ambientCarBlocked.store(
        w.ambientCarBlocked.Average(), std::memory_order_relaxed);
    g_debugStats.ambientCarWantedPasses.store(
        w.ambientCarWantedPasses.Average(), std::memory_order_relaxed);
    g_debugStats.pedAmbientCap.store(
        w.freshStereoFrames > 0 ? w.lastStereo.pedAmbientCap : 0,
        std::memory_order_relaxed);
    g_debugStats.pedAmbientAttempts.store(
        w.pedAmbientAttempts.Average(), std::memory_order_relaxed);
    g_debugStats.pedAmbientSuccesses.store(
        w.pedAmbientSuccesses.Average(), std::memory_order_relaxed);
    g_debugStats.pedAmbientBlocked.store(
        w.pedAmbientBlocked.Average(), std::memory_order_relaxed);
    g_debugStats.gameValid.store(true, std::memory_order_relaxed);
    g_debugStats.revision.fetch_add(1, std::memory_order_release);
    }

    LOGI("[perf.game] v2 win=%.2fs calls=%d(%.1fHz) rendered=%d(%.1fHz) "
         "gateSkip=%d(%.0f%%) stereoFresh=%d jumps=%d rsCalls=%d "
         "cap=%d gta=%.1f step=%.3f/%.3f "
         "frame=%u flags=%d/%d core=%d stereo=%d",
         seconds, w.callbacks, callbackHz, w.renderedCallbacks, renderedHz,
         w.limiterSkips, skipPct, w.freshStereoFrames, w.sequenceJumps,
         w.renderSceneCalls, w.last.rsFrameLimit,
         w.last.gameFps, w.last.timeStep, w.last.timeStepNonClipped,
         w.last.gameFrameCounter, w.last.skipProcess, w.last.skipFrame,
         w.last.cpuCore, w.last.stereoActive ? 1 : 0);
    LOGI("[perf.game.t] period=%.2f/%.2f/%.2f cbWork=%.2f/%.2f/%.2f "
         "javaDt=%.2f gap=%.2f/%.2f/%.2f "
         "renderW=%.2f/%.2f/%.2f renderCPU=%.2f block=%.2f "
         "skipW=%.3f/%.3f/%.3f skipCPU=%.3f block=%.3f "
         "swap=%d fail=%d %.2f/%.2f sleepR=%.2f/%.2f sleepS=%.2f/%.2f cpuDuty=%.0f%% "
         "recW/CPU=%.2f/%.2f eyesW=%.2f+%.2f eyesCPU=%.2f+%.2f "
         "alpha=%.0f/%.0f/%.0f dedupe=%d/%d/%d shadow=%d/%d hooks=%d/%d "
         "other=%.2f ent=%.0f/%.0f "
         "cull=%.0f/%.0f/%.0f scale=%d%% %dx%d",
         w.callbackPeriod.Average(), w.callbackPeriod.P95(), w.callbackPeriod.maximum,
         w.callbackWork.Average(), w.callbackWork.P95(), w.callbackWork.maximum,
         w.javaDelta.Average(),
         w.renderGap.Average(), w.renderGap.P95(), w.renderGap.maximum,
         w.renderWall.Average(), w.renderWall.P95(), w.renderWall.maximum,
         w.renderCpu.Average(), w.renderBlocked.Average(),
         w.skipWall.Average(), w.skipWall.P95(), w.skipWall.maximum,
         w.skipCpu.Average(), w.skipBlocked.Average(),
         w.wrapperSwapCalls, w.wrapperSwapFailures,
         w.swapWall.Average(), w.swapWall.maximum,
         w.renderSleepRequested.Average(), w.renderSleepActual.Average(),
         w.skipSleepRequested.Average(), w.skipSleepActual.Average(), threadCpuPct,
         w.recordWall.Average(), w.recordCpu.Average(),
         w.sceneLeft.Average(), w.sceneRight.Average(),
         w.sceneLeftCpu.Average(), w.sceneRightCpu.Average(),
         w.alphaNodesBefore.Average(), w.alphaNodesAfterLeft.Average(),
         w.alphaNodesAfterRight.Average(), w.alphaDedupeHits,
         w.alphaDedupeChecks, w.alphaDedupeFaults,
         w.shadowUpdateSecondEyeSkips, w.shadowUpdateCalls,
         w.lastStereo.alphaHookActive ? 1 : 0,
         w.lastStereo.shadowHookActive ? 1 : 0,
         w.engineOutsideRecord.Average(), w.visibleEntities.Average(),
         w.visibleEntities.maximum, w.staticTests.Average(),
         w.dynamicTests.Average(), w.behindTests.Average(),
           w.lastStereo.renderScalePercent, w.lastStereo.eyeWidth,
           w.lastStereo.eyeHeight);

    LOGI("[perf.game.phase] valid=%d/%d faults=%d "
         "W pre/prep/rec/tail/post=%.3f/%.3f/%.3f/%.3f/%.3f err=%.4f "
         "CPU pre/prep/rec/tail/post=%.3f/%.3f/%.3f/%.3f/%.3f err=%.4f",
         w.phaseValidFrames > 0 ? 1 : 0, w.phaseValidFrames, w.phaseFaults,
         w.enginePreWall.Average(), w.stereoPrepareWall.Average(),
         w.recordWall.Average(), w.stereoTailWall.Average(),
         w.enginePostWall.Average(), w.phaseWallError.Average(),
         w.enginePreCpu.Average(), w.stereoPrepareCpu.Average(),
         w.recordCpu.Average(), w.stereoTailCpu.Average(),
         w.enginePostCpu.Average(), w.phaseCpuError.Average());

    LOGI("[perf.game.pre] valid=%d/%d faults=%d "
         "W before/main/stock/after=%.3f/%.3f/%.3f/%.3f res=%.4f "
         "CPU before/main/stock/after=%.3f/%.3f/%.3f/%.3f res=%.4f",
         w.enginePreBreakdownValidFrames > 0 &&
                 w.enginePreBreakdownFaults == 0 ? 1 : 0,
         w.enginePreBreakdownValidFrames, w.enginePreBreakdownFaults,
         w.enginePreBeforeScanWall.Average(),
         w.enginePreMainScanWall.Average(),
         w.enginePreStockScanWall.Average(),
         w.enginePreAfterScanWall.Average(),
         w.enginePreBreakdownResidualWall.Average(),
         w.enginePreBeforeScanCpu.Average(),
         w.enginePreMainScanCpu.Average(),
         w.enginePreStockScanCpu.Average(),
         w.enginePreAfterScanCpu.Average(),
         w.enginePreBreakdownResidualCpu.Average());
    LOGI("[perf.game.rq] hook=%d data=%d "
         "finish call/W/CPU/block/max=%.2f/%.3f/%.3f/%.3f/%.3f "
         "flush call/W/CPU/block/max=%.2f/%.3f/%.3f/%.3f/%.3f "
         "type sized/unsized/reserved=%.2f/%.2f/%.2f "
         "stage before/main/after W=%.3f/%.3f/%.3f high_water=%.0fKiB fault=%d",
         w.freshStereoFrames > 0 &&
                 w.lastStereo.renderQueueWaitHookActive ? 1 : 0,
         w.enginePreBreakdownValidFrames > 0 &&
                 w.enginePreBreakdownFaults == 0 ? 1 : 0,
         w.renderQueueFinishCalls.Average(),
         w.renderQueueFinishWall.Average(),
         w.renderQueueFinishCpu.Average(),
         std::max(0.0, w.renderQueueFinishWall.Average() -
                           w.renderQueueFinishCpu.Average()),
         w.renderQueueFinishMaxWall.maximum,
         w.renderQueueFlushCalls.Average(),
         w.renderQueueFlushWall.Average(),
         w.renderQueueFlushCpu.Average(),
         std::max(0.0, w.renderQueueFlushWall.Average() -
                           w.renderQueueFlushCpu.Average()),
         w.renderQueueFlushMaxWall.maximum,
         w.renderQueueSizedFlushCalls.Average(),
         w.renderQueueNearEndFlushCalls.Average(),
         w.renderQueueExplicitFlushCalls.Average(),
         w.renderQueueWaitBeforeScanWall.Average(),
         w.renderQueueWaitMainScanWall.Average(),
         w.renderQueueWaitAfterScanWall.Average(),
         w.renderQueueWaitMaxUsedKiB.maximum,
         w.renderQueueWaitClassificationFaults);
    LOGI("[perf.game.rq.defer] active=%d "
         "request/defer/drain/fallback=%.2f/%.2f/%.2f/%.2f "
         "overlap=%.3f drain W/CPU/block/max=%.3f/%.3f/%.3f/%.3f "
         "pending max/fault=%d/%d",
         w.freshStereoFrames > 0 &&
                 w.lastStereo.renderQueueFinishDeferActive ? 1 : 0,
         w.renderQueueFinishRequestCalls.Average(),
         w.renderQueueFinishDeferredCalls.Average(),
         w.renderQueueFinishDrainCalls.Average(),
         w.renderQueueFinishFallbackCalls.Average(),
         w.renderQueueFinishOverlapWall.Average(),
         w.renderQueueFinishDrainWall.Average(),
         w.renderQueueFinishDrainCpu.Average(),
         std::max(0.0, w.renderQueueFinishDrainWall.Average() -
                           w.renderQueueFinishDrainCpu.Average()),
         w.renderQueueFinishDrainMaxWall.maximum,
         static_cast<int>(w.renderQueueFinishPendingDepthMax.maximum),
         w.renderQueueFinishPendingFaults);

    LOGI("[perf.game.cull] call/vis/wide/fall/cul=%.0f/%.0f/%.0f/%.0f/%.0f "
         "wide60+/back=%.0f/%.0f sphere45=%.0f "
         "static%.0f test/stock/accept=%.0f/%.0f/%.0f "
         "accept45-60/60-80/bld/dmy=%.0f/%.0f/%.0f/%.0f "
         "hmdCone=%.0fdeg valid=%d reject=%.0f attrFault=%d",
         w.dynamicMatrixCalls.Average(), w.dynamicTests.Average(),
         w.dynamicWidenAccepts.Average(), w.dynamicFallbackVisible.Average(),
         w.dynamicFallbackCulled.Average(), w.dynamicWiden60To80.Average(),
         w.dynamicWidenBehind.Average(), w.sphere45Shortcuts.Average(),
         w.freshStereoFrames > 0 ? w.lastStereo.staticSafetyRadiusM : 0.0,
         w.staticSafetyTests.Average(),
         w.staticSafetyStockVisible.Average(),
         w.staticSafetyAccepts.Average(),
         w.staticSafety45To60.Average(), w.staticSafety60To80.Average(),
         w.staticSafetyBuildingAccepts.Average(),
         w.staticSafetyDummyAccepts.Average(),
         w.freshStereoFrames > 0
             ? w.lastStereo.staticCullConeHalfAngleDeg : 0.0,
         w.freshStereoFrames > 0 && w.lastStereo.staticCullConeValid ? 1 : 0,
         w.staticSafetyConeRejects.Average(),
         w.cullAttributionFaults);

    LOGI("[perf.game.nearby] active=%d radius=%.0fm hmd_heading=%d "
         "main/secondary=%d/%d sectors/reject/visible+/request+="
         "%.1f/%.1f/%.1f/%.1f wall/cpu=%.3f/%.3fms",
         w.freshStereoFrames > 0 && w.lastStereo.nearbyScanActive ? 1 : 0,
         w.freshStereoFrames > 0 ? w.lastStereo.nearbyScanRadiusM : 0.0,
         w.freshStereoFrames > 0 &&
                  w.lastStereo.nearbyScanHmdHeadingActive ? 1 : 0,
         w.freshStereoFrames > 0 ? w.lastStereo.nearbyScanMainPasses : 0,
         w.freshStereoFrames > 0
             ? w.lastStereo.nearbyScanSecondaryBypasses : 0,
         w.nearbyScanSectors.Average(),
         w.nearbyScanConeRejectedSectors.Average(),
         w.nearbyScanVisibleAdded.Average(),
         w.nearbyScanStreamingRequests.Average(),
         w.nearbyScanWallMs.Average(), w.nearbyScanCpuMs.Average());

    LOGI("[perf.game.tree] radius=%.0fm "
         "ext test/stock/accept/reject=%.1f/%.1f/%.1f/%.1f",
         w.freshStereoFrames > 0
             ? w.lastStereo.largeVegetationSafetyRadiusM : 0.0,
         w.largeVegetationSafetyTests.Average(),
         w.largeVegetationSafetyStockVisible.Average(),
         w.largeVegetationSafetyAccepts.Average(),
         w.largeVegetationSafetyConeRejects.Average());

    LOGI("[perf.game.roadside] radius=%.0fm "
         "ext test/stock/accept/reject=%.1f/%.1f/%.1f/%.1f "
         "occ_active/bypass=%d/%.1f",
         w.freshStereoFrames > 0
             ? w.lastStereo.roadsideSafetyRadiusM : 0.0,
         w.roadsideSafetyTests.Average(),
         w.roadsideSafetyStockVisible.Average(),
         w.roadsideSafetyAccepts.Average(),
         w.roadsideSafetyConeRejects.Average(),
         w.freshStereoFrames > 0 &&
                 w.lastStereo.targetedOcclusionHookActive ? 1 : 0,
         w.targetedOcclusionBypasses.Average());

    const double targetedOcclusionRouteError = std::fabs(
        w.targetedOcclusionStockOccluded.Average() -
        (w.targetedOcclusionRadialBypasses.Average() +
         w.targetedOcclusionConeBypasses.Average() +
         w.targetedOcclusionRecentBypasses.Average() +
         w.targetedOcclusionStockCulls.Average() +
         w.targetedOcclusionInvalidPoseBypasses.Average()));
    LOGI("[perf.game.occlusion] active=%d tests=%.1f stock_vis/occ=%.1f/%.1f "
         "radial/cone/recent/invalid/cull=%.1f/%.1f/%.1f/%.1f/%.1f "
         "route_err=%.2f cache hit/miss/evict/overflow/reset="
         "%.1f/%.1f/%.1f/%.1f/%.1f",
         w.freshStereoFrames > 0 &&
                 w.lastStereo.targetedOcclusionHookActive ? 1 : 0,
         w.targetedOcclusionTests.Average(),
         w.targetedOcclusionStockVisible.Average(),
         w.targetedOcclusionStockOccluded.Average(),
         w.targetedOcclusionRadialBypasses.Average(),
         w.targetedOcclusionConeBypasses.Average(),
         w.targetedOcclusionRecentBypasses.Average(),
         w.targetedOcclusionInvalidPoseBypasses.Average(),
         w.targetedOcclusionStockCulls.Average(),
         targetedOcclusionRouteError,
         w.targetedOcclusionCacheHits.Average(),
         w.targetedOcclusionCacheMisses.Average(),
         w.targetedOcclusionCacheEvictions.Average(),
         w.targetedOcclusionCacheOverflows.Average(),
         w.targetedOcclusionIdentityResets.Average());

    LOGI("[perf.game.lod] list ent/lod/super=%.0f/%.0f/%.0f "
         "max=%.0f/%.0f/%.0f stream q/p=%.0f/%.0f mem=%.1f/%.1fMiB "
         "scale/cam/gen=%.3f/%.3f/%.3f fov=%.1f far=%.1f valid=%d",
         w.visibleEntities.Average(), w.visibleLods.Average(),
         w.visibleSuperLods.Average(), w.visibleEntities.maximum,
         w.visibleLods.maximum, w.visibleSuperLods.maximum,
         w.streamingRequests.Average(), w.streamingPriorityRequests.Average(),
         w.streamingMemoryUsedMiB.Average(), w.streamingMemoryAvailableMiB.Average(),
         w.lodScale.Average(), w.cameraLodMultiplier.Average(),
         w.cameraGenerationMultiplier.Average(), w.cameraFov.Average(),
         w.farClip.Average(),
         w.freshStereoFrames > 0 && w.lastStereo.lodWitnessValid ? 1 : 0);

    LOGI("[perf.game.pop] car valid=%d R/L/M/P/K=%.1f/%.1f/%.1f/%.1f/%.1f "
         "cap/dens=%.1f/%.2f ped valid=%d T/C/G/M/P=%.1f/%.1f/%.1f/%.1f/%.1f "
         "cap/dens=%.1f/%.2f",
         w.freshStereoFrames > 0 && w.lastStereo.carPopulationValid ? 1 : 0,
         w.carRandom.Average(), w.carLaw.Average(), w.carMission.Average(),
         w.carParked.Average(), w.carPermanent.Average(), w.carMax.Average(),
         w.carDensity.Average(),
         w.freshStereoFrames > 0 && w.lastStereo.pedPopulationValid ? 1 : 0,
         w.pedTotal.Average(), w.pedCiv.Average(), w.pedGang.Average(),
         w.pedMission.Average(), w.pedCarPassenger.Average(), w.pedMax.Average(),
         w.pedDensity.Average());
    LOGI("[perf.game.pool] valid=%d V/R/M/P/K/U/L=%u/%u/%u/%u/%u/%u/%u "
         "birth/death window=%llu/%llu total=%llu/%llu samples=%llu "
         "local valid/near/front/useful/law=%d/%u/%u/%u/%u "
         "director base/effective/wanted=%d/%d/%u",
         trafficCensus.valid ? 1 : 0, trafficCensus.live,
         trafficCensus.random, trafficCensus.mission, trafficCensus.parked,
         trafficCensus.permanent, trafficCensus.unknown, trafficCensus.law,
         static_cast<unsigned long long>(trafficBirthsWindow),
         static_cast<unsigned long long>(trafficDeathsWindow),
         static_cast<unsigned long long>(trafficCensus.birthsTotal),
         static_cast<unsigned long long>(trafficCensus.deathsTotal),
         static_cast<unsigned long long>(trafficCensus.samples),
         trafficCensus.localViewValid ? 1 : 0,
         trafficCensus.localRandomNear,
         trafficCensus.localRandomForward,
         trafficCensus.localRandomUseful,
         trafficCensus.localLawUseful,
         trafficCensus.directorBaseTarget,
         trafficCensus.directorEffectiveTarget,
         trafficCensus.directorWantedLevel);
    LOGI("[perf.game.traffic-class] road/random/law/effective_law=%u/%u/%u/%u "
         "air/heli/nonroadlaw=%u/%u/%u "
         "local road near/front/useful/law=%u/%u/%u/%u",
         trafficCensus.road, trafficCensus.roadRandom,
         trafficCensus.roadLaw, trafficCensus.roadLawEffective,
         trafficCensus.aircraft,
         trafficCensus.helicopters, trafficCensus.nonRoadLaw,
         trafficCensus.localRoadRandomNear,
         trafficCensus.localRoadRandomForward,
         trafficCensus.localRoadRandomUseful,
         trafficCensus.localRoadLawUseful);
    LOGI("[perf.game.traffic-state] ambient "
         "raw/effective/moving/driverless/fading/abandoned/wrecked/terminal="
         "%u/%u/%u/%u/%u/%u/%u/%u "
         "local_effective near/front/useful=%u/%u/%u",
         trafficCensus.roadRandom,
         trafficCensus.roadRandomEffective,
         trafficCensus.roadRandomMoving,
         trafficCensus.roadRandomDriverless,
         trafficCensus.roadRandomFading,
         trafficCensus.roadRandomAbandoned,
         trafficCensus.roadRandomWrecked,
         trafficCensus.roadRandomTerminal,
         trafficCensus.localRoadRandomEffectiveNear,
         trafficCensus.localRoadRandomEffectiveForward,
         trafficCensus.localRoadRandomEffectiveUseful);
    LOGI("[perf.game.traffic-response] response "
         "total/effective/driverless/fading/abandoned/wrecked/terminal="
         "%u/%u/%u/%u/%u/%u/%u police_model total/law/legacy_ambient="
         "%u/%u/%u copbike total/law/legacy_ambient=%u/%u/%u",
         trafficCensus.roadResponse,
         trafficCensus.roadResponseEffective,
         trafficCensus.roadResponseDriverless,
         trafficCensus.roadResponseFading,
         trafficCensus.roadResponseAbandoned,
         trafficCensus.roadResponseWrecked,
         trafficCensus.roadResponseTerminal,
         trafficCensus.roadPoliceModel,
         trafficCensus.roadPoliceModelLaw,
         trafficCensus.roadPoliceModelAmbient,
         trafficCensus.roadCopBike,
         trafficCensus.roadCopBikeLaw,
         trafficCensus.roadCopBikeAmbient);
    LOGI("[perf.game.traffic-scavenger] witness=%d stamped/marks=%u/%llu "
         "terminal/retired/eligible/over/pending=%u/%u/%u/%d/%d "
         "block young/needed/recent/retry/suppress/view/near/cone/heli="
         "%u/%u/%u/%u/%u/%u/%u/%u/%u "
         "candidate kind/age_ms/render_age_ms/distance/witness="
         "%u/%u/%u/%.1f/%d "
         "total select/claim/guard/stale/delete/unconfirmed="
         "%llu/%llu/%llu/%llu/%llu/%llu",
         trafficCensus.cleanupRenderWitnessAvailable ? 1 : 0,
         trafficCensus.cleanupRenderStamped,
         static_cast<unsigned long long>(
             trafficCensus.cleanupRenderMarksTotal),
         trafficCensus.cleanupTerminal,
         trafficCensus.cleanupRetiredHelicopters,
         trafficCensus.cleanupEligible,
         trafficCensus.cleanupOverBudget ? 1 : 0,
         trafficCensus.cleanupCandidatePending ? 1 : 0,
         trafficCensus.cleanupBlockedYoung,
         trafficCensus.cleanupBlockedExplicitNeeded,
         trafficCensus.cleanupBlockedRecentRender,
         trafficCensus.cleanupBlockedRetry,
         trafficCensus.cleanupBlockedSuppressed,
         trafficCensus.cleanupBlockedViewInvalid,
         trafficCensus.cleanupBlockedNear,
         trafficCensus.cleanupBlockedConeFallback,
         trafficCensus.cleanupBlockedRetiredDistance,
         static_cast<unsigned>(trafficCensus.cleanupCandidateKind),
         trafficCensus.cleanupCandidateAgeMs,
         trafficCensus.cleanupCandidateLastRenderedAgeMs,
         static_cast<double>(trafficCensus.cleanupCandidateDistanceM),
         trafficCensus.cleanupCandidateRenderWitnessBacked ? 1 : 0,
         static_cast<unsigned long long>(
             trafficCensus.cleanupSelectionsTotal),
         static_cast<unsigned long long>(trafficCensus.cleanupClaimsTotal),
         static_cast<unsigned long long>(trafficCensus.cleanupGuardedTotal),
         static_cast<unsigned long long>(trafficCensus.cleanupStaleTotal),
         static_cast<unsigned long long>(trafficCensus.cleanupDeletedTotal),
         static_cast<unsigned long long>(
             trafficCensus.cleanupDeleteUnconfirmedTotal));
    LOGI("[perf.game.traffic-witness2] atomic active/stamped/marks="
         "%d/%u/%llu probes attempt/noatomic/pipeline/clump="
         "%llu/%llu/%llu/%llu terminal_zone invalid/near15/core120/"
         "fringe120to140/out140=%u/%u/%u/%u/%u "
         "recent entry/atomic/entry_only/unavailable/atomic_never/atomic_stale="
         "%u/%u/%u/%u/%u/%u",
         trafficCensus.cleanupAtomicWitnessAvailable ? 1 : 0,
         trafficCensus.cleanupAtomicStamped,
         static_cast<unsigned long long>(
             trafficCensus.cleanupAtomicMarksTotal),
         static_cast<unsigned long long>(
             trafficCensus.cleanupAtomicProbeAttemptsTotal),
         static_cast<unsigned long long>(
             trafficCensus.cleanupAtomicProbeNoAtomicTotal),
         static_cast<unsigned long long>(
             trafficCensus.cleanupAtomicProbePipelineFailTotal),
         static_cast<unsigned long long>(
             trafficCensus.cleanupAtomicProbeClumpMismatchTotal),
         trafficCensus.cleanupTerminalZoneInvalid,
         trafficCensus.cleanupTerminalZoneNear45,
         trafficCensus.cleanupTerminalZoneCoreCone,
         trafficCensus.cleanupTerminalZoneWitnessFringe,
         trafficCensus.cleanupTerminalZoneOutsideWitness,
         trafficCensus.cleanupBlockedRecentRender,
         trafficCensus.cleanupRecentAtomicRecent,
         trafficCensus.cleanupRecentEntryOnly,
         trafficCensus.cleanupRecentAtomicUnavailable,
         trafficCensus.cleanupRecentAtomicNever,
         trafficCensus.cleanupRecentAtomicStale);
    const auto& witnessProbe = trafficCensus.cleanupWitnessProbe;
    LOGI("[perf.game.traffic-witness2.probe] "
         "valid/suspicious/model/slot/gen/state/current_zone/entry_zone="
         "%d/%d/%d/%u/%u/0x%02x/%u/%u terminal_age_ms=%u "
         "entry stamped/recent/age_ms/distance="
         "%d/%d/%u/%.1f atomic stamped/recent/age_ms="
         "%d/%d/%u current_distance=%.1f",
         witnessProbe.valid ? 1 : 0,
         witnessProbe.suspicious ? 1 : 0,
         static_cast<int>(witnessProbe.modelId),
         static_cast<unsigned>(witnessProbe.slot),
         static_cast<unsigned>(witnessProbe.generation),
         static_cast<unsigned>(witnessProbe.stateMask),
         static_cast<unsigned>(witnessProbe.zone),
         static_cast<unsigned>(witnessProbe.entryZone),
         witnessProbe.terminalAgeMs,
         witnessProbe.entryStamped ? 1 : 0,
         witnessProbe.entryRecent ? 1 : 0,
         witnessProbe.entryAgeMs,
         static_cast<double>(witnessProbe.entryDistanceM),
         witnessProbe.atomicStamped ? 1 : 0,
         witnessProbe.atomicRecent ? 1 : 0,
         witnessProbe.atomicAgeMs,
         static_cast<double>(witnessProbe.distanceM));

    const bool vehicleWitnessValid = w.vehicleMultiPassM.count > 0 &&
        w.vehicleLod0M.count > 0 && w.vehicleLod1M.count > 0 &&
        w.vehicleBigLod0M.count > 0;
    LOGI("[perf.game.lodlink] hook=%d "
         "avg t/load/vis/cull/stream/near=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f "
         "closest id/result/loaded=%d/%d/%d d/th=%.1f/%.1f "
         "car valid=%d mp/l0/l1/b0=%.1f/%.1f/%.1f/%.1f",
         w.freshStereoFrames > 0 && w.lastStereo.lodHandoffHookActive ? 1 : 0,
         w.linkedEntityTests.Average(), w.linkedEntityLoaded.Average(),
         w.linkedResultVisible.Average(), w.linkedResultCulled.Average(),
         w.linkedResultStream.Average(), w.linkedNearThreshold.Average(),
         w.handoffCandidateValid ? w.handoffModelId : -1,
         w.handoffCandidateValid ? w.handoffResult : -1,
         w.handoffCandidateValid && w.handoffLoaded ? 1 : 0,
         w.handoffCandidateValid ? w.handoffDistance : -1.0,
         w.handoffCandidateValid ? w.handoffThreshold : -1.0,
         vehicleWitnessValid ? 1 : 0, w.vehicleMultiPassM.Average(),
         w.vehicleLod0M.Average(), w.vehicleLod1M.Average(),
         w.vehicleBigLod0M.Average());

    LOGI("[perf.game.prefetch] active=%d factor=%.2f "
         "avg band/streamme/request/enqueue/pending/throttle/anomaly="
         "%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f edge ready/missing=%.1f/%.1f",
         w.freshStereoFrames > 0 && w.lastStereo.lodPrefetchActive ? 1 : 0,
         w.freshStereoFrames > 0 ? w.lastStereo.lodPrefetchFactor : 0.0,
         w.prefetchBand.Average(), w.prefetchStreamMe.Average(),
         w.prefetchRequestCalls.Average(), w.prefetchEnqueues.Average(),
         w.prefetchAlreadyPending.Average(), w.prefetchThrottled.Average(),
         w.prefetchStateAnomalies.Average(),
         w.linkedNearReady.Average(), w.linkedNearMissing.Average());

    LOGI("[perf.game.vehicle] override=%d target=%.1f hook=%d calls=%.1f "
         "near valid/high=%d/%d d/th=%.1f/%.1f",
         w.freshStereoFrames > 0 && w.lastStereo.vehicleLodOverrideActive ? 1 : 0,
         w.freshStereoFrames > 0 ? w.lastStereo.vehicleLodTargetM : 0.0,
         w.freshStereoFrames > 0 && w.lastStereo.vehicleLodSampleHookActive ? 1 : 0,
         w.vehicleSampleCalls.Average(), w.vehicleCandidateValid ? 1 : 0,
         w.vehicleCandidateValid && w.vehicleNearHigh ? 1 : 0,
         w.vehicleCandidateValid ? w.vehicleNearDistance : -1.0,
         w.vehicleCandidateValid ? w.vehicleNearThreshold : -1.0);

    const double propFadeOut = w.propCandidateValid
        ? w.propAppliedThreshold + 22.0 * w.lodScale.Average() : -1.0;
    LOGI("[perf.game.props] floor=%d/%.1f avg short/target/register=%.1f/%.1f/%.1f "
         "near id/result/loaded/target=%d/%d/%d/%d d=%.1f "
         "full stock/applied=%.1f/%.1f fade_out=%.1f",
         w.freshStereoFrames > 0 && w.lastStereo.streetPropFloorActive ? 1 : 0,
         w.freshStereoFrames > 0 ? w.lastStereo.streetPropFloorM : 0.0,
         w.unlinkedShortTests.Average(), w.streetPropTests.Average(),
         w.streetPropWrites.Average(),
         w.propCandidateValid ? w.propModelId : -1,
         w.propCandidateValid ? w.propResult : -1,
         w.propCandidateValid && w.propLoaded ? 1 : 0,
         w.propCandidateValid && w.propTargeted ? 1 : 0,
         w.propCandidateValid ? w.propDistance : -1.0,
         w.propCandidateValid ? w.propStockThreshold : -1.0,
         w.propCandidateValid ? w.propAppliedThreshold : -1.0,
         propFadeOut);

    LOGI("[perf.game.detail] building tests/override=%.1f/%.1f "
         "model_draw_restore_fault=%d",
         w.buildingDetailTests.Average(),
         w.buildingDetailOverrides.Average(),
         w.modelDrawRestoreFaults);
    LOGI("[perf.game.popgate] car active/target=%d/%d "
         "attempt/block/wanted=%.1f/%.1f/%.1f "
         "ped cap/attempt/success/block=%d/%.1f/%.1f/%.1f",
         w.freshStereoFrames > 0 && w.lastStereo.ambientCarGateActive ? 1 : 0,
         w.freshStereoFrames > 0 ? w.lastStereo.ambientCarTarget : 0,
         w.ambientCarAttempts.Average(), w.ambientCarBlocked.Average(),
         w.ambientCarWantedPasses.Average(),
         w.freshStereoFrames > 0 ? w.lastStereo.pedAmbientCap : 0,
         w.pedAmbientAttempts.Average(),
         w.pedAmbientSuccesses.Average(),
         w.pedAmbientBlocked.Average());

    if (FILE* file = OpenGameCsv()) {
        std::fprintf(file,
            "%.3f,%d,%.3f,%d,%.3f,%d,%.2f,%d,%d,%d,%d,%.3f,%.5f,%.5f,%u,%d,%d,%d,%d,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,"
            "%.4f,%.4f,%.4f,%.4f,"
            "%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%d,%d,%d,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.2f,%.1f",
            seconds, w.callbacks, callbackHz, w.renderedCallbacks, renderedHz,
            w.limiterSkips, skipPct, w.freshStereoFrames, w.sequenceJumps,
            w.renderSceneCalls,
            w.last.rsFrameLimit, w.last.gameFps, w.last.timeStep,
            w.last.timeStepNonClipped, w.last.gameFrameCounter,
            w.last.skipProcess, w.last.skipFrame, w.last.cpuCore,
            w.last.stereoActive ? 1 : 0,
            w.callbackPeriod.Average(), w.callbackPeriod.P95(), w.callbackPeriod.maximum,
            w.callbackWork.Average(), w.callbackWork.P95(), w.callbackWork.maximum,
            w.javaDelta.Average(),
            w.renderGap.Average(), w.renderGap.P95(), w.renderGap.maximum,
            w.renderWall.Average(), w.renderWall.P95(), w.renderWall.maximum,
            w.renderCpu.Average(), w.renderBlocked.Average(),
            w.skipWall.Average(), w.skipWall.P95(), w.skipWall.maximum,
            w.skipCpu.Average(), w.skipBlocked.Average(),
            w.wrapperSwapCalls, w.wrapperSwapFailures,
            w.swapWall.Average(), w.swapWall.maximum,
            w.renderSleepRequested.Average(), w.renderSleepActual.Average(),
            w.skipSleepRequested.Average(), w.skipSleepActual.Average(), threadCpuPct,
            w.recordWall.Average(), w.recordCpu.Average(),
            w.sceneLeft.Average(), w.sceneRight.Average(),
            w.sceneLeftCpu.Average(), w.sceneRightCpu.Average(),
            w.alphaNodesBefore.Average(), w.alphaNodesAfterLeft.Average(),
            w.alphaNodesAfterRight.Average(), w.alphaDedupeChecks,
            w.alphaDedupeHits, w.alphaDedupeFaults,
            w.shadowUpdateCalls, w.shadowUpdateSecondEyeSkips,
            w.lastStereo.alphaHookActive ? 1 : 0,
            w.lastStereo.shadowHookActive ? 1 : 0,
            w.skyLeft.Average(), w.skyRight.Average(), w.cameraEnd.Average(),
            w.engineOutsideRecord.Average(),
            w.phaseValidFrames > 0 ? 1 : 0, w.phaseFaults,
            w.enginePreWall.Average(), w.enginePreCpu.Average(),
            w.stereoPrepareWall.Average(), w.stereoPrepareCpu.Average(),
            w.stereoTailWall.Average(), w.stereoTailCpu.Average(),
            w.enginePostWall.Average(), w.enginePostCpu.Average(),
            w.phaseWallError.Average(), w.phaseCpuError.Average(),
            w.visibleEntities.Average(), w.visibleEntities.maximum,
            w.staticTests.Average(), w.dynamicTests.Average(),
            w.behindTests.Average(), w.lastStereo.eyeWidth,
            w.lastStereo.eyeHeight, w.lastStereo.renderScalePercent,
            w.dynamicMatrixCalls.Average(),
            w.dynamicWidenAccepts.Average(), w.dynamicFallbackVisible.Average(),
            w.dynamicFallbackCulled.Average(), w.dynamicWiden60To80.Average(),
            w.dynamicWidenBehind.Average(), w.sphere45Shortcuts.Average(),
            w.cullAttributionFaults,
            w.visibleLods.Average(), w.visibleLods.maximum,
            w.visibleSuperLods.Average(), w.visibleSuperLods.maximum,
            w.streamingRequests.Average(), w.streamingPriorityRequests.Average(),
            w.streamingMemoryUsedMiB.Average(), w.streamingMemoryAvailableMiB.Average(),
            w.lodScale.Average(), w.cameraLodMultiplier.Average(),
            w.cameraGenerationMultiplier.Average(), w.cameraFov.Average(),
            w.farClip.Average());
        std::fprintf(file,
            ",%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,"
            "%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%d",
            w.freshStereoFrames > 0 && w.lastStereo.carPopulationValid ? 1 : 0,
            w.carRandom.Average(), w.carRandom.maximum, w.carLaw.Average(),
            w.carMission.Average(), w.carParked.Average(),
            w.carPermanent.Average(), w.carMax.Average(), w.carDensity.Average(),
            w.freshStereoFrames > 0 && w.lastStereo.pedPopulationValid ? 1 : 0,
            w.pedTotal.Average(), w.pedTotal.maximum, w.pedCiv.Average(),
            w.pedGang.Average(), w.pedMission.Average(),
            w.pedCarPassenger.Average(), w.pedMax.Average(), w.pedDensity.Average(),
            w.freshStereoFrames > 0 && w.lastStereo.lodWitnessValid ? 1 : 0);
        std::fprintf(file,
            ",%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%d,%d,%d,"
            "%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f",
            w.freshStereoFrames > 0 && w.lastStereo.lodHandoffHookActive ? 1 : 0,
            w.linkedEntityTests.Average(), w.linkedEntityLoaded.Average(),
            w.linkedResultVisible.Average(), w.linkedResultCulled.Average(),
            w.linkedResultStream.Average(), w.linkedNearThreshold.Average(),
            w.handoffCandidateValid ? w.handoffModelId : -1,
            w.handoffCandidateValid ? w.handoffDistance : -1.0,
            w.handoffCandidateValid ? w.handoffThreshold : -1.0,
            w.handoffCandidateValid ? w.handoffResult : -1,
            w.handoffCandidateValid && w.handoffLoaded ? 1 : 0,
            vehicleWitnessValid ? 1 : 0,
            w.vehicleMultiPassM.Average(), w.vehicleLod0M.Average(),
            w.vehicleLod1M.Average(), w.vehicleBigLod0M.Average(),
            w.freshStereoFrames > 0 && w.lastStereo.lodPrefetchActive ? 1 : 0,
            w.freshStereoFrames > 0 ? w.lastStereo.lodPrefetchFactor : 0.0,
            w.prefetchBand.Average(), w.prefetchEnqueues.Average(),
            w.linkedNearReady.Average(), w.linkedNearMissing.Average());
        std::fprintf(file,
            ",%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%d,%.2f,%d,%.2f,%.2f,%d,"
            "%d,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%d,%d,%d",
            w.prefetchStreamMe.Average(), w.prefetchRequestCalls.Average(),
            w.prefetchAlreadyPending.Average(), w.prefetchThrottled.Average(),
            w.prefetchStateAnomalies.Average(),
            w.freshStereoFrames > 0 && w.lastStereo.vehicleLodOverrideActive ? 1 : 0,
            w.freshStereoFrames > 0 ? w.lastStereo.vehicleLodTargetM : 0.0,
            w.freshStereoFrames > 0 && w.lastStereo.vehicleLodSampleHookActive ? 1 : 0,
            w.vehicleSampleCalls.Average(), w.vehicleCandidateValid ? 1 : 0,
            w.vehicleCandidateValid ? w.vehicleNearDistance : -1.0,
            w.vehicleCandidateValid ? w.vehicleNearThreshold : -1.0,
            w.vehicleCandidateValid && w.vehicleNearHigh ? 1 : 0,
            w.freshStereoFrames > 0 && w.lastStereo.streetPropFloorActive ? 1 : 0,
            w.freshStereoFrames > 0 ? w.lastStereo.streetPropFloorM : 0.0,
            w.unlinkedShortTests.Average(), w.streetPropTests.Average(),
            w.streetPropWrites.Average(),
            w.propCandidateValid ? w.propModelId : -1,
            w.propCandidateValid ? w.propDistance : -1.0,
            w.propCandidateValid ? w.propStockThreshold : -1.0,
            w.propCandidateValid ? w.propAppliedThreshold : -1.0,
            w.propCandidateValid ? w.propResult : -1,
            w.propCandidateValid && w.propLoaded ? 1 : 0,
            w.propCandidateValid && w.propTargeted ? 1 : 0);
        std::fprintf(file,
            ",%d,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,"
            "%d,%u,%u,%u,%u,%d,%d,%u",
            trafficCensus.valid ? 1 : 0, trafficCensus.live,
            trafficCensus.random, trafficCensus.mission, trafficCensus.parked,
            trafficCensus.permanent, trafficCensus.unknown, trafficCensus.law,
            static_cast<unsigned long long>(trafficBirthsWindow),
            static_cast<unsigned long long>(trafficDeathsWindow),
            static_cast<unsigned long long>(trafficCensus.birthsTotal),
            static_cast<unsigned long long>(trafficCensus.deathsTotal),
            static_cast<unsigned long long>(trafficCensus.samples),
            trafficCensus.localViewValid ? 1 : 0,
            trafficCensus.localRandomNear,
            trafficCensus.localRandomForward,
            trafficCensus.localRandomUseful,
            trafficCensus.localLawUseful,
            trafficCensus.directorBaseTarget,
            trafficCensus.directorEffectiveTarget,
            trafficCensus.directorWantedLevel);
        std::fprintf(file,
            ",%.2f,%.2f,%d,%d,%d,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,"
            "%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.1f,%d,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,"
            "%.1f,%.2f,%.2f,%.2f,%.2f,"
            "%d,%.2f",
            w.buildingDetailTests.Average(),
            w.buildingDetailOverrides.Average(),
            w.modelDrawRestoreFaults,
            w.freshStereoFrames > 0 &&
                    w.lastStereo.ambientCarGateActive ? 1 : 0,
            w.freshStereoFrames > 0 ? w.lastStereo.ambientCarTarget : 0,
            w.ambientCarAttempts.Average(), w.ambientCarBlocked.Average(),
            w.ambientCarWantedPasses.Average(),
            w.freshStereoFrames > 0 ? w.lastStereo.pedAmbientCap : 0,
            w.pedAmbientAttempts.Average(),
            w.pedAmbientSuccesses.Average(),
            w.pedAmbientBlocked.Average(),
            w.freshStereoFrames > 0 ? w.lastStereo.staticSafetyRadiusM : 0.0,
            w.staticSafetyTests.Average(),
            w.staticSafetyStockVisible.Average(),
            w.staticSafetyAccepts.Average(),
            w.staticSafety45To60.Average(),
            w.staticSafety60To80.Average(),
            w.staticSafetyBuildingAccepts.Average(),
            w.staticSafetyDummyAccepts.Average(),
            w.freshStereoFrames > 0
                ? w.lastStereo.staticCullConeHalfAngleDeg : 0.0,
            w.freshStereoFrames > 0 &&
                    w.lastStereo.staticCullConeValid ? 1 : 0,
            w.staticSafetyConeRejects.Average(),
            w.freshStereoFrames > 0
                ? w.lastStereo.largeVegetationSafetyRadiusM : 0.0,
            w.largeVegetationSafetyTests.Average(),
            w.largeVegetationSafetyStockVisible.Average(),
            w.largeVegetationSafetyAccepts.Average(),
            w.largeVegetationSafetyConeRejects.Average(),
            w.freshStereoFrames > 0
                ? w.lastStereo.roadsideSafetyRadiusM : 0.0,
            w.roadsideSafetyTests.Average(),
            w.roadsideSafetyStockVisible.Average(),
            w.roadsideSafetyAccepts.Average(),
            w.roadsideSafetyConeRejects.Average(),
            w.freshStereoFrames > 0 &&
                    w.lastStereo.targetedOcclusionHookActive ? 1 : 0,
            w.targetedOcclusionBypasses.Average());
        std::fprintf(file,
            ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,"
            "%d,%.1f,%.2f,%.2f,%d,%.2f,%.4f,%.4f",
            w.targetedOcclusionTests.Average(),
            w.targetedOcclusionStockVisible.Average(),
            w.targetedOcclusionStockOccluded.Average(),
            w.targetedOcclusionConeBypasses.Average(),
            w.targetedOcclusionRadialBypasses.Average(),
            w.targetedOcclusionRecentBypasses.Average(),
            w.targetedOcclusionStockCulls.Average(),
            w.targetedOcclusionInvalidPoseBypasses.Average(),
            w.targetedOcclusionCacheHits.Average(),
            w.targetedOcclusionCacheMisses.Average(),
            w.targetedOcclusionCacheEvictions.Average(),
            w.targetedOcclusionCacheOverflows.Average(),
            w.targetedOcclusionIdentityResets.Average(),
            w.freshStereoFrames > 0 && w.lastStereo.nearbyScanActive ? 1 : 0,
            w.freshStereoFrames > 0 ? w.lastStereo.nearbyScanRadiusM : 0.0,
            w.nearbyScanSectors.Average(),
            w.nearbyScanVisibleAdded.Average(),
            w.freshStereoFrames > 0 &&
                    w.lastStereo.nearbyScanHmdHeadingActive ? 1 : 0,
            w.nearbyScanStreamingRequests.Average(),
            w.nearbyScanWallMs.Average(),
            w.nearbyScanCpuMs.Average());
        std::fprintf(file,
            ",%d,%d,%d,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%d,"
            "%.2f,%.4f,%.4f,%.4f,%.4f,"
            "%.2f,%.4f,%.4f,%.4f,%.4f,"
            "%.2f,%.2f,%.2f,"
            "%.2f,%.4f,%.4f,%.2f,%.4f,%.4f,%.2f,%.4f,%.4f,"
            "%.1f,%d",
            w.enginePreBreakdownValidFrames > 0 &&
                    w.enginePreBreakdownFaults == 0 ? 1 : 0,
            w.enginePreBreakdownValidFrames,
            w.enginePreBreakdownFaults,
            w.enginePreBeforeScanWall.Average(),
            w.enginePreBeforeScanCpu.Average(),
            w.enginePreMainScanWall.Average(),
            w.enginePreMainScanCpu.Average(),
            w.enginePreStockScanWall.Average(),
            w.enginePreStockScanCpu.Average(),
            w.enginePreAfterScanWall.Average(),
            w.enginePreAfterScanCpu.Average(),
            w.enginePreBreakdownResidualWall.Average(),
            w.enginePreBreakdownResidualCpu.Average(),
            w.freshStereoFrames > 0 &&
                    w.lastStereo.renderQueueWaitHookActive ? 1 : 0,
            w.renderQueueFinishCalls.Average(),
            w.renderQueueFinishWall.Average(),
            w.renderQueueFinishCpu.Average(),
            std::max(0.0, w.renderQueueFinishWall.Average() -
                              w.renderQueueFinishCpu.Average()),
            w.renderQueueFinishMaxWall.maximum,
            w.renderQueueFlushCalls.Average(),
            w.renderQueueFlushWall.Average(),
            w.renderQueueFlushCpu.Average(),
            std::max(0.0, w.renderQueueFlushWall.Average() -
                              w.renderQueueFlushCpu.Average()),
            w.renderQueueFlushMaxWall.maximum,
            w.renderQueueSizedFlushCalls.Average(),
            w.renderQueueNearEndFlushCalls.Average(),
            w.renderQueueExplicitFlushCalls.Average(),
            w.renderQueueWaitBeforeScanCalls.Average(),
            w.renderQueueWaitBeforeScanWall.Average(),
            w.renderQueueWaitBeforeScanCpu.Average(),
            w.renderQueueWaitMainScanCalls.Average(),
            w.renderQueueWaitMainScanWall.Average(),
            w.renderQueueWaitMainScanCpu.Average(),
            w.renderQueueWaitAfterScanCalls.Average(),
            w.renderQueueWaitAfterScanWall.Average(),
            w.renderQueueWaitAfterScanCpu.Average(),
            w.renderQueueWaitMaxUsedKiB.maximum,
            w.renderQueueWaitClassificationFaults);
        std::fprintf(file,
            ",%d,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d",
            w.freshStereoFrames > 0 &&
                    w.lastStereo.renderQueueFinishDeferActive ? 1 : 0,
            w.renderQueueFinishRequestCalls.Average(),
            w.renderQueueFinishDeferredCalls.Average(),
            w.renderQueueFinishDrainCalls.Average(),
            w.renderQueueFinishFallbackCalls.Average(),
            w.renderQueueFinishOverlapWall.Average(),
            w.renderQueueFinishDrainWall.Average(),
            w.renderQueueFinishDrainCpu.Average(),
            std::max(0.0, w.renderQueueFinishDrainWall.Average() -
                              w.renderQueueFinishDrainCpu.Average()),
            w.renderQueueFinishDrainMaxWall.maximum,
            static_cast<int>(w.renderQueueFinishPendingDepthMax.maximum),
            w.renderQueueFinishPendingFaults);
        std::fprintf(file,
            ",%d,%.2f,%.2f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            "%.2f\n",
            w.aircraftLodActiveFrames > 0 ? 1 : 0,
            w.aircraftAltitudeM.Average(),
            w.aircraftOrdinaryMaxSlantM.Average(),
            w.aircraftHeadForwardZCount > 0
                ? w.aircraftHeadForwardZSum /
                      static_cast<double>(w.aircraftHeadForwardZCount)
                : std::numeric_limits<double>::quiet_NaN(),
            w.aircraftOrdinaryProbes.Average(),
            w.aircraftOrdinaryQualified.Average(),
            w.aircraftOrdinaryForcedVisible.Average(),
            w.aircraftOrdinaryRangePromotions.Average(),
            w.aircraftOrdinaryHorizontalRejects.Average(),
            w.aircraftOrdinarySlantRejects.Average(),
            w.aircraftOrdinaryConeRejects.Average(),
            w.aircraftOrdinaryForceLimit.Average(),
            w.aircraftOrdinaryCapacity.Average(),
            w.aircraftOrdinaryOcclusionBypasses.Average(),
            w.aircraftOrdinaryOcclusionUnconsumed.Average(),
            w.aircraftOrdinaryInnerSlantRejects.Average(),
            w.aircraftOrdinaryUnloadedRootlessForward.Average(),
            w.aircraftOrdinaryPrefetchCandidates.Average(),
            w.aircraftOrdinaryPrefetchUnique.Average(),
            w.aircraftOrdinaryPrefetchRequestCalls.Average(),
            w.aircraftOrdinaryPrefetchEnqueues.Average(),
            w.aircraftOrdinaryPrefetchAlreadyPending.Average(),
            w.aircraftOrdinaryPrefetchQueueBlocked.Average(),
            w.aircraftOrdinaryPrefetchBudgetLimited.Average(),
            w.aircraftOrdinaryPrefetchStateAnomalies.Average(),
            w.aircraftOrdinaryOuterActive.Average(),
            w.aircraftOrdinaryOuterRadiusM.Average(),
            w.aircraftOrdinaryOuterCandidateSectors.Average(),
            w.aircraftOrdinaryOuterScannedSectors.Average(),
            w.aircraftOrdinaryOuterConeSkippedSectors.Average(),
            w.aircraftOrdinaryOuterSectorLimit.Average(),
            w.aircraftOrdinaryOuterProbes.Average(),
            w.aircraftOrdinaryOuterQualified.Average(),
            w.aircraftOrdinaryOuterUnloaded.Average(),
            w.aircraftOrdinaryOuterAdmitted.Average(),
            w.aircraftOrdinaryOuterForceStops.Average(),
            w.aircraftOrdinaryOuterFaults.Average(),
            w.aircraftOrdinaryOuterWallMs.Average(),
            w.aircraftOrdinaryOuterCpuMs.Average(),
            w.aircraftOrdinaryOuterPhase1InnerPromotions.Average(),
            w.aircraftOrdinaryOuterPhase1OuterPromotions.Average(),
            w.aircraftOrdinaryOuterPhase1InnerAdmissions.Average(),
            w.aircraftOrdinaryOuterPhase1OuterAdmissions.Average(),
            w.aircraftOrdinaryOuterPhase1InnerPromotionStops.Average(),
            w.aircraftOrdinaryOuterPhase1OuterPromotionStops.Average(),
            w.aircraftOrdinaryOuterPhase1InnerAdmissionStops.Average(),
            w.aircraftOrdinaryOuterPhase1OuterAdmissionStops.Average(),
            w.aircraftOrdinaryPhase2BackfillAdmissions.Average(),
            w.aircraftOrdinaryInnerLimit.Average(),
            w.aircraftOrdinaryOuterLimit.Average(),
            w.aircraftOrdinaryOuterNearLimit.Average(),
            w.aircraftOrdinaryOuterFarLimit.Average(),
            w.aircraftOrdinaryTotalLimit.Average(),
            w.aircraftOrdinaryOuterNearPromotions.Average(),
            w.aircraftOrdinaryOuterFarPromotions.Average(),
            w.aircraftOrdinaryOuterNearAdmissions.Average(),
            w.aircraftOrdinaryOuterFarAdmissions.Average(),
            w.aircraftOrdinaryOuterNearPromotionStops.Average(),
            w.aircraftOrdinaryOuterFarPromotionStops.Average(),
            w.aircraftOrdinaryOuterNearAdmissionStops.Average(),
            w.aircraftOrdinaryOuterFarAdmissionStops.Average(),
            w.aircraftOrdinaryOuterSessionDisabled.Average(),
            w.aircraftOrdinaryOuterRadialCandidates.Average(),
            w.aircraftOrdinaryOuterRadialInnerCandidates.Average(),
            w.aircraftOrdinaryOuterRadialOuterCandidates.Average(),
            w.aircraftOrdinaryOuterRadialSelectedInner.Average(),
            w.aircraftOrdinaryOuterRadialSelectedOuter.Average(),
            w.aircraftOrdinaryOuterRadialRetainedInner.Average(),
            w.aircraftOrdinaryOuterRadialRetainedOuter.Average(),
            w.aircraftOrdinaryOuterRadialReplayRequested.Average(),
            w.aircraftOrdinaryOuterRadialReplayVisited.Average(),
            w.aircraftOrdinaryOuterRadialReplayCompleted.Average(),
            w.aircraftOrdinaryOuterRadialReplayMisses.Average(),
            w.aircraftOrdinaryOuterRadialReplaySectors.Average(),
            w.aircraftOrdinaryOuterRadialReplayVisible.Average(),
            w.aircraftOrdinaryOuterRadialReplayCulled.Average(),
            w.aircraftOrdinaryOuterRadialReplayStream.Average(),
            w.aircraftOrdinaryOuterRadialReplayOther.Average(),
            w.aircraftOrdinaryOuterRadialCaptureOverflow.Average(),
            w.aircraftOrdinaryOuterRadialSectorOverflow.Average());
        static int rowsSinceFlush = 0;
        if (++rowsSinceFlush >= 5) {
            std::fflush(file);
            rowsSinceFlush = 0;
        }
    }
}

int g_lastSubmittedStereoSequence = -1;

struct PresentWindow {
    double startMs{};
    int attempts{};
    int successfulPresents{};
    int endFailures{};
    int generationRaces{};
    int freshSequences{};
    int repeatedSequences{};
    int noStereoSequence{};
    int sequenceJumps{};
    int stereoSyncWaits{};
    int stereoSyncRescued{};
    int stereoSyncTimeouts{};
    int noLayers{};
    int shouldNotRender{};
    int fxaaDraws{};
    int fxaaFallbacks{};
    int fxaaErrors{};
    Series loopPeriod;
    Series consumerUpdate;
    Series consumerUpdateCpu;
    Series waitFrame;
    Series work;
    Series endFrame;
    Series threadCpu;
    Series predictedDelta;
    Series stereoSyncWait;
    Series submittedStereoAge;
    Series fxaaSubmitWall;
    PresentFrameSample last{};
};

PresentWindow g_presentWindow{};
FILE* g_xrCsv = nullptr;
bool g_xrCsvAttempted = false;
std::array<char, 64 * 1024> g_xrCsvBuffer{};

FILE* OpenXrCsv() {
    if (g_xrCsv || g_xrCsvAttempted) return g_xrCsv;
    g_xrCsvAttempted = true;
    if (!ArchivePreviousCsv(kXrCsvPath, "XR")) {
        LOGW("[perf.init] preserving previous XR CSV; current capture disabled");
        return nullptr;
    }
    g_xrCsv = std::fopen(kXrCsvPath, "w");
    if (!g_xrCsv) {
        LOGW("[perf.init] cannot open %s", kXrCsvPath);
        return nullptr;
    }
    std::setvbuf(g_xrCsv, g_xrCsvBuffer.data(), _IOFBF, g_xrCsvBuffer.size());
    std::fprintf(g_xrCsv,
        "window_s,attempts,presented,present_hz,end_failures,last_end_result,ring_races,"
        "fresh,repeated,no_stereo,seq_jumps,no_layers,should_not_render,theater,"
        "sync_waits,sync_rescued,sync_timeouts,sync_wait_avg,sync_wait_p95,sync_wait_max,"
        "stereo_age_avg,stereo_age_p95,stereo_age_max,"
        "display_valid,display_hz,budget_ms,meta_cpu_valid,meta_cpu_ms,meta_gpu_valid,meta_gpu_ms,"
        "loop_avg,loop_p95,loop_max,pred_avg,pred_p95,pred_max,"
        "update_avg,update_p95,update_max,update_cpu_avg,"
        "wait_avg,wait_p95,wait_max,work_avg,work_p95,work_max,"
        "end_avg,end_p95,end_max,present_thread_cpu_avg,"
        "fxaa_requested,fxaa_active,fxaa_draws,fxaa_fallbacks,fxaa_errors,"
        "fxaa_submit_avg,fxaa_submit_p95,fxaa_submit_max\n");
    LOGI("[perf.init] XR summary CSV: %s", kXrCsvPath);
    return g_xrCsv;
}

void EmitPresentWindow(double endMs) {
    PresentWindow& w = g_presentWindow;
    if (w.attempts == 0 || w.startMs <= 0.0 || endMs <= w.startMs) return;
    const double seconds = (endMs - w.startMs) / 1000.0;
    const double presentHz = w.successfulPresents / seconds;
    const float metaCpu = w.last.runtimeCpuValid ? w.last.runtimeCpuMs : -1.0f;
    const float metaGpu = w.last.runtimeGpuValid ? w.last.runtimeGpuMs : -1.0f;
    const double budgetMs = w.last.displayRefreshValid && w.last.displayRefreshHz > 0.1f
        ? 1000.0 / static_cast<double>(w.last.displayRefreshHz) : 0.0;

    {
    std::lock_guard<std::mutex> debugLock(g_debugStatsMutex);
    g_debugStats.presentHz.store(presentHz, std::memory_order_relaxed);
    g_debugStats.displayValid.store(
        w.last.displayRefreshValid, std::memory_order_relaxed);
    g_debugStats.displayHz.store(w.last.displayRefreshHz, std::memory_order_relaxed);
    g_debugStats.budgetMs.store(budgetMs, std::memory_order_relaxed);
    g_debugStats.runtimeCpuValid.store(w.last.runtimeCpuValid, std::memory_order_relaxed);
    g_debugStats.runtimeCpuMs.store(
        w.last.runtimeCpuValid ? w.last.runtimeCpuMs : 0.0,
        std::memory_order_relaxed);
    g_debugStats.runtimeGpuValid.store(w.last.runtimeGpuValid, std::memory_order_relaxed);
    g_debugStats.runtimeGpuMs.store(
        w.last.runtimeGpuValid ? w.last.runtimeGpuMs : 0.0,
        std::memory_order_relaxed);
    g_debugStats.endFailures.store(w.endFailures, std::memory_order_relaxed);
    g_debugStats.ringRaces.store(w.generationRaces, std::memory_order_relaxed);
    g_debugStats.fresh.store(w.freshSequences, std::memory_order_relaxed);
    g_debugStats.repeated.store(w.repeatedSequences, std::memory_order_relaxed);
    g_debugStats.stereoSyncWaits.store(w.stereoSyncWaits,
                                      std::memory_order_relaxed);
    g_debugStats.stereoSyncRescued.store(w.stereoSyncRescued,
                                        std::memory_order_relaxed);
    g_debugStats.stereoSyncTimeouts.store(w.stereoSyncTimeouts,
                                         std::memory_order_relaxed);
    g_debugStats.stereoSyncWaitMs.store(w.stereoSyncWait.Average(),
                                       std::memory_order_relaxed);
    g_debugStats.fxaaRequested.store(w.last.fxaaRequested, std::memory_order_relaxed);
    g_debugStats.fxaaActive.store(w.last.fxaaActive, std::memory_order_relaxed);
    g_debugStats.fxaaDraws.store(w.fxaaDraws, std::memory_order_relaxed);
    g_debugStats.fxaaFallbacks.store(w.fxaaFallbacks, std::memory_order_relaxed);
    g_debugStats.fxaaErrors.store(w.fxaaErrors, std::memory_order_relaxed);
    g_debugStats.fxaaSubmitWallMs.store(
        w.fxaaSubmitWall.Average(), std::memory_order_relaxed);
    g_debugStats.presentValid.store(true, std::memory_order_relaxed);
    g_debugStats.revision.fetch_add(1, std::memory_order_release);
    }

    LOGI("[perf.xr] v2 win=%.2fs present=%d(%.1fHz) attempts=%d endFail=%d lastEnd=%d ringRace=%d "
         "fresh=%d repeat=%d "
         "noStereo=%d jumps=%d layers0=%d should0=%d theater=%d display=%d/%.1fHz "
         "budget=%.2f metaCPU=%.2f metaGPU=%.2f",
         seconds, w.successfulPresents, presentHz, w.attempts,
         w.endFailures, w.last.endResult, w.generationRaces, w.freshSequences,
         w.repeatedSequences, w.noStereoSequence, w.sequenceJumps,
         w.noLayers, w.shouldNotRender, w.last.theaterMode ? 1 : 0,
         w.last.displayRefreshValid ? 1 : 0, w.last.displayRefreshHz, budgetMs,
         metaCpu, metaGpu);
    LOGI("[perf.xr.fxaa] requested=%d active=%d draws=%d fallback=%d errors=%d "
         "submitW=%.3f/%.3f/%.3fms (CPU/driver)",
         w.last.fxaaRequested ? 1 : 0, w.last.fxaaActive ? 1 : 0,
         w.fxaaDraws, w.fxaaFallbacks, w.fxaaErrors,
         w.fxaaSubmitWall.Average(), w.fxaaSubmitWall.P95(),
         w.fxaaSubmitWall.maximum);
    LOGI("[perf.xr.sync] wait/rescue/timeout=%d/%d/%d "
         "wait=%.3f/%.3f/%.3fms age=%.2f/%.2f/%.2fms",
         w.stereoSyncWaits, w.stereoSyncRescued, w.stereoSyncTimeouts,
         w.stereoSyncWait.Average(), w.stereoSyncWait.P95(),
         w.stereoSyncWait.maximum, w.submittedStereoAge.Average(),
         w.submittedStereoAge.P95(), w.submittedStereoAge.maximum);
    LOGI("[perf.xr.t] loop=%.2f/%.2f/%.2f pred=%.2f/%.2f/%.2f "
         "updateW=%.2f/%.2f/%.2f updateCPU=%.2f "
         "wait=%.2f/%.2f/%.2f work=%.2f/%.2f/%.2f "
         "end=%.2f/%.2f/%.2f threadCPU=%.2f",
         w.loopPeriod.Average(), w.loopPeriod.P95(), w.loopPeriod.maximum,
         w.predictedDelta.Average(), w.predictedDelta.P95(), w.predictedDelta.maximum,
         w.consumerUpdate.Average(), w.consumerUpdate.P95(), w.consumerUpdate.maximum,
         w.consumerUpdateCpu.Average(),
         w.waitFrame.Average(), w.waitFrame.P95(), w.waitFrame.maximum,
         w.work.Average(), w.work.P95(), w.work.maximum,
         w.endFrame.Average(), w.endFrame.P95(), w.endFrame.maximum,
         w.threadCpu.Average());

    if (FILE* file = OpenXrCsv()) {
        std::fprintf(file,
            "%.3f,%d,%d,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
            "%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%d,%.3f,%.4f,%d,%.4f,%d,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,"
            "%d,%d,%d,%d,%d,%.4f,%.4f,%.4f\n",
            seconds, w.attempts, w.successfulPresents, presentHz,
            w.endFailures, w.last.endResult, w.generationRaces,
            w.freshSequences,
            w.repeatedSequences, w.noStereoSequence, w.sequenceJumps,
            w.noLayers, w.shouldNotRender, w.last.theaterMode ? 1 : 0,
            w.stereoSyncWaits, w.stereoSyncRescued, w.stereoSyncTimeouts,
            w.stereoSyncWait.Average(), w.stereoSyncWait.P95(),
            w.stereoSyncWait.maximum, w.submittedStereoAge.Average(),
            w.submittedStereoAge.P95(), w.submittedStereoAge.maximum,
            w.last.displayRefreshValid ? 1 : 0, w.last.displayRefreshHz, budgetMs,
            w.last.runtimeCpuValid ? 1 : 0, metaCpu,
            w.last.runtimeGpuValid ? 1 : 0, metaGpu,
            w.loopPeriod.Average(), w.loopPeriod.P95(), w.loopPeriod.maximum,
            w.predictedDelta.Average(), w.predictedDelta.P95(), w.predictedDelta.maximum,
            w.consumerUpdate.Average(), w.consumerUpdate.P95(), w.consumerUpdate.maximum,
            w.consumerUpdateCpu.Average(),
            w.waitFrame.Average(), w.waitFrame.P95(), w.waitFrame.maximum,
            w.work.Average(), w.work.P95(), w.work.maximum,
            w.endFrame.Average(), w.endFrame.P95(), w.endFrame.maximum,
            w.threadCpu.Average(),
            w.last.fxaaRequested ? 1 : 0, w.last.fxaaActive ? 1 : 0,
            w.fxaaDraws, w.fxaaFallbacks, w.fxaaErrors,
            w.fxaaSubmitWall.Average(), w.fxaaSubmitWall.P95(),
            w.fxaaSubmitWall.maximum);
        static int rowsSinceFlush = 0;
        if (++rowsSinceFlush >= 5) {
            std::fflush(file);
            rowsSinceFlush = 0;
        }
    }
}

} // namespace

double MonotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1e6;
}

double ThreadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1e6;
}

std::uint64_t RenderSceneSerial() {
    return g_renderSceneSerial.load(std::memory_order_relaxed);
}

void NotifyRenderSceneEntry() {
    g_renderSceneSerial.fetch_add(1, std::memory_order_relaxed);
}

DebugStatsSnapshot GetDebugStats() {
    std::lock_guard<std::mutex> debugLock(g_debugStatsMutex);
    DebugStatsSnapshot snapshot{};
    snapshot.gameValid = g_debugStats.gameValid.load(std::memory_order_relaxed);
    snapshot.callbackHz = g_debugStats.callbackHz.load(std::memory_order_relaxed);
    snapshot.renderedHz = g_debugStats.renderedHz.load(std::memory_order_relaxed);
    snapshot.frameWallMs = g_debugStats.frameWallMs.load(std::memory_order_relaxed);
    snapshot.frameCpuMs = g_debugStats.frameCpuMs.load(std::memory_order_relaxed);
    snapshot.frameBlockedMs = g_debugStats.frameBlockedMs.load(std::memory_order_relaxed);
    snapshot.renderWallMs = g_debugStats.renderWallMs.load(std::memory_order_relaxed);
    snapshot.renderCpuMs = g_debugStats.renderCpuMs.load(std::memory_order_relaxed);
    snapshot.renderBlockedMs = g_debugStats.renderBlockedMs.load(std::memory_order_relaxed);
    snapshot.recordWallMs = g_debugStats.recordWallMs.load(std::memory_order_relaxed);
    snapshot.recordCpuMs = g_debugStats.recordCpuMs.load(std::memory_order_relaxed);
    snapshot.enginePreWallMs =
        g_debugStats.enginePreWallMs.load(std::memory_order_relaxed);
    snapshot.enginePreCpuMs =
        g_debugStats.enginePreCpuMs.load(std::memory_order_relaxed);
    snapshot.enginePreBreakdownValid =
        g_debugStats.enginePreBreakdownValid.load(std::memory_order_relaxed);
    snapshot.enginePreBreakdownFaults =
        g_debugStats.enginePreBreakdownFaults.load(std::memory_order_relaxed);
    snapshot.enginePreBeforeScanWallMs =
        g_debugStats.enginePreBeforeScanWallMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreBeforeScanCpuMs =
        g_debugStats.enginePreBeforeScanCpuMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreMainScanWallMs =
        g_debugStats.enginePreMainScanWallMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreMainScanCpuMs =
        g_debugStats.enginePreMainScanCpuMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreStockScanWallMs =
        g_debugStats.enginePreStockScanWallMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreStockScanCpuMs =
        g_debugStats.enginePreStockScanCpuMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreAfterScanWallMs =
        g_debugStats.enginePreAfterScanWallMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreAfterScanCpuMs =
        g_debugStats.enginePreAfterScanCpuMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreBreakdownResidualWallMs =
        g_debugStats.enginePreBreakdownResidualWallMs.load(
            std::memory_order_relaxed);
    snapshot.enginePreBreakdownResidualCpuMs =
        g_debugStats.enginePreBreakdownResidualCpuMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueWaitHookActive =
        g_debugStats.renderQueueWaitHookActive.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishCalls =
        g_debugStats.renderQueueFinishCalls.load(std::memory_order_relaxed);
    snapshot.renderQueueFinishWallMs =
        g_debugStats.renderQueueFinishWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishCpuMs =
        g_debugStats.renderQueueFinishCpuMs.load(std::memory_order_relaxed);
    snapshot.renderQueueFinishBlockedMs =
        g_debugStats.renderQueueFinishBlockedMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishMaxWallMs =
        g_debugStats.renderQueueFinishMaxWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFlushCalls =
        g_debugStats.renderQueueFlushCalls.load(std::memory_order_relaxed);
    snapshot.renderQueueFlushWallMs =
        g_debugStats.renderQueueFlushWallMs.load(std::memory_order_relaxed);
    snapshot.renderQueueFlushCpuMs =
        g_debugStats.renderQueueFlushCpuMs.load(std::memory_order_relaxed);
    snapshot.renderQueueFlushBlockedMs =
        g_debugStats.renderQueueFlushBlockedMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFlushMaxWallMs =
        g_debugStats.renderQueueFlushMaxWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueWaitBeforeScanWallMs =
        g_debugStats.renderQueueWaitBeforeScanWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueWaitMainScanWallMs =
        g_debugStats.renderQueueWaitMainScanWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueWaitAfterScanWallMs =
        g_debugStats.renderQueueWaitAfterScanWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueWaitMaxUsedKiB =
        g_debugStats.renderQueueWaitMaxUsedKiB.load(
            std::memory_order_relaxed);
    snapshot.renderQueueSizedFlushCalls =
        g_debugStats.renderQueueSizedFlushCalls.load(
            std::memory_order_relaxed);
    snapshot.renderQueueNearEndFlushCalls =
        g_debugStats.renderQueueNearEndFlushCalls.load(
            std::memory_order_relaxed);
    snapshot.renderQueueExplicitFlushCalls =
        g_debugStats.renderQueueExplicitFlushCalls.load(
            std::memory_order_relaxed);
    snapshot.renderQueueWaitClassificationFaults =
        g_debugStats.renderQueueWaitClassificationFaults.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishDeferActive =
        g_debugStats.renderQueueFinishDeferActive.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishRequestCalls =
        g_debugStats.renderQueueFinishRequestCalls.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishDeferredCalls =
        g_debugStats.renderQueueFinishDeferredCalls.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishDrainCalls =
        g_debugStats.renderQueueFinishDrainCalls.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishFallbackCalls =
        g_debugStats.renderQueueFinishFallbackCalls.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishOverlapWallMs =
        g_debugStats.renderQueueFinishOverlapWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishDrainWallMs =
        g_debugStats.renderQueueFinishDrainWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishDrainCpuMs =
        g_debugStats.renderQueueFinishDrainCpuMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishDrainBlockedMs =
        g_debugStats.renderQueueFinishDrainBlockedMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishDrainMaxWallMs =
        g_debugStats.renderQueueFinishDrainMaxWallMs.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishPendingDepthMax =
        g_debugStats.renderQueueFinishPendingDepthMax.load(
            std::memory_order_relaxed);
    snapshot.renderQueueFinishPendingFaults =
        g_debugStats.renderQueueFinishPendingFaults.load(
            std::memory_order_relaxed);
    snapshot.stereoPrepareWallMs =
        g_debugStats.stereoPrepareWallMs.load(std::memory_order_relaxed);
    snapshot.stereoPrepareCpuMs =
        g_debugStats.stereoPrepareCpuMs.load(std::memory_order_relaxed);
    snapshot.stereoTailWallMs =
        g_debugStats.stereoTailWallMs.load(std::memory_order_relaxed);
    snapshot.stereoTailCpuMs =
        g_debugStats.stereoTailCpuMs.load(std::memory_order_relaxed);
    snapshot.enginePostWallMs =
        g_debugStats.enginePostWallMs.load(std::memory_order_relaxed);
    snapshot.enginePostCpuMs =
        g_debugStats.enginePostCpuMs.load(std::memory_order_relaxed);
    snapshot.sceneLeftWallMs =
        g_debugStats.sceneLeftWallMs.load(std::memory_order_relaxed);
    snapshot.sceneRightWallMs =
        g_debugStats.sceneRightWallMs.load(std::memory_order_relaxed);
    snapshot.sceneLeftCpuMs =
        g_debugStats.sceneLeftCpuMs.load(std::memory_order_relaxed);
    snapshot.sceneRightCpuMs =
        g_debugStats.sceneRightCpuMs.load(std::memory_order_relaxed);
    snapshot.entities = g_debugStats.entities.load(std::memory_order_relaxed);
    snapshot.visibleLods = g_debugStats.visibleLods.load(std::memory_order_relaxed);
    snapshot.visibleSuperLods =
        g_debugStats.visibleSuperLods.load(std::memory_order_relaxed);
    snapshot.streamingRequests =
        g_debugStats.streamingRequests.load(std::memory_order_relaxed);
    snapshot.streamingPriorityRequests =
        g_debugStats.streamingPriorityRequests.load(std::memory_order_relaxed);
    snapshot.streamingMemoryUsedMiB =
        g_debugStats.streamingMemoryUsedMiB.load(std::memory_order_relaxed);
    snapshot.streamingMemoryAvailableMiB =
        g_debugStats.streamingMemoryAvailableMiB.load(std::memory_order_relaxed);
    snapshot.lodScale = g_debugStats.lodScale.load(std::memory_order_relaxed);
    snapshot.cameraLodMultiplier =
        g_debugStats.cameraLodMultiplier.load(std::memory_order_relaxed);
    snapshot.cameraGenerationMultiplier =
        g_debugStats.cameraGenerationMultiplier.load(std::memory_order_relaxed);
    snapshot.cameraFov = g_debugStats.cameraFov.load(std::memory_order_relaxed);
    snapshot.farClip = g_debugStats.farClip.load(std::memory_order_relaxed);
    snapshot.carPopulationValid =
        g_debugStats.carPopulationValid.load(std::memory_order_relaxed);
    snapshot.carRandom = g_debugStats.carRandom.load(std::memory_order_relaxed);
    snapshot.carRandomMax = g_debugStats.carRandomMax.load(std::memory_order_relaxed);
    snapshot.carLaw = g_debugStats.carLaw.load(std::memory_order_relaxed);
    snapshot.carMission = g_debugStats.carMission.load(std::memory_order_relaxed);
    snapshot.carParked = g_debugStats.carParked.load(std::memory_order_relaxed);
    snapshot.carPermanent = g_debugStats.carPermanent.load(std::memory_order_relaxed);
    snapshot.carMax = g_debugStats.carMax.load(std::memory_order_relaxed);
    snapshot.carDensity = g_debugStats.carDensity.load(std::memory_order_relaxed);
    snapshot.pedPopulationValid =
        g_debugStats.pedPopulationValid.load(std::memory_order_relaxed);
    snapshot.pedTotal = g_debugStats.pedTotal.load(std::memory_order_relaxed);
    snapshot.pedTotalMax = g_debugStats.pedTotalMax.load(std::memory_order_relaxed);
    snapshot.pedCiv = g_debugStats.pedCiv.load(std::memory_order_relaxed);
    snapshot.pedGang = g_debugStats.pedGang.load(std::memory_order_relaxed);
    snapshot.pedMission = g_debugStats.pedMission.load(std::memory_order_relaxed);
    snapshot.pedCarPassenger =
        g_debugStats.pedCarPassenger.load(std::memory_order_relaxed);
    snapshot.pedMax = g_debugStats.pedMax.load(std::memory_order_relaxed);
    snapshot.pedDensity = g_debugStats.pedDensity.load(std::memory_order_relaxed);
    snapshot.lodWitnessValid =
        g_debugStats.lodWitnessValid.load(std::memory_order_relaxed);
    snapshot.lodHandoffHookActive =
        g_debugStats.lodHandoffHookActive.load(std::memory_order_relaxed);
    snapshot.linkedEntityTests =
        g_debugStats.linkedEntityTests.load(std::memory_order_relaxed);
    snapshot.linkedEntityLoaded =
        g_debugStats.linkedEntityLoaded.load(std::memory_order_relaxed);
    snapshot.linkedResultVisible =
        g_debugStats.linkedResultVisible.load(std::memory_order_relaxed);
    snapshot.linkedResultCulled =
        g_debugStats.linkedResultCulled.load(std::memory_order_relaxed);
    snapshot.linkedResultStream =
        g_debugStats.linkedResultStream.load(std::memory_order_relaxed);
    snapshot.linkedNearThreshold =
        g_debugStats.linkedNearThreshold.load(std::memory_order_relaxed);
    snapshot.lodPrefetchActive =
        g_debugStats.lodPrefetchActive.load(std::memory_order_relaxed);
    snapshot.lodPrefetchFactor =
        g_debugStats.lodPrefetchFactor.load(std::memory_order_relaxed);
    snapshot.prefetchBand =
        g_debugStats.prefetchBand.load(std::memory_order_relaxed);
    snapshot.prefetchStreamMe =
        g_debugStats.prefetchStreamMe.load(std::memory_order_relaxed);
    snapshot.prefetchRequestCalls =
        g_debugStats.prefetchRequestCalls.load(std::memory_order_relaxed);
    snapshot.prefetchEnqueues =
        g_debugStats.prefetchEnqueues.load(std::memory_order_relaxed);
    snapshot.prefetchAlreadyPending =
        g_debugStats.prefetchAlreadyPending.load(std::memory_order_relaxed);
    snapshot.prefetchThrottled =
        g_debugStats.prefetchThrottled.load(std::memory_order_relaxed);
    snapshot.prefetchStateAnomalies =
        g_debugStats.prefetchStateAnomalies.load(std::memory_order_relaxed);
    snapshot.linkedNearReady =
        g_debugStats.linkedNearReady.load(std::memory_order_relaxed);
    snapshot.linkedNearMissing =
        g_debugStats.linkedNearMissing.load(std::memory_order_relaxed);
    snapshot.handoffModelId =
        g_debugStats.handoffModelId.load(std::memory_order_relaxed);
    snapshot.handoffDistance =
        g_debugStats.handoffDistance.load(std::memory_order_relaxed);
    snapshot.handoffThreshold =
        g_debugStats.handoffThreshold.load(std::memory_order_relaxed);
    snapshot.handoffResult =
        g_debugStats.handoffResult.load(std::memory_order_relaxed);
    snapshot.handoffLoaded =
        g_debugStats.handoffLoaded.load(std::memory_order_relaxed);
    snapshot.vehicleLodWitnessValid =
        g_debugStats.vehicleLodWitnessValid.load(std::memory_order_relaxed);
    snapshot.vehicleMultiPassM =
        g_debugStats.vehicleMultiPassM.load(std::memory_order_relaxed);
    snapshot.vehicleLod0M =
        g_debugStats.vehicleLod0M.load(std::memory_order_relaxed);
    snapshot.vehicleLod1M =
        g_debugStats.vehicleLod1M.load(std::memory_order_relaxed);
    snapshot.vehicleBigLod0M =
        g_debugStats.vehicleBigLod0M.load(std::memory_order_relaxed);
    snapshot.vehicleLodOverrideActive =
        g_debugStats.vehicleLodOverrideActive.load(std::memory_order_relaxed);
    snapshot.vehicleLodTargetM =
        g_debugStats.vehicleLodTargetM.load(std::memory_order_relaxed);
    snapshot.vehicleLodSampleHookActive =
        g_debugStats.vehicleLodSampleHookActive.load(std::memory_order_relaxed);
    snapshot.vehicleSampleCalls =
        g_debugStats.vehicleSampleCalls.load(std::memory_order_relaxed);
    snapshot.vehicleNearValid =
        g_debugStats.vehicleNearValid.load(std::memory_order_relaxed);
    snapshot.vehicleNearDistance =
        g_debugStats.vehicleNearDistance.load(std::memory_order_relaxed);
    snapshot.vehicleNearThreshold =
        g_debugStats.vehicleNearThreshold.load(std::memory_order_relaxed);
    snapshot.vehicleNearHigh =
        g_debugStats.vehicleNearHigh.load(std::memory_order_relaxed);
    snapshot.streetPropFloorActive =
        g_debugStats.streetPropFloorActive.load(std::memory_order_relaxed);
    snapshot.streetPropFloorM =
        g_debugStats.streetPropFloorM.load(std::memory_order_relaxed);
    snapshot.unlinkedShortTests =
        g_debugStats.unlinkedShortTests.load(std::memory_order_relaxed);
    snapshot.streetPropTests =
        g_debugStats.streetPropTests.load(std::memory_order_relaxed);
    snapshot.streetPropWrites =
        g_debugStats.streetPropWrites.load(std::memory_order_relaxed);
    snapshot.propModelId =
        g_debugStats.propModelId.load(std::memory_order_relaxed);
    snapshot.propDistance =
        g_debugStats.propDistance.load(std::memory_order_relaxed);
    snapshot.propStockThreshold =
        g_debugStats.propStockThreshold.load(std::memory_order_relaxed);
    snapshot.propAppliedThreshold =
        g_debugStats.propAppliedThreshold.load(std::memory_order_relaxed);
    snapshot.propResult =
        g_debugStats.propResult.load(std::memory_order_relaxed);
    snapshot.propLoaded =
        g_debugStats.propLoaded.load(std::memory_order_relaxed);
    snapshot.propTargeted =
        g_debugStats.propTargeted.load(std::memory_order_relaxed);
    snapshot.staticTests = g_debugStats.staticTests.load(std::memory_order_relaxed);
    snapshot.dynamicTests = g_debugStats.dynamicTests.load(std::memory_order_relaxed);
    snapshot.behindTests = g_debugStats.behindTests.load(std::memory_order_relaxed);
    snapshot.cullAttributionValid =
        g_debugStats.cullAttributionValid.load(std::memory_order_relaxed);
    snapshot.dynamicMatrixCalls =
        g_debugStats.dynamicMatrixCalls.load(std::memory_order_relaxed);
    snapshot.dynamicWidenAccepts =
        g_debugStats.dynamicWidenAccepts.load(std::memory_order_relaxed);
    snapshot.dynamicFallbackVisible =
        g_debugStats.dynamicFallbackVisible.load(std::memory_order_relaxed);
    snapshot.dynamicFallbackCulled =
        g_debugStats.dynamicFallbackCulled.load(std::memory_order_relaxed);
    snapshot.dynamicWiden60To80 =
        g_debugStats.dynamicWiden60To80.load(std::memory_order_relaxed);
    snapshot.dynamicWidenBehind =
        g_debugStats.dynamicWidenBehind.load(std::memory_order_relaxed);
    snapshot.sphere45Shortcuts =
        g_debugStats.sphere45Shortcuts.load(std::memory_order_relaxed);
    snapshot.staticSafetyRadiusM =
        g_debugStats.staticSafetyRadiusM.load(std::memory_order_relaxed);
    snapshot.staticSafetyTests =
        g_debugStats.staticSafetyTests.load(std::memory_order_relaxed);
    snapshot.staticSafetyStockVisible =
        g_debugStats.staticSafetyStockVisible.load(std::memory_order_relaxed);
    snapshot.staticSafetyAccepts =
        g_debugStats.staticSafetyAccepts.load(std::memory_order_relaxed);
    snapshot.staticSafety45To60 =
        g_debugStats.staticSafety45To60.load(std::memory_order_relaxed);
    snapshot.staticSafety60To80 =
        g_debugStats.staticSafety60To80.load(std::memory_order_relaxed);
    snapshot.staticSafetyBuildingAccepts =
        g_debugStats.staticSafetyBuildingAccepts.load(
            std::memory_order_relaxed);
    snapshot.staticSafetyDummyAccepts =
        g_debugStats.staticSafetyDummyAccepts.load(
            std::memory_order_relaxed);
    snapshot.nearbyScanActive =
        g_debugStats.nearbyScanActive.load(std::memory_order_relaxed);
    snapshot.nearbyScanRadiusM =
        g_debugStats.nearbyScanRadiusM.load(std::memory_order_relaxed);
    snapshot.nearbyScanSectors =
        g_debugStats.nearbyScanSectors.load(std::memory_order_relaxed);
    snapshot.nearbyScanVisibleAdded =
        g_debugStats.nearbyScanVisibleAdded.load(std::memory_order_relaxed);
    snapshot.nearbyScanWallMs =
        g_debugStats.nearbyScanWallMs.load(std::memory_order_relaxed);
    snapshot.nearbyScanCpuMs =
        g_debugStats.nearbyScanCpuMs.load(std::memory_order_relaxed);
    snapshot.cullAttributionFaults =
        g_debugStats.cullAttributionFaults.load(std::memory_order_relaxed);
    snapshot.alphaBefore = g_debugStats.alphaBefore.load(std::memory_order_relaxed);
    snapshot.alphaAfterLeft =
        g_debugStats.alphaAfterLeft.load(std::memory_order_relaxed);
    snapshot.alphaAfterRight =
        g_debugStats.alphaAfterRight.load(std::memory_order_relaxed);
    snapshot.dedupeChecks = g_debugStats.dedupeChecks.load(std::memory_order_relaxed);
    snapshot.dedupeHits = g_debugStats.dedupeHits.load(std::memory_order_relaxed);
    snapshot.dedupeFaults = g_debugStats.dedupeFaults.load(std::memory_order_relaxed);
    snapshot.shadowCalls = g_debugStats.shadowCalls.load(std::memory_order_relaxed);
    snapshot.shadowEye2Skips =
        g_debugStats.shadowEye2Skips.load(std::memory_order_relaxed);
    snapshot.gateSkips = g_debugStats.gateSkips.load(std::memory_order_relaxed);
    snapshot.frameLimit = g_debugStats.frameLimit.load(std::memory_order_relaxed);
    snapshot.eyeWidth = g_debugStats.eyeWidth.load(std::memory_order_relaxed);
    snapshot.eyeHeight = g_debugStats.eyeHeight.load(std::memory_order_relaxed);
    snapshot.renderScalePercent =
        g_debugStats.renderScalePercent.load(std::memory_order_relaxed);
    snapshot.alphaHookActive =
        g_debugStats.alphaHookActive.load(std::memory_order_relaxed);
    snapshot.shadowHookActive =
        g_debugStats.shadowHookActive.load(std::memory_order_relaxed);
    snapshot.buildingDetailTests =
        g_debugStats.buildingDetailTests.load(std::memory_order_relaxed);
    snapshot.buildingDetailOverrides =
        g_debugStats.buildingDetailOverrides.load(std::memory_order_relaxed);
    snapshot.modelDrawRestoreFaults =
        g_debugStats.modelDrawRestoreFaults.load(
            std::memory_order_relaxed);
    snapshot.ambientCarGateActive =
        g_debugStats.ambientCarGateActive.load(std::memory_order_relaxed);
    snapshot.ambientCarTarget =
        g_debugStats.ambientCarTarget.load(std::memory_order_relaxed);
    snapshot.ambientCarAttempts =
        g_debugStats.ambientCarAttempts.load(std::memory_order_relaxed);
    snapshot.ambientCarBlocked =
        g_debugStats.ambientCarBlocked.load(std::memory_order_relaxed);
    snapshot.ambientCarWantedPasses =
        g_debugStats.ambientCarWantedPasses.load(std::memory_order_relaxed);
    snapshot.pedAmbientCap =
        g_debugStats.pedAmbientCap.load(std::memory_order_relaxed);
    snapshot.pedAmbientAttempts =
        g_debugStats.pedAmbientAttempts.load(std::memory_order_relaxed);
    snapshot.pedAmbientSuccesses =
        g_debugStats.pedAmbientSuccesses.load(std::memory_order_relaxed);
    snapshot.pedAmbientBlocked =
        g_debugStats.pedAmbientBlocked.load(std::memory_order_relaxed);

    snapshot.presentValid = g_debugStats.presentValid.load(std::memory_order_relaxed);
    snapshot.presentHz = g_debugStats.presentHz.load(std::memory_order_relaxed);
    snapshot.displayValid = g_debugStats.displayValid.load(std::memory_order_relaxed);
    snapshot.displayHz = g_debugStats.displayHz.load(std::memory_order_relaxed);
    snapshot.budgetMs = g_debugStats.budgetMs.load(std::memory_order_relaxed);
    snapshot.runtimeCpuValid =
        g_debugStats.runtimeCpuValid.load(std::memory_order_relaxed);
    snapshot.runtimeCpuMs = g_debugStats.runtimeCpuMs.load(std::memory_order_relaxed);
    snapshot.runtimeGpuValid =
        g_debugStats.runtimeGpuValid.load(std::memory_order_relaxed);
    snapshot.runtimeGpuMs = g_debugStats.runtimeGpuMs.load(std::memory_order_relaxed);
    snapshot.endFailures = g_debugStats.endFailures.load(std::memory_order_relaxed);
    snapshot.ringRaces = g_debugStats.ringRaces.load(std::memory_order_relaxed);
    snapshot.fresh = g_debugStats.fresh.load(std::memory_order_relaxed);
    snapshot.repeated = g_debugStats.repeated.load(std::memory_order_relaxed);
    snapshot.stereoSyncWaits =
        g_debugStats.stereoSyncWaits.load(std::memory_order_relaxed);
    snapshot.stereoSyncRescued =
        g_debugStats.stereoSyncRescued.load(std::memory_order_relaxed);
    snapshot.stereoSyncTimeouts =
        g_debugStats.stereoSyncTimeouts.load(std::memory_order_relaxed);
    snapshot.stereoSyncWaitMs =
        g_debugStats.stereoSyncWaitMs.load(std::memory_order_relaxed);
    snapshot.fxaaRequested =
        g_debugStats.fxaaRequested.load(std::memory_order_relaxed);
    snapshot.fxaaActive = g_debugStats.fxaaActive.load(std::memory_order_relaxed);
    snapshot.fxaaDraws = g_debugStats.fxaaDraws.load(std::memory_order_relaxed);
    snapshot.fxaaFallbacks =
        g_debugStats.fxaaFallbacks.load(std::memory_order_relaxed);
    snapshot.fxaaErrors = g_debugStats.fxaaErrors.load(std::memory_order_relaxed);
    snapshot.fxaaSubmitWallMs =
        g_debugStats.fxaaSubmitWallMs.load(std::memory_order_relaxed);

    snapshot.limiterActive =
        g_debugStats.limiterActive.load(std::memory_order_relaxed);
    snapshot.limiterRxRestored =
        g_debugStats.limiterRxRestored.load(std::memory_order_relaxed);
    snapshot.outerPacerHz = g_debugStats.outerPacerHz.load(std::memory_order_relaxed);
    snapshot.revision = g_debugStats.revision.load(std::memory_order_relaxed);
    return snapshot;
}

std::uint64_t DebugStatsRevision() {
    return g_debugStats.revision.load(std::memory_order_acquire);
}

void SetLimiterDebugStatus(bool active, bool rxRestored, float outerPacerHz) {
    std::lock_guard<std::mutex> debugLock(g_debugStatsMutex);
    g_debugStats.limiterActive.store(active, std::memory_order_relaxed);
    g_debugStats.limiterRxRestored.store(rxRestored, std::memory_order_relaxed);
    g_debugStats.outerPacerHz.store(outerPacerHz, std::memory_order_relaxed);
    g_debugStats.revision.fetch_add(1, std::memory_order_release);
}

void SubmitStereoFrame(const StereoFrameSample& sample) {
    g_latestStereo = sample;
    g_haveStereo = sample.sequence >= 0;
}

void SubmitGameFrame(const GameFrameSample& sample) {
    if (g_gameWindow.startMs <= 0.0) g_gameWindow.startMs = sample.monoMs;
    if (sample.monoMs - g_gameWindow.startMs >= 1000.0) {
        EmitGameWindow(sample.monoMs);
        g_gameWindow = {};
        g_gameWindow.startMs = sample.monoMs;
    }

    GameWindow& w = g_gameWindow;
    ++w.callbacks;
    w.last = sample;
    if (sample.callbackPeriodMs > 0.0) w.callbackPeriod.Add(sample.callbackPeriodMs);
    w.callbackWork.Add(sample.totalWallMs);
    if (sample.javaDeltaSeconds > 0.0f)
        w.javaDelta.Add(static_cast<double>(sample.javaDeltaSeconds) * 1000.0);
    w.totalThreadCpuMs += std::max(0.0, sample.totalCpuMs);
    if (sample.wrapperSwapCalled) {
        ++w.wrapperSwapCalls;
        w.swapWall.Add(sample.wrapperSwapMs);
        if (!sample.wrapperSwapSucceeded) ++w.wrapperSwapFailures;
    }

    const bool rendered = sample.renderSceneCalls > 0;
    w.renderSceneCalls += std::max(0, sample.renderSceneCalls);
    if (rendered) {
        ++w.renderedCallbacks;
        if (g_lastRenderedGameMs > 0.0)
            w.renderGap.Add(sample.monoMs - g_lastRenderedGameMs);
        g_lastRenderedGameMs = sample.monoMs;
        w.renderWall.Add(sample.implWallMs);
        w.renderCpu.Add(sample.implCpuMs);
        w.renderBlocked.Add(std::max(0.0, sample.implWallMs - sample.implCpuMs));
        w.renderSleepRequested.Add(sample.sleepRequestedMs);
        w.renderSleepActual.Add(sample.sleepActualMs);
    } else {
        ++w.limiterSkips;
        w.skipWall.Add(sample.implWallMs);
        w.skipCpu.Add(sample.implCpuMs);
        w.skipBlocked.Add(std::max(0.0, sample.implWallMs - sample.implCpuMs));
        w.skipSleepRequested.Add(sample.sleepRequestedMs);
        w.skipSleepActual.Add(sample.sleepActualMs);
    }

    const int currentSequence = g_haveStereo ? g_latestStereo.sequence : -1;
    const bool freshStereo = currentSequence >= 0 &&
                             currentSequence != g_lastGameStereoSequence;
    if (freshStereo) {
        ++w.freshStereoFrames;
        if (g_lastGameStereoSequence >= 0 && currentSequence > g_lastGameStereoSequence + 1)
            w.sequenceJumps += currentSequence - g_lastGameStereoSequence - 1;
        g_lastGameStereoSequence = currentSequence;
        w.lastStereo = g_latestStereo;
        // Publish attribution faults even when they are the reason the stricter
        // engine_pre breakdown below is rejected.  Keeping this inside the
        // valid branch masked the exact failure it was meant to diagnose.
        w.renderQueueWaitClassificationFaults += std::max(
            0, g_latestStereo.renderQueueWaitClassificationFaults);
        w.renderQueueFinishRequestCalls.Add(std::max(
            0, g_latestStereo.renderQueueFinishRequestCalls));
        w.renderQueueFinishDeferredCalls.Add(std::max(
            0, g_latestStereo.renderQueueFinishDeferredCalls));
        w.renderQueueFinishDrainCalls.Add(std::max(
            0, g_latestStereo.renderQueueFinishDrainCalls));
        w.renderQueueFinishFallbackCalls.Add(std::max(
            0, g_latestStereo.renderQueueFinishFallbackCalls));
        w.renderQueueFinishOverlapWall.Add(std::max(
            0.0, g_latestStereo.renderQueueFinishOverlapWallMs));
        w.renderQueueFinishDrainWall.Add(std::max(
            0.0, g_latestStereo.renderQueueFinishDrainWallMs));
        w.renderQueueFinishDrainCpu.Add(std::max(
            0.0, g_latestStereo.renderQueueFinishDrainCpuMs));
        w.renderQueueFinishDrainMaxWall.Add(std::max(
            0.0, g_latestStereo.renderQueueFinishDrainMaxWallMs));
        w.renderQueueFinishPendingDepthMax.Add(std::max(
            0, g_latestStereo.renderQueueFinishPendingDepthMax));
        w.renderQueueFinishPendingFaults += std::max(
            0, g_latestStereo.renderQueueFinishPendingFaults);

        constexpr double kPhaseClockToleranceMs = 0.25;
        const auto finiteNonNegative = [](double value) {
            return std::isfinite(value) && value >= 0.0;
        };
        const double hookEntryWallOffset =
            g_latestStereo.hookEntryMonoMs - sample.monoMs;
        const double hookExitWallOffset =
            g_latestStereo.hookExitMonoMs - sample.monoMs;
        const double hookEntryCpuOffset =
            g_latestStereo.hookEntryCpuMs - sample.implStartCpuMs;
        const double hookExitCpuOffset =
            g_latestStereo.hookExitCpuMs - sample.implStartCpuMs;
        const double hookWallSpan =
            g_latestStereo.hookExitMonoMs - g_latestStereo.hookEntryMonoMs;
        const double hookCpuSpan =
            g_latestStereo.hookExitCpuMs - g_latestStereo.hookEntryCpuMs;
        const double attributedHookWall =
            g_latestStereo.stereoPrepareWallMs +
            g_latestStereo.recordWallMs +
            g_latestStereo.stereoTailWallMs;
        const double attributedHookCpu =
            g_latestStereo.stereoPrepareCpuMs +
            g_latestStereo.recordCpuMs +
            g_latestStereo.stereoTailCpuMs;
        const bool phaseTimingValid = g_latestStereo.phaseTimingValid &&
            finiteNonNegative(sample.monoMs) &&
            finiteNonNegative(sample.implStartCpuMs) &&
            finiteNonNegative(sample.implWallMs) &&
            finiteNonNegative(sample.implCpuMs) &&
            finiteNonNegative(g_latestStereo.hookEntryMonoMs) &&
            finiteNonNegative(g_latestStereo.hookEntryCpuMs) &&
            finiteNonNegative(g_latestStereo.hookExitMonoMs) &&
            finiteNonNegative(g_latestStereo.hookExitCpuMs) &&
            finiteNonNegative(g_latestStereo.stereoPrepareWallMs) &&
            finiteNonNegative(g_latestStereo.stereoPrepareCpuMs) &&
            finiteNonNegative(g_latestStereo.recordWallMs) &&
            finiteNonNegative(g_latestStereo.recordCpuMs) &&
            finiteNonNegative(g_latestStereo.stereoTailWallMs) &&
            finiteNonNegative(g_latestStereo.stereoTailCpuMs) &&
            hookEntryWallOffset >= -kPhaseClockToleranceMs &&
            hookExitWallOffset + kPhaseClockToleranceMs >= hookEntryWallOffset &&
            hookExitWallOffset <= sample.implWallMs + kPhaseClockToleranceMs &&
            hookEntryCpuOffset >= -kPhaseClockToleranceMs &&
            hookExitCpuOffset + kPhaseClockToleranceMs >= hookEntryCpuOffset &&
            hookExitCpuOffset <= sample.implCpuMs + kPhaseClockToleranceMs &&
            hookWallSpan >= -kPhaseClockToleranceMs &&
            hookCpuSpan >= -kPhaseClockToleranceMs &&
            attributedHookWall <= hookWallSpan + kPhaseClockToleranceMs &&
            attributedHookCpu <= hookCpuSpan + kPhaseClockToleranceMs;
        if (phaseTimingValid) {
            const double enginePreWall = std::clamp(
                hookEntryWallOffset, 0.0, sample.implWallMs);
            const double enginePreCpu = std::clamp(
                hookEntryCpuOffset, 0.0, sample.implCpuMs);
            const double hookExitWall = std::clamp(
                hookExitWallOffset, enginePreWall, sample.implWallMs);
            const double hookExitCpu = std::clamp(
                hookExitCpuOffset, enginePreCpu, sample.implCpuMs);
            const double stereoPrepareWall = std::clamp(
                g_latestStereo.stereoPrepareWallMs, 0.0, sample.implWallMs);
            const double stereoPrepareCpu = std::clamp(
                g_latestStereo.stereoPrepareCpuMs, 0.0, sample.implCpuMs);
            const double stereoTailWall = std::clamp(
                g_latestStereo.stereoTailWallMs, 0.0, sample.implWallMs);
            const double stereoTailCpu = std::clamp(
                g_latestStereo.stereoTailCpuMs, 0.0, sample.implCpuMs);
            const double enginePostWall = std::max(
                0.0, sample.implWallMs - hookExitWall);
            const double enginePostCpu = std::max(
                0.0, sample.implCpuMs - hookExitCpu);
            const double phaseWallResidual = sample.implWallMs -
                (enginePreWall + stereoPrepareWall +
                 g_latestStereo.recordWallMs + stereoTailWall +
                 enginePostWall);
            const double phaseCpuResidual = sample.implCpuMs -
                (enginePreCpu + stereoPrepareCpu +
                 g_latestStereo.recordCpuMs + stereoTailCpu +
                 enginePostCpu);

            ++w.phaseValidFrames;
            w.enginePreWall.Add(enginePreWall);
            w.enginePreCpu.Add(enginePreCpu);
            w.stereoPrepareWall.Add(stereoPrepareWall);
            w.stereoPrepareCpu.Add(stereoPrepareCpu);
            w.stereoTailWall.Add(stereoTailWall);
            w.stereoTailCpu.Add(stereoTailCpu);
            w.enginePostWall.Add(enginePostWall);
            w.enginePostCpu.Add(enginePostCpu);
            w.phaseWallError.Add(std::fabs(phaseWallResidual));
            w.phaseCpuError.Add(std::fabs(phaseCpuResidual));

            const double scanEntryWallOffset =
                g_latestStereo.enginePreMainScanEntryMonoMs - sample.monoMs;
            const double scanExitWallOffset =
                g_latestStereo.enginePreMainScanExitMonoMs - sample.monoMs;
            const double scanEntryCpuOffset =
                g_latestStereo.enginePreMainScanEntryCpuMs -
                    sample.implStartCpuMs;
            const double scanExitCpuOffset =
                g_latestStereo.enginePreMainScanExitCpuMs -
                    sample.implStartCpuMs;
            const int rqTotalCalls = g_latestStereo.renderQueueFinishCalls +
                g_latestStereo.renderQueueFlushCalls;
            const int rqStageCalls =
                g_latestStereo.renderQueueWaitBeforeScanCalls +
                g_latestStereo.renderQueueWaitMainScanCalls +
                g_latestStereo.renderQueueWaitAfterScanCalls;
            const int rqFlushTypeCalls =
                g_latestStereo.renderQueueSizedFlushCalls +
                g_latestStereo.renderQueueNearEndFlushCalls +
                g_latestStereo.renderQueueExplicitFlushCalls;
            const double rqTotalWall =
                g_latestStereo.renderQueueFinishWallMs +
                g_latestStereo.renderQueueFlushWallMs;
            const double rqStageWall =
                g_latestStereo.renderQueueWaitBeforeScanWallMs +
                g_latestStereo.renderQueueWaitMainScanWallMs +
                g_latestStereo.renderQueueWaitAfterScanWallMs;
            const double rqTotalCpu =
                g_latestStereo.renderQueueFinishCpuMs +
                g_latestStereo.renderQueueFlushCpuMs;
            const double rqStageCpu =
                g_latestStereo.renderQueueWaitBeforeScanCpuMs +
                g_latestStereo.renderQueueWaitMainScanCpuMs +
                g_latestStereo.renderQueueWaitAfterScanCpuMs;
            const int rqFinishRequests =
                g_latestStereo.renderQueueFinishRequestCalls;
            const int rqFinishOutcomes =
                g_latestStereo.renderQueueFinishDrainCalls +
                g_latestStereo.renderQueueFinishFallbackCalls;
            const bool rqDeferredProtocolValid =
                g_latestStereo.renderQueueFinishPendingDepthMax >= 0 &&
                g_latestStereo.renderQueueFinishPendingDepthMax <= 1 &&
                g_latestStereo.renderQueueFinishPendingFaults == 0 &&
                finiteNonNegative(
                    g_latestStereo.renderQueueFinishOverlapWallMs) &&
                finiteNonNegative(
                    g_latestStereo.renderQueueFinishDrainWallMs) &&
                finiteNonNegative(
                    g_latestStereo.renderQueueFinishDrainCpuMs) &&
                finiteNonNegative(
                    g_latestStereo.renderQueueFinishDrainMaxWallMs) &&
                g_latestStereo.renderQueueFinishDrainWallMs <=
                    g_latestStereo.renderQueueFinishWallMs +
                        kPhaseClockToleranceMs &&
                g_latestStereo.renderQueueFinishDrainCpuMs <=
                    g_latestStereo.renderQueueFinishCpuMs +
                        kPhaseClockToleranceMs &&
                g_latestStereo.renderQueueFinishDrainMaxWallMs <=
                    g_latestStereo.renderQueueFinishMaxWallMs +
                        kPhaseClockToleranceMs &&
                (!g_latestStereo.renderQueueFinishDeferActive ||
                 (rqFinishRequests >= 0 &&
                  rqFinishRequests ==
                      g_latestStereo.renderQueueFinishCalls &&
                  rqFinishRequests == rqFinishOutcomes &&
                  g_latestStereo.renderQueueFinishDeferredCalls >=
                      g_latestStereo.renderQueueFinishDrainCalls &&
                  g_latestStereo.renderQueueFinishDeferredCalls <=
                      rqFinishRequests));
            const bool enginePreBreakdownValid =
                g_latestStereo.enginePreProfileCaptured &&
                g_latestStereo.enginePreMainScanCalls == 1 &&
                finiteNonNegative(scanEntryWallOffset) &&
                finiteNonNegative(scanExitWallOffset) &&
                finiteNonNegative(scanEntryCpuOffset) &&
                finiteNonNegative(scanExitCpuOffset) &&
                finiteNonNegative(
                    g_latestStereo.enginePreStockScanWallMs) &&
                finiteNonNegative(
                    g_latestStereo.enginePreStockScanCpuMs) &&
                scanExitWallOffset + kPhaseClockToleranceMs >=
                    scanEntryWallOffset &&
                hookEntryWallOffset + kPhaseClockToleranceMs >=
                    scanExitWallOffset &&
                scanExitCpuOffset + kPhaseClockToleranceMs >=
                    scanEntryCpuOffset &&
                hookEntryCpuOffset + kPhaseClockToleranceMs >=
                    scanExitCpuOffset &&
                g_latestStereo.enginePreStockScanWallMs <=
                    (scanExitWallOffset - scanEntryWallOffset) +
                        kPhaseClockToleranceMs &&
                g_latestStereo.enginePreStockScanCpuMs <=
                    (scanExitCpuOffset - scanEntryCpuOffset) +
                        kPhaseClockToleranceMs &&
                rqTotalCalls >= 0 && rqStageCalls == rqTotalCalls &&
                rqFlushTypeCalls == g_latestStereo.renderQueueFlushCalls &&
                finiteNonNegative(rqTotalWall) &&
                finiteNonNegative(rqStageWall) &&
                finiteNonNegative(rqTotalCpu) &&
                finiteNonNegative(rqStageCpu) &&
                rqDeferredProtocolValid &&
                std::fabs(rqTotalWall - rqStageWall) <=
                    kPhaseClockToleranceMs &&
                std::fabs(rqTotalCpu - rqStageCpu) <=
                    kPhaseClockToleranceMs;
            if (enginePreBreakdownValid) {
                const double beforeScanWall = std::clamp(
                    scanEntryWallOffset, 0.0, enginePreWall);
                const double mainScanWall = std::clamp(
                    scanExitWallOffset - scanEntryWallOffset,
                    0.0, enginePreWall);
                const double afterScanWall = std::max(
                    0.0, enginePreWall - scanExitWallOffset);
                const double beforeScanCpu = std::clamp(
                    scanEntryCpuOffset, 0.0, enginePreCpu);
                const double mainScanCpu = std::clamp(
                    scanExitCpuOffset - scanEntryCpuOffset,
                    0.0, enginePreCpu);
                const double afterScanCpu = std::max(
                    0.0, enginePreCpu - scanExitCpuOffset);
                const double breakdownWallResidual = enginePreWall -
                    (beforeScanWall + mainScanWall + afterScanWall);
                const double breakdownCpuResidual = enginePreCpu -
                    (beforeScanCpu + mainScanCpu + afterScanCpu);

                ++w.enginePreBreakdownValidFrames;
                w.enginePreBeforeScanWall.Add(beforeScanWall);
                w.enginePreBeforeScanCpu.Add(beforeScanCpu);
                w.enginePreMainScanWall.Add(mainScanWall);
                w.enginePreMainScanCpu.Add(mainScanCpu);
                w.enginePreStockScanWall.Add(
                    g_latestStereo.enginePreStockScanWallMs);
                w.enginePreStockScanCpu.Add(
                    g_latestStereo.enginePreStockScanCpuMs);
                w.enginePreAfterScanWall.Add(afterScanWall);
                w.enginePreAfterScanCpu.Add(afterScanCpu);
                w.enginePreBreakdownResidualWall.Add(
                    std::fabs(breakdownWallResidual));
                w.enginePreBreakdownResidualCpu.Add(
                    std::fabs(breakdownCpuResidual));

                w.renderQueueFinishCalls.Add(
                    g_latestStereo.renderQueueFinishCalls);
                w.renderQueueFinishWall.Add(
                    g_latestStereo.renderQueueFinishWallMs);
                w.renderQueueFinishCpu.Add(
                    g_latestStereo.renderQueueFinishCpuMs);
                w.renderQueueFinishMaxWall.Add(
                    g_latestStereo.renderQueueFinishMaxWallMs);
                w.renderQueueFlushCalls.Add(
                    g_latestStereo.renderQueueFlushCalls);
                w.renderQueueFlushWall.Add(
                    g_latestStereo.renderQueueFlushWallMs);
                w.renderQueueFlushCpu.Add(
                    g_latestStereo.renderQueueFlushCpuMs);
                w.renderQueueFlushMaxWall.Add(
                    g_latestStereo.renderQueueFlushMaxWallMs);
                w.renderQueueSizedFlushCalls.Add(
                    g_latestStereo.renderQueueSizedFlushCalls);
                w.renderQueueNearEndFlushCalls.Add(
                    g_latestStereo.renderQueueNearEndFlushCalls);
                w.renderQueueExplicitFlushCalls.Add(
                    g_latestStereo.renderQueueExplicitFlushCalls);
                w.renderQueueWaitBeforeScanCalls.Add(
                    g_latestStereo.renderQueueWaitBeforeScanCalls);
                w.renderQueueWaitBeforeScanWall.Add(
                    g_latestStereo.renderQueueWaitBeforeScanWallMs);
                w.renderQueueWaitBeforeScanCpu.Add(
                    g_latestStereo.renderQueueWaitBeforeScanCpuMs);
                w.renderQueueWaitMainScanCalls.Add(
                    g_latestStereo.renderQueueWaitMainScanCalls);
                w.renderQueueWaitMainScanWall.Add(
                    g_latestStereo.renderQueueWaitMainScanWallMs);
                w.renderQueueWaitMainScanCpu.Add(
                    g_latestStereo.renderQueueWaitMainScanCpuMs);
                w.renderQueueWaitAfterScanCalls.Add(
                    g_latestStereo.renderQueueWaitAfterScanCalls);
                w.renderQueueWaitAfterScanWall.Add(
                    g_latestStereo.renderQueueWaitAfterScanWallMs);
                w.renderQueueWaitAfterScanCpu.Add(
                    g_latestStereo.renderQueueWaitAfterScanCpuMs);
                w.renderQueueWaitMaxUsedKiB.Add(
                    g_latestStereo.renderQueueWaitMaxUsedKiB);
            } else {
                ++w.enginePreBreakdownFaults;
            }
        } else {
            ++w.phaseFaults;
            ++w.enginePreBreakdownFaults;
        }

        w.recordWall.Add(g_latestStereo.recordWallMs);
        w.recordCpu.Add(g_latestStereo.recordCpuMs);
        w.sceneLeft.Add(g_latestStereo.sceneLeftMs);
        w.sceneRight.Add(g_latestStereo.sceneRightMs);
        w.sceneLeftCpu.Add(g_latestStereo.sceneLeftCpuMs);
        w.sceneRightCpu.Add(g_latestStereo.sceneRightCpuMs);
        w.alphaNodesBefore.Add(g_latestStereo.alphaNodesBefore);
        w.alphaNodesAfterLeft.Add(g_latestStereo.alphaNodesAfterLeft);
        w.alphaNodesAfterRight.Add(g_latestStereo.alphaNodesAfterRight);
        w.alphaDedupeChecks += std::max(0, g_latestStereo.alphaDedupeChecks);
        w.alphaDedupeHits += std::max(0, g_latestStereo.alphaDedupeHits);
        w.alphaDedupeFaults += std::max(0, g_latestStereo.alphaDedupeFaults);
        w.shadowUpdateCalls += std::max(0, g_latestStereo.shadowUpdateCalls);
        w.shadowUpdateSecondEyeSkips +=
            std::max(0, g_latestStereo.shadowUpdateSecondEyeSkips);
        w.skyLeft.Add(g_latestStereo.skyLeftMs);
        w.skyRight.Add(g_latestStereo.skyRightMs);
        w.cameraEnd.Add(g_latestStereo.cameraEndMs);
        w.engineOutsideRecord.Add(std::max(0.0,
            sample.implWallMs - g_latestStereo.recordWallMs));
        w.visibleEntities.Add(g_latestStereo.visibleEntities);
        w.visibleLods.Add(g_latestStereo.visibleLods);
        w.visibleSuperLods.Add(g_latestStereo.visibleSuperLods);
        w.streamingRequests.Add(g_latestStereo.streamingRequests);
        w.streamingPriorityRequests.Add(g_latestStereo.streamingPriorityRequests);
        w.streamingMemoryUsedMiB.Add(g_latestStereo.streamingMemoryUsedMiB);
        w.streamingMemoryAvailableMiB.Add(g_latestStereo.streamingMemoryAvailableMiB);
        w.lodScale.Add(g_latestStereo.lodScale);
        w.cameraLodMultiplier.Add(g_latestStereo.cameraLodMultiplier);
        w.cameraGenerationMultiplier.Add(g_latestStereo.cameraGenerationMultiplier);
        w.cameraFov.Add(g_latestStereo.cameraFov);
        w.farClip.Add(g_latestStereo.farClip);
        if (g_latestStereo.carPopulationValid) {
            w.carRandom.Add(g_latestStereo.carRandom);
            w.carLaw.Add(g_latestStereo.carLaw);
            w.carMission.Add(g_latestStereo.carMission);
            w.carParked.Add(g_latestStereo.carParked);
            w.carPermanent.Add(g_latestStereo.carPermanent);
            w.carMax.Add(g_latestStereo.carMax);
            w.carDensity.Add(g_latestStereo.carDensity);
        }
        if (g_latestStereo.pedPopulationValid) {
            w.pedTotal.Add(g_latestStereo.pedTotal);
            w.pedCiv.Add(g_latestStereo.pedCiv);
            w.pedGang.Add(g_latestStereo.pedGang);
            w.pedMission.Add(g_latestStereo.pedMission);
            w.pedCarPassenger.Add(g_latestStereo.pedCarPassenger);
            w.pedMax.Add(g_latestStereo.pedMax);
            w.pedDensity.Add(g_latestStereo.pedDensity);
        }
        w.linkedEntityTests.Add(g_latestStereo.linkedEntityTests);
        w.linkedEntityLoaded.Add(g_latestStereo.linkedEntityLoaded);
        w.linkedResultVisible.Add(g_latestStereo.linkedResultVisible);
        w.linkedResultCulled.Add(g_latestStereo.linkedResultCulled);
        w.linkedResultStream.Add(g_latestStereo.linkedResultStream);
        w.linkedNearThreshold.Add(g_latestStereo.linkedNearThreshold);
        w.prefetchBand.Add(g_latestStereo.prefetchBand);
        w.prefetchStreamMe.Add(g_latestStereo.prefetchStreamMe);
        w.prefetchRequestCalls.Add(g_latestStereo.prefetchRequestCalls);
        w.prefetchEnqueues.Add(g_latestStereo.prefetchEnqueues);
        w.prefetchAlreadyPending.Add(g_latestStereo.prefetchAlreadyPending);
        w.prefetchThrottled.Add(g_latestStereo.prefetchThrottled);
        w.prefetchStateAnomalies.Add(g_latestStereo.prefetchStateAnomalies);
        w.linkedNearReady.Add(g_latestStereo.linkedNearReady);
        w.linkedNearMissing.Add(g_latestStereo.linkedNearMissing);
        const bool handoffCandidateValid =
            g_latestStereo.handoffModelId >= 0 &&
            g_latestStereo.handoffResult >= 0 &&
            g_latestStereo.handoffResult <= 3 &&
            g_latestStereo.handoffResult != 2 &&
            std::isfinite(g_latestStereo.handoffDistance) &&
            g_latestStereo.handoffDistance >= 0.0 &&
            std::isfinite(g_latestStereo.handoffThreshold) &&
            g_latestStereo.handoffThreshold > 0.0;
        if (handoffCandidateValid) {
            const double delta = std::fabs(
                g_latestStereo.handoffDistance - g_latestStereo.handoffThreshold);
            if (!w.handoffCandidateValid || delta < w.handoffAbsDelta) {
                w.handoffCandidateValid = true;
                w.handoffAbsDelta = delta;
                w.handoffModelId = g_latestStereo.handoffModelId;
                w.handoffDistance = g_latestStereo.handoffDistance;
                w.handoffThreshold = g_latestStereo.handoffThreshold;
                w.handoffResult = g_latestStereo.handoffResult;
                w.handoffLoaded = g_latestStereo.handoffLoaded;
            }
        }
        if (g_latestStereo.vehicleLodWitnessValid) {
            w.vehicleMultiPassM.Add(g_latestStereo.vehicleMultiPassM);
            w.vehicleLod0M.Add(g_latestStereo.vehicleLod0M);
            w.vehicleLod1M.Add(g_latestStereo.vehicleLod1M);
            w.vehicleBigLod0M.Add(g_latestStereo.vehicleBigLod0M);
        }
        w.vehicleSampleCalls.Add(g_latestStereo.vehicleSampleCalls);
        const bool vehicleCandidateValid = g_latestStereo.vehicleNearValid &&
            std::isfinite(g_latestStereo.vehicleNearDistance) &&
            g_latestStereo.vehicleNearDistance >= 0.0 &&
            std::isfinite(g_latestStereo.vehicleNearThreshold) &&
            g_latestStereo.vehicleNearThreshold > 0.0;
        if (vehicleCandidateValid) {
            const double delta = std::fabs(
                g_latestStereo.vehicleNearDistance -
                g_latestStereo.vehicleNearThreshold);
            if (!w.vehicleCandidateValid || delta < w.vehicleNearAbsDelta) {
                w.vehicleCandidateValid = true;
                w.vehicleNearAbsDelta = delta;
                w.vehicleNearDistance = g_latestStereo.vehicleNearDistance;
                w.vehicleNearThreshold = g_latestStereo.vehicleNearThreshold;
                w.vehicleNearHigh = g_latestStereo.vehicleNearHigh;
            }
        }
        w.unlinkedShortTests.Add(g_latestStereo.unlinkedShortTests);
        w.streetPropTests.Add(g_latestStereo.streetPropTests);
        w.streetPropWrites.Add(g_latestStereo.streetPropWrites);
        const bool propCandidateValid = g_latestStereo.propModelId >= 0 &&
            g_latestStereo.propModelId < 20000 &&
            g_latestStereo.propResult >= 0 &&
            g_latestStereo.propResult <= 3 &&
            g_latestStereo.propResult != 2 &&
            std::isfinite(g_latestStereo.propDistance) &&
            g_latestStereo.propDistance >= 0.0 &&
            std::isfinite(g_latestStereo.propStockThreshold) &&
            g_latestStereo.propStockThreshold > 0.0 &&
            std::isfinite(g_latestStereo.propAppliedThreshold) &&
            g_latestStereo.propAppliedThreshold > 0.0;
        if (propCandidateValid) {
            constexpr double kRetailFadeTailBaseM = 22.0;
            const double liveLodScale =
                std::isfinite(g_latestStereo.lodScale) &&
                g_latestStereo.lodScale > 0.0
                    ? g_latestStereo.lodScale : 1.0;
            const double retailFadeTailM =
                kRetailFadeTailBaseM * liveLodScale;
            const double delta = std::min(
                std::fabs(g_latestStereo.propDistance -
                          g_latestStereo.propAppliedThreshold),
                std::fabs(g_latestStereo.propDistance -
                           (g_latestStereo.propAppliedThreshold +
                            retailFadeTailM)));
            if (!w.propCandidateValid || delta < w.propAbsDelta) {
                w.propCandidateValid = true;
                w.propAbsDelta = delta;
                w.propModelId = g_latestStereo.propModelId;
                w.propDistance = g_latestStereo.propDistance;
                w.propStockThreshold = g_latestStereo.propStockThreshold;
                w.propAppliedThreshold = g_latestStereo.propAppliedThreshold;
                w.propResult = g_latestStereo.propResult;
                w.propLoaded = g_latestStereo.propLoaded;
                w.propTargeted = g_latestStereo.propTargeted;
            }
        }
        w.staticTests.Add(g_latestStereo.staticTests);
        w.dynamicTests.Add(g_latestStereo.dynamicTests);
        w.behindTests.Add(g_latestStereo.behindTests);
        w.dynamicMatrixCalls.Add(g_latestStereo.dynamicMatrixCalls);
        w.dynamicWidenAccepts.Add(g_latestStereo.dynamicWidenAccepts);
        w.dynamicFallbackVisible.Add(g_latestStereo.dynamicFallbackVisible);
        w.dynamicFallbackCulled.Add(g_latestStereo.dynamicFallbackCulled);
        w.dynamicWiden60To80.Add(g_latestStereo.dynamicWiden60To80);
        w.dynamicWidenBehind.Add(g_latestStereo.dynamicWidenBehind);
        w.sphere45Shortcuts.Add(g_latestStereo.sphere45Shortcuts);
        w.staticSafetyTests.Add(g_latestStereo.staticSafetyTests);
        w.staticSafetyStockVisible.Add(
            g_latestStereo.staticSafetyStockVisible);
        w.staticSafetyAccepts.Add(g_latestStereo.staticSafetyAccepts);
        w.staticSafety45To60.Add(g_latestStereo.staticSafety45To60);
        w.staticSafety60To80.Add(g_latestStereo.staticSafety60To80);
        w.staticSafetyBuildingAccepts.Add(
            g_latestStereo.staticSafetyBuildingAccepts);
        w.staticSafetyDummyAccepts.Add(
            g_latestStereo.staticSafetyDummyAccepts);
        w.staticSafetyConeRejects.Add(
            g_latestStereo.staticSafetyConeRejects);
        w.largeVegetationSafetyTests.Add(
            g_latestStereo.largeVegetationSafetyTests);
        w.largeVegetationSafetyStockVisible.Add(
            g_latestStereo.largeVegetationSafetyStockVisible);
        w.largeVegetationSafetyAccepts.Add(
            g_latestStereo.largeVegetationSafetyAccepts);
        w.largeVegetationSafetyConeRejects.Add(
            g_latestStereo.largeVegetationSafetyConeRejects);
        w.roadsideSafetyTests.Add(g_latestStereo.roadsideSafetyTests);
        w.roadsideSafetyStockVisible.Add(
            g_latestStereo.roadsideSafetyStockVisible);
        w.roadsideSafetyAccepts.Add(g_latestStereo.roadsideSafetyAccepts);
        w.roadsideSafetyConeRejects.Add(
            g_latestStereo.roadsideSafetyConeRejects);
        w.targetedOcclusionBypasses.Add(
            g_latestStereo.targetedOcclusionBypasses);
        w.targetedOcclusionTests.Add(
            g_latestStereo.targetedOcclusionTests);
        w.targetedOcclusionStockVisible.Add(
            g_latestStereo.targetedOcclusionStockVisible);
        w.targetedOcclusionStockOccluded.Add(
            g_latestStereo.targetedOcclusionStockOccluded);
        w.targetedOcclusionConeBypasses.Add(
            g_latestStereo.targetedOcclusionConeBypasses);
        w.targetedOcclusionRadialBypasses.Add(
            g_latestStereo.targetedOcclusionRadialBypasses);
        w.targetedOcclusionRecentBypasses.Add(
            g_latestStereo.targetedOcclusionRecentBypasses);
        w.targetedOcclusionStockCulls.Add(
            g_latestStereo.targetedOcclusionStockCulls);
        w.targetedOcclusionInvalidPoseBypasses.Add(
            g_latestStereo.targetedOcclusionInvalidPoseBypasses);
        w.targetedOcclusionCacheHits.Add(
            g_latestStereo.targetedOcclusionCacheHits);
        w.targetedOcclusionCacheMisses.Add(
            g_latestStereo.targetedOcclusionCacheMisses);
        w.targetedOcclusionCacheEvictions.Add(
            g_latestStereo.targetedOcclusionCacheEvictions);
        w.targetedOcclusionCacheOverflows.Add(
            g_latestStereo.targetedOcclusionCacheOverflows);
        w.targetedOcclusionIdentityResets.Add(
            g_latestStereo.targetedOcclusionIdentityResets);
        w.nearbyScanSectors.Add(g_latestStereo.nearbyScanSectors);
        w.nearbyScanVisibleAdded.Add(g_latestStereo.nearbyScanVisibleAdded);
        w.nearbyScanStreamingRequests.Add(
            g_latestStereo.nearbyScanStreamingRequests);
        w.nearbyScanConeRejectedSectors.Add(
            g_latestStereo.nearbyScanConeRejectedSectors);
        w.nearbyScanWallMs.Add(g_latestStereo.nearbyScanWallMs);
        w.nearbyScanCpuMs.Add(g_latestStereo.nearbyScanCpuMs);
        if (g_latestStereo.aircraftLodActive) {
            ++w.aircraftLodActiveFrames;
            w.aircraftAltitudeM.Add(g_latestStereo.aircraftAltitudeM);
            w.aircraftOrdinaryMaxSlantM.Add(
                g_latestStereo.aircraftOrdinaryMaxSlantM);
            if (std::isfinite(g_latestStereo.aircraftHeadForwardZ) &&
                g_latestStereo.aircraftHeadForwardZ >= -1.001 &&
                g_latestStereo.aircraftHeadForwardZ <= 1.001) {
                w.aircraftHeadForwardZSum +=
                    g_latestStereo.aircraftHeadForwardZ;
                ++w.aircraftHeadForwardZCount;
            }
            w.aircraftOrdinaryProbes.Add(
                g_latestStereo.aircraftOrdinaryProbes);
            w.aircraftOrdinaryQualified.Add(
                g_latestStereo.aircraftOrdinaryQualified);
            w.aircraftOrdinaryForcedVisible.Add(
                g_latestStereo.aircraftOrdinaryForcedVisible);
            w.aircraftOrdinaryRangePromotions.Add(
                g_latestStereo.aircraftOrdinaryRangePromotions);
            w.aircraftOrdinaryHorizontalRejects.Add(
                g_latestStereo.aircraftOrdinaryHorizontalRejects);
            w.aircraftOrdinarySlantRejects.Add(
                g_latestStereo.aircraftOrdinarySlantRejects);
            w.aircraftOrdinaryConeRejects.Add(
                g_latestStereo.aircraftOrdinaryConeRejects);
            w.aircraftOrdinaryForceLimit.Add(
                g_latestStereo.aircraftOrdinaryForceLimit);
            w.aircraftOrdinaryCapacity.Add(
                g_latestStereo.aircraftOrdinaryCapacity);
            w.aircraftOrdinaryOcclusionBypasses.Add(
                g_latestStereo.aircraftOrdinaryOcclusionBypasses);
            w.aircraftOrdinaryOcclusionUnconsumed.Add(
                g_latestStereo.aircraftOrdinaryOcclusionUnconsumed);
            w.aircraftOrdinaryInnerSlantRejects.Add(
                g_latestStereo.aircraftOrdinaryInnerSlantRejects);
            w.aircraftOrdinaryUnloadedRootlessForward.Add(
                g_latestStereo.aircraftOrdinaryUnloadedRootlessForward);
            w.aircraftOrdinaryPrefetchCandidates.Add(
                g_latestStereo.aircraftOrdinaryPrefetchCandidates);
            w.aircraftOrdinaryPrefetchUnique.Add(
                g_latestStereo.aircraftOrdinaryPrefetchUnique);
            w.aircraftOrdinaryPrefetchRequestCalls.Add(
                g_latestStereo.aircraftOrdinaryPrefetchRequestCalls);
            w.aircraftOrdinaryPrefetchEnqueues.Add(
                g_latestStereo.aircraftOrdinaryPrefetchEnqueues);
            w.aircraftOrdinaryPrefetchAlreadyPending.Add(
                g_latestStereo.aircraftOrdinaryPrefetchAlreadyPending);
            w.aircraftOrdinaryPrefetchQueueBlocked.Add(
                g_latestStereo.aircraftOrdinaryPrefetchQueueBlocked);
            w.aircraftOrdinaryPrefetchBudgetLimited.Add(
                g_latestStereo.aircraftOrdinaryPrefetchBudgetLimited);
            w.aircraftOrdinaryPrefetchStateAnomalies.Add(
                g_latestStereo.aircraftOrdinaryPrefetchStateAnomalies);
            w.aircraftOrdinaryOuterActive.Add(
                g_latestStereo.aircraftOrdinaryOuterActive ? 1.0 : 0.0);
            w.aircraftOrdinaryOuterRadiusM.Add(
                g_latestStereo.aircraftOrdinaryOuterRadiusM);
            w.aircraftOrdinaryOuterCandidateSectors.Add(
                g_latestStereo.aircraftOrdinaryOuterCandidateSectors);
            w.aircraftOrdinaryOuterScannedSectors.Add(
                g_latestStereo.aircraftOrdinaryOuterScannedSectors);
            w.aircraftOrdinaryOuterConeSkippedSectors.Add(
                g_latestStereo.aircraftOrdinaryOuterConeSkippedSectors);
            w.aircraftOrdinaryOuterSectorLimit.Add(
                g_latestStereo.aircraftOrdinaryOuterSectorLimit);
            w.aircraftOrdinaryOuterProbes.Add(
                g_latestStereo.aircraftOrdinaryOuterProbes);
            w.aircraftOrdinaryOuterQualified.Add(
                g_latestStereo.aircraftOrdinaryOuterQualified);
            w.aircraftOrdinaryOuterUnloaded.Add(
                g_latestStereo.aircraftOrdinaryOuterUnloaded);
            w.aircraftOrdinaryOuterAdmitted.Add(
                g_latestStereo.aircraftOrdinaryOuterAdmitted);
            w.aircraftOrdinaryOuterForceStops.Add(
                g_latestStereo.aircraftOrdinaryOuterForceStops);
            w.aircraftOrdinaryOuterFaults.Add(
                g_latestStereo.aircraftOrdinaryOuterFaults);
            w.aircraftOrdinaryOuterWallMs.Add(
                g_latestStereo.aircraftOrdinaryOuterWallMs);
            w.aircraftOrdinaryOuterCpuMs.Add(
                g_latestStereo.aircraftOrdinaryOuterCpuMs);
            w.aircraftOrdinaryOuterPhase1InnerPromotions.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1InnerPromotions);
            w.aircraftOrdinaryOuterPhase1OuterPromotions.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1OuterPromotions);
            w.aircraftOrdinaryOuterPhase1InnerAdmissions.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1InnerAdmissions);
            w.aircraftOrdinaryOuterPhase1OuterAdmissions.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1OuterAdmissions);
            w.aircraftOrdinaryOuterPhase1InnerPromotionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1InnerPromotionStops);
            w.aircraftOrdinaryOuterPhase1OuterPromotionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1OuterPromotionStops);
            w.aircraftOrdinaryOuterPhase1InnerAdmissionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1InnerAdmissionStops);
            w.aircraftOrdinaryOuterPhase1OuterAdmissionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterPhase1OuterAdmissionStops);
            w.aircraftOrdinaryPhase2BackfillAdmissions.Add(
                g_latestStereo.aircraftOrdinaryPhase2BackfillAdmissions);
            w.aircraftOrdinaryInnerLimit.Add(
                g_latestStereo.aircraftOrdinaryInnerLimit);
            w.aircraftOrdinaryOuterLimit.Add(
                g_latestStereo.aircraftOrdinaryOuterLimit);
            w.aircraftOrdinaryOuterNearLimit.Add(
                g_latestStereo.aircraftOrdinaryOuterNearLimit);
            w.aircraftOrdinaryOuterFarLimit.Add(
                g_latestStereo.aircraftOrdinaryOuterFarLimit);
            w.aircraftOrdinaryTotalLimit.Add(
                g_latestStereo.aircraftOrdinaryTotalLimit);
            w.aircraftOrdinaryOuterNearPromotions.Add(
                g_latestStereo.aircraftOrdinaryOuterNearPromotions);
            w.aircraftOrdinaryOuterFarPromotions.Add(
                g_latestStereo.aircraftOrdinaryOuterFarPromotions);
            w.aircraftOrdinaryOuterNearAdmissions.Add(
                g_latestStereo.aircraftOrdinaryOuterNearAdmissions);
            w.aircraftOrdinaryOuterFarAdmissions.Add(
                g_latestStereo.aircraftOrdinaryOuterFarAdmissions);
            w.aircraftOrdinaryOuterNearPromotionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterNearPromotionStops);
            w.aircraftOrdinaryOuterFarPromotionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterFarPromotionStops);
            w.aircraftOrdinaryOuterNearAdmissionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterNearAdmissionStops);
            w.aircraftOrdinaryOuterFarAdmissionStops.Add(
                g_latestStereo.aircraftOrdinaryOuterFarAdmissionStops);
            w.aircraftOrdinaryOuterSessionDisabled.Add(
                g_latestStereo.aircraftOrdinaryOuterSessionDisabled
                    ? 1.0 : 0.0);
            w.aircraftOrdinaryOuterRadialCandidates.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialCandidates);
            w.aircraftOrdinaryOuterRadialInnerCandidates.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialInnerCandidates);
            w.aircraftOrdinaryOuterRadialOuterCandidates.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialOuterCandidates);
            w.aircraftOrdinaryOuterRadialSelectedInner.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialSelectedInner);
            w.aircraftOrdinaryOuterRadialSelectedOuter.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialSelectedOuter);
            w.aircraftOrdinaryOuterRadialRetainedInner.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialRetainedInner);
            w.aircraftOrdinaryOuterRadialRetainedOuter.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialRetainedOuter);
            w.aircraftOrdinaryOuterRadialReplayRequested.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayRequested);
            w.aircraftOrdinaryOuterRadialReplayVisited.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayVisited);
            w.aircraftOrdinaryOuterRadialReplayCompleted.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayCompleted);
            w.aircraftOrdinaryOuterRadialReplayMisses.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayMisses);
            w.aircraftOrdinaryOuterRadialReplaySectors.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplaySectors);
            w.aircraftOrdinaryOuterRadialReplayVisible.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayVisible);
            w.aircraftOrdinaryOuterRadialReplayCulled.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayCulled);
            w.aircraftOrdinaryOuterRadialReplayStream.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayStream);
            w.aircraftOrdinaryOuterRadialReplayOther.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialReplayOther);
            w.aircraftOrdinaryOuterRadialCaptureOverflow.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialCaptureOverflow);
            w.aircraftOrdinaryOuterRadialSectorOverflow.Add(
                g_latestStereo.aircraftOrdinaryOuterRadialSectorOverflow);
        }
        w.cullAttributionFaults +=
            std::max(0, g_latestStereo.cullAttributionFaults);
        w.buildingDetailTests.Add(g_latestStereo.buildingDetailTests);
        w.buildingDetailOverrides.Add(g_latestStereo.buildingDetailOverrides);
        w.modelDrawRestoreFaults +=
            std::max(0, g_latestStereo.modelDrawRestoreFaults);
        w.ambientCarAttempts.Add(g_latestStereo.ambientCarAttempts);
        w.ambientCarBlocked.Add(g_latestStereo.ambientCarBlocked);
        w.ambientCarWantedPasses.Add(g_latestStereo.ambientCarWantedPasses);
        w.pedAmbientAttempts.Add(g_latestStereo.pedAmbientAttempts);
        w.pedAmbientSuccesses.Add(g_latestStereo.pedAmbientSuccesses);
        w.pedAmbientBlocked.Add(g_latestStereo.pedAmbientBlocked);
    }
}

void ResetGameTelemetry() {
    g_gameWindow = {};
    g_latestStereo = {};
    g_haveStereo = false;
    g_lastGameStereoSequence = -1;
    g_lastRenderedGameMs = 0.0;
    {
        std::lock_guard<std::mutex> debugLock(g_debugStatsMutex);
        g_debugStats.gameValid.store(false, std::memory_order_relaxed);
        g_debugStats.carPopulationValid.store(false, std::memory_order_relaxed);
        g_debugStats.pedPopulationValid.store(false, std::memory_order_relaxed);
        g_debugStats.cullAttributionValid.store(false, std::memory_order_relaxed);
        g_debugStats.revision.fetch_add(1, std::memory_order_release);
    }
}

void SubmitPresentFrame(const PresentFrameSample& sample) {
    if (g_presentWindow.startMs <= 0.0) g_presentWindow.startMs = sample.monoMs;
    if (sample.monoMs - g_presentWindow.startMs >= 1000.0) {
        EmitPresentWindow(sample.monoMs);
        g_presentWindow = {};
        g_presentWindow.startMs = sample.monoMs;
    }

    PresentWindow& w = g_presentWindow;
    ++w.attempts;
    w.last = sample;
    if (sample.loopPeriodMs > 0.0) w.loopPeriod.Add(sample.loopPeriodMs);
    w.consumerUpdate.Add(sample.consumerUpdateMs);
    w.consumerUpdateCpu.Add(sample.consumerUpdateCpuMs);
    w.waitFrame.Add(sample.waitFrameMs);
    w.work.Add(sample.workMs);
    w.endFrame.Add(sample.endFrameMs);
    w.threadCpu.Add(sample.threadCpuMs);
    if (sample.predictedDeltaMs > 0.0) w.predictedDelta.Add(sample.predictedDeltaMs);
    if (sample.stereoSyncWaitAttempted) {
        ++w.stereoSyncWaits;
        w.stereoSyncWait.Add(std::max(0.0, sample.stereoSyncWaitMs));
    }
    if (sample.stereoSyncWaitRescued) ++w.stereoSyncRescued;
    if (sample.stereoSyncWaitTimedOut) ++w.stereoSyncTimeouts;
    if (sample.submittedStereoSequenceAgeMs > 0.0)
        w.submittedStereoAge.Add(sample.submittedStereoSequenceAgeMs);
    w.fxaaDraws += std::max(0, sample.fxaaDraws);
    w.fxaaFallbacks += std::max(0, sample.fxaaFallbacks);
    // Error count is a persistent gauge from the XR renderer, not a per-frame
    // event counter. Keep its maximum instead of multiplying one failure by Hz.
    w.fxaaErrors = std::max(w.fxaaErrors, std::max(0, sample.fxaaErrors));
    if (sample.fxaaSubmitWallMs > 0.0)
        w.fxaaSubmitWall.Add(sample.fxaaSubmitWallMs);
    if (!sample.shouldRender) ++w.shouldNotRender;
    if (sample.layerCount == 0) ++w.noLayers;
    if (sample.stereoGenerationRace) ++w.generationRaces;

    if (!sample.endSucceeded) {
        ++w.endFailures;
        return;
    }
    ++w.successfulPresents;

    const int sequence = sample.submittedStereoSequence;
    if (sequence < 0) {
        ++w.noStereoSequence;
    } else if (sequence == g_lastSubmittedStereoSequence) {
        ++w.repeatedSequences;
    } else {
        ++w.freshSequences;
        if (g_lastSubmittedStereoSequence >= 0 && sequence > g_lastSubmittedStereoSequence + 1)
            w.sequenceJumps += sequence - g_lastSubmittedStereoSequence - 1;
        g_lastSubmittedStereoSequence = sequence;
    }
}

void ResetPresentTelemetry() {
    g_presentWindow = {};
    g_lastSubmittedStereoSequence = -1;
    {
        std::lock_guard<std::mutex> debugLock(g_debugStatsMutex);
        g_debugStats.presentValid.store(false, std::memory_order_relaxed);
        g_debugStats.revision.fetch_add(1, std::memory_order_release);
    }
}

} // namespace savr::perf
