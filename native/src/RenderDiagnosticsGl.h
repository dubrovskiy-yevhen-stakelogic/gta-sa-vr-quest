#pragma once

#include "RenderDiagnostics.h"
#include "PerfTelemetry.h"
#include "Xr.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdint>

namespace savr::render_diag {
namespace gl_detail {

struct Errors {
    GLenum first{GL_NO_ERROR};
    GLenum last{GL_NO_ERROR};
    unsigned int count{};
};

// GL errors cannot be put back. Report the pre-probe queue explicitly as
// pending through the copy, rather than attributing it to the copy itself.
inline Errors DrainErrors() {
    Errors result{};
    for (unsigned int i = 0; i < 16; ++i) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) break;
        if (result.count == 0) result.first = error;
        result.last = error;
        ++result.count;
    }
    return result;
}

struct PixelSample {
    GLint x{};
    GLint y{};
    GLubyte rgba[4]{};
    Errors errors{};
};

struct PixelSet {
    PixelSample sample[5]{};
    unsigned int valid{};
    unsigned int nonzeroRgb{};
    unsigned int minimumRgb{255};
    unsigned int maximumRgb{};
};

inline PixelSet ReadFivePixels(int width, int height) {
    PixelSet result{};
    const GLint xs[5] = {width / 2, width / 4, width - 1 - width / 4,
                          width / 4, width - 1 - width / 4};
    const GLint ys[5] = {height / 2, height / 4, height / 4,
                          height - 1 - height / 4,
                          height - 1 - height / 4};
    for (int i = 0; i < 5; ++i) {
        PixelSample& sample = result.sample[i];
        sample.x = std::clamp(xs[i], 0, width - 1);
        sample.y = std::clamp(ys[i], 0, height - 1);
        glReadPixels(sample.x, sample.y, 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, sample.rgba);
        sample.errors = DrainErrors();
        if (sample.errors.count != 0) continue;
        ++result.valid;
        if (sample.rgba[0] != 0 || sample.rgba[1] != 0 || sample.rgba[2] != 0)
            ++result.nonzeroRgb;
        for (int component = 0; component < 3; ++component) {
            const unsigned int value = sample.rgba[component];
            result.minimumRgb = std::min(result.minimumRgb, value);
            result.maximumRgb = std::max(result.maximumRgb, value);
        }
    }
    if (result.valid == 0) result.minimumRgb = 0;
    return result;
}

inline void LogPixels(int eye, int sequence, const char* target,
                      const PixelSet& pixels) {
    // Five samples are observations at these coordinates, never proof that the
    // whole frame is black. Raw RGBA also exposes alpha-only/encoding surprises.
    Log("[render.diag.pixels] eye=%d seq=%d target=%s samples=5 valid=%u "
        "nonzero_rgb=%u rgb_min/max=%u/%u scope=five_points_only",
        eye, sequence, target, pixels.valid, pixels.nonzeroRgb,
        pixels.minimumRgb, pixels.maximumRgb);
    for (int i = 0; i < 5; ++i) {
        const PixelSample& sample = pixels.sample[i];
        Log("[render.diag.pixel] eye=%d seq=%d target=%s point=%d xy=%d/%d "
            "rgba=%u/%u/%u/%u error_count=%u first/last=0x%x/0x%x",
            eye, sequence, target, i, sample.x, sample.y,
            static_cast<unsigned int>(sample.rgba[0]),
            static_cast<unsigned int>(sample.rgba[1]),
            static_cast<unsigned int>(sample.rgba[2]),
            static_cast<unsigned int>(sample.rgba[3]),
            sample.errors.count, sample.errors.first, sample.errors.last);
    }
}

// GL ES 3 has four pixel-pack fields; all four and the pixel-pack buffer must
// be controlled before passing a CPU pointer to glReadPixels. Read-buffer state
// belongs to the framebuffer, so restore it separately on each visited FBO.
inline void ProbePixels(int eye, int sequence, GLint sourceFbo,
                        GLint destinationFbo, int sourceW, int sourceH,
                        int destinationW, int destinationH) {
    GLint packBuffer = 0, packAlignment = 4, packRowLength = 0;
    GLint packSkipRows = 0, packSkipPixels = 0;
    GLint sourceReadBuffer = GL_COLOR_ATTACHMENT0;
    GLint destinationReadBuffer = GL_COLOR_ATTACHMENT0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &packRowLength);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &packSkipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &packSkipPixels);
    glGetIntegerv(GL_READ_BUFFER, &sourceReadBuffer);
    const Errors saveErrors = DrainErrors();
    if (saveErrors.count != 0) {
        Log("[render.diag.pixels] eye=%d seq=%d skipped=save_state_failed "
            "errors=%u first/last=0x%x/0x%x",
            eye, sequence, saveErrors.count, saveErrors.first, saveErrors.last);
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                       static_cast<GLuint>(destinationFbo));
    glGetIntegerv(GL_READ_BUFFER, &destinationReadBuffer);
    const Errors destinationSaveErrors = DrainErrors();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(sourceFbo));
    const Errors sourceRestoreErrors = DrainErrors();
    if (destinationSaveErrors.count != 0 || sourceRestoreErrors.count != 0) {
        Log("[render.diag.pixels] eye=%d seq=%d skipped=save_destination_failed "
            "save_errors=%u first/last=0x%x/0x%x restore_errors=%u "
            "first/last=0x%x/0x%x",
            eye, sequence, destinationSaveErrors.count,
            destinationSaveErrors.first, destinationSaveErrors.last,
            sourceRestoreErrors.count, sourceRestoreErrors.first,
            sourceRestoreErrors.last);
        return;
    }

    const double startMs = perf::MonotonicMs();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadBuffer(sourceFbo == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
    const Errors sourceSetupErrors = DrainErrors();
    PixelSet sourcePixels{};
    if (sourceSetupErrors.count == 0)
        sourcePixels = ReadFivePixels(sourceW, sourceH);
    glReadBuffer(static_cast<GLenum>(sourceReadBuffer));
    const Errors sourceReadRestoreErrors = DrainErrors();

    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                       static_cast<GLuint>(destinationFbo));
    glReadBuffer(destinationFbo == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
    const Errors destinationSetupErrors = DrainErrors();
    PixelSet destinationPixels{};
    if (destinationSetupErrors.count == 0)
        destinationPixels = ReadFivePixels(destinationW, destinationH);
    glReadBuffer(static_cast<GLenum>(destinationReadBuffer));
    const Errors destinationReadRestoreErrors = DrainErrors();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(sourceFbo));
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, packRowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, packSkipRows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, packSkipPixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
    const Errors restoreErrors = DrainErrors();
    const double wallMs = std::max(0.0, perf::MonotonicMs() - startMs);

    Log("[render.diag.pixels] eye=%d seq=%d readback_wall_ms=%.3f "
        "includes_gpu_stall=1 source_setup=0x%x destination_setup=0x%x "
        "source_read_restore=0x%x destination_read_restore=0x%x "
        "state_restore=0x%x",
        eye, sequence, wallMs, sourceSetupErrors.first,
        destinationSetupErrors.first, sourceReadRestoreErrors.first,
        destinationReadRestoreErrors.first, restoreErrors.first);
    if (sourceSetupErrors.count == 0)
        LogPixels(eye, sequence, "source", sourcePixels);
    if (destinationSetupErrors.count == 0)
        LogPixels(eye, sequence, "destination_before_overlays", destinationPixels);
}

}  // namespace gl_detail

