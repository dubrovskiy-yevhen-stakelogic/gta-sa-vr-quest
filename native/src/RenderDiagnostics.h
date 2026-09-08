#pragma once

#include "Log.h"
#include "PerfTelemetry.h"
#include <sys/system_properties.h>
#include <cstdarg>
#include <cstring>
#include <algorithm>

// Support diagnostics remain available in SAVR_DEV=OFF. Properties are latched
// once per process; ordinary runs do no formatting, GL queries or readbacks.
namespace savr::render_diag {
inline bool Property(const char* name) {
    char text[PROP_VALUE_MAX]{};
    return __system_property_get(name, text) > 0 && std::strcmp(text, "1") == 0;
}
inline bool Enabled() {
    static const bool enabled = Property("debug.savr.render_diag");
    return enabled;
}
inline bool PixelsEnabled() {
    static const bool enabled = Property("debug.savr.render_diag_pixels");
    return Enabled() && enabled;
}
inline void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline void Log(const char* format, ...) {
    if (!Enabled()) return;
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, SAVR_TAG, format, args);
    va_end(args);
}
inline const char* Outcome(perf::StereoProjectionOutcome value) {
    constexpr const char* names[]{"not-attempted", "submitted", "no-swapchains",
        "no-safe-pair", "generation-pre", "no-timestamp", "stale", "bad-size",
        "missing-pose", "acquire-left", "wait-left", "release-left",
        "acquire-right", "wait-right", "release-right", "generation-post",
        "recovery-hold", "read-lease-conflict"};
    static_assert(sizeof(names) / sizeof(names[0]) ==
                  static_cast<unsigned>(perf::StereoProjectionOutcome::Count));
    const auto index = static_cast<unsigned>(value);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}
inline const char* Fallback(perf::StereoFallbackSource value) {
    switch (value) {
    case perf::StereoFallbackSource::None: return "none";
    case perf::StereoFallbackSource::GameSurface: return "game-surface";
    case perf::StereoFallbackSource::Black: return "black";
    case perf::StereoFallbackSource::Failed: return "failed";
    }
    return "unknown";
}
// Called only by the XR thread. Counts preserve short failures between reports.
inline void Present(const perf::PresentFrameSample& s) {
    if (!Enabled()) return;
    struct Window {
        double start{-1.0};
        unsigned frames{}, black{}, endErrors{}, claimDrops{}, fenceFailures{};
        unsigned reasons[static_cast<unsigned>(perf::StereoProjectionOutcome::Count)]{};
        double maxAge{};
    };
    static Window w;
    if (w.start < 0.0) w.start = s.monoMs;
    ++w.frames;
    w.black += s.stereoFallbackSource == perf::StereoFallbackSource::Black;
    w.endErrors += !s.endSucceeded;
    w.claimDrops += std::max(0, s.stereoWriteClaimBusyDrops);
    w.fenceFailures += std::max(0, s.stereoWriteFencePollWaitFailed) +
                       std::max(0, s.stereoWriteFenceWaitFailed);
    w.maxAge = std::max(w.maxAge, s.candidateStereoSequenceAgeMs);
    const auto reason = static_cast<unsigned>(s.stereoProjectionOutcome);
    if (reason < static_cast<unsigned>(perf::StereoProjectionOutcome::Count))
        ++w.reasons[reason];
    static bool first = true;
    if (!first && s.monoMs - w.start < 2000.0) return;
    first = false;
    Log("[render.diag] present t=%.0f window_ms=%.0f frames=%u theater=%d render=%d "
        "layers=%u end=%d outcome=%s fallback=%s live=%d seq=%d age_ms=%.2f "
        "max_candidate_ms=%.2f repeats=%d recovery=%d black=%u end_errors=%u "
        "claim_drops=%u fence_failures=%u release_mask=%u fxaa=%d/%d/%d "
        "cpu_ms=%.2f(valid=%d) gpu_ms=%.2f(valid=%d)",
        s.monoMs, s.monoMs - w.start, w.frames, s.theaterMode, s.shouldRender,
        s.layerCount, s.endResult, Outcome(s.stereoProjectionOutcome),
        Fallback(s.stereoFallbackSource), static_cast<int>(s.stereoLivenessState),
        s.submittedStereoSequence, s.submittedStereoSequenceAgeMs, w.maxAge,
        s.consecutiveStereoRepeats, s.stereoRecoveryProgress, w.black, w.endErrors,
        w.claimDrops, w.fenceFailures, s.eyeReleaseFailureMask,
        s.fxaaRequested, s.fxaaActive, s.fxaaErrors,
        s.runtimeCpuMs, s.runtimeCpuValid, s.runtimeGpuMs, s.runtimeGpuValid);
    for (unsigned i = 0; i < static_cast<unsigned>(perf::StereoProjectionOutcome::Count); ++i)
        if (w.reasons[i]) Log("[render.diag] outcomes t=%.0f reason=%s count=%u",
            s.monoMs, Outcome(static_cast<perf::StereoProjectionOutcome>(i)), w.reasons[i]);
    w = {};
    w.start = s.monoMs;
}
} // namespace savr::render_diag
