// ARM64 retail 2.11 bridge for
// CRenderer::SetupMapEntityVisibility(CEntity*, CBaseModelInfo*, float, bool).
//
// The retail function loads CBaseModelInfo::m_fDrawDistance into s3 at
// libGame.so+0x4b68e0.  The C++ wrapper has already calculated the effective
// per-entity VR value, so duplicate the verified retail prologue, put that
// value in s3, and continue at +0x4b68e4.  No shared model memory is modified.

    .text
    .p2align 2
    .global SavrSetupMapEntityVisibilityWithDrawDistance
    .hidden SavrSetupMapEntityVisibilityWithDrawDistance
    .type SavrSetupMapEntityVisibilityWithDrawDistance, %function
SavrSetupMapEntityVisibilityWithDrawDistance:
    // Original ABI plus:
    //   s1 = effective model draw distance
    //   x3 = TheCamera
    //   x4 = &CRenderer::ms_fFarClipPlane
    str     d8, [sp, #-0x30]!
    stp     x29, x30, [sp, #0x8]
    str     x21, [sp, #0x18]
    stp     x20, x19, [sp, #0x20]
    add     x29, sp, #0x8
    mov     x8, x3
    mov     x9, x4
    ldr     x10, [x1, #0x30]
    fmov    s3, s1

    adrp    x16, g_savrSetupMapEntityVisibilityContinue
    add     x16, x16, :lo12:g_savrSetupMapEntityVisibilityContinue
    ldr     x16, [x16]
    br      x16
    .size SavrSetupMapEntityVisibilityWithDrawDistance, .-SavrSetupMapEntityVisibilityWithDrawDistance