// Caller owns a stereo read lease and throttles this probe. The source color
// is already attached to READ and the copied swapchain is attached to DRAW.
// This probe never changes attachments or shared texture-object state.
inline void ProbeEyeCopy(int eye, int sequence, GLuint sourceTexture,
                         int sourceW, int sourceH, int destW, int destH,
                         const char* copyPath,
                         const savr::xr::MobileColorState& color) {
    if (!Enabled()) return;
    const double startMs = perf::MonotonicMs();
    const gl_detail::Errors pending = gl_detail::DrainErrors();
    GLint sourceFbo = 0, destinationFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &sourceFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &destinationFbo);
    const GLenum sourceStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    const GLenum destinationStatus =
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    const GLboolean sourceIsTexture = glIsTexture(sourceTexture);
    GLint destinationEncoding = 0;
    glGetFramebufferAttachmentParameteriv(
        GL_DRAW_FRAMEBUFFER,
        destinationFbo == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &destinationEncoding);
    const gl_detail::Errors queryErrors = gl_detail::DrainErrors();

    Log("[render.diag.copy] eye=%d seq=%d path=%s tex=%u is_texture=%d "
        "source=%dx%d destination=%dx%d fbo=%d/%d status=0x%x/0x%x "
        "destination_encoding=0x%x pending_through_copy_count=%u "
        "first/last=0x%x/0x%x query_errors=%u first/last=0x%x/0x%x "
        "probe_wall_ms=%.3f",
        eye, sequence, copyPath ? copyPath : "unknown", sourceTexture,
        sourceIsTexture == GL_TRUE ? 1 : 0, sourceW, sourceH, destW, destH,
        sourceFbo, destinationFbo, sourceStatus, destinationStatus,
        static_cast<unsigned int>(destinationEncoding), pending.count,
        pending.first, pending.last, queryErrors.count, queryErrors.first,
        queryErrors.last, std::max(0.0, perf::MonotonicMs() - startMs));
    Log("[render.diag.color] eye=%d seq=%d mode=%d mult=%.6g/%.6g/%.6g "
        "add=%.6g/%.6g/%.6g red=%.6g/%.6g/%.6g/%.6g "
        "green=%.6g/%.6g/%.6g/%.6g blue=%.6g/%.6g/%.6g/%.6g",
        eye, sequence, color.mode,
        static_cast<double>(color.contrastMult[0]),
        static_cast<double>(color.contrastMult[1]),
        static_cast<double>(color.contrastMult[2]),
        static_cast<double>(color.contrastAdd[0]),
        static_cast<double>(color.contrastAdd[1]),
        static_cast<double>(color.contrastAdd[2]),
        static_cast<double>(color.redGrade[0]),
        static_cast<double>(color.redGrade[1]),
        static_cast<double>(color.redGrade[2]),
        static_cast<double>(color.redGrade[3]),
        static_cast<double>(color.greenGrade[0]),
        static_cast<double>(color.greenGrade[1]),
        static_cast<double>(color.greenGrade[2]),
        static_cast<double>(color.greenGrade[3]),
        static_cast<double>(color.blueGrade[0]),
        static_cast<double>(color.blueGrade[1]),
        static_cast<double>(color.blueGrade[2]),
        static_cast<double>(color.blueGrade[3]));

    if (!PixelsEnabled()) return;
    if (queryErrors.count != 0 || sourceStatus != GL_FRAMEBUFFER_COMPLETE ||
        destinationStatus != GL_FRAMEBUFFER_COMPLETE || sourceW <= 0 ||
        sourceH <= 0 || destW <= 0 || destH <= 0) {
        Log("[render.diag.pixels] eye=%d seq=%d skipped=invalid_fbo_or_dimensions",
            eye, sequence);
        return;
    }
    gl_detail::ProbePixels(eye, sequence, sourceFbo, destinationFbo,
                           sourceW, sourceH, destW, destH);
}

}  // namespace savr::render_diag
