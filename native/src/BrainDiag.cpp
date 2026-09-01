// Object script-brain start-chain diagnostics (basketball investigation).
//
// The basketball court prompt is driven by a streamed script brain attached
// to the hoop models (946/947/3496/3497). The brain table is healthy and the
// script-global gates are clear, yet the prompt widget is never polled — so
// the script is not starting. These hooks log the engine chain:
//   CDummyObject::CreateObject -> CScriptsForBrains::CheckIfNewEntityNeedsScript
//     -> StartOrRequestNewStreamedScriptBrain -> StartNewStreamedScriptBrain.
// All three prologues are plain callee-save stores (no PC-relative words in
// the first 16 bytes), verified against the 2.11.311 arm64 libGame.so.
#include "BrainDiag.h"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Log.h"

namespace savr::braindiag {
namespace {

constexpr int kEntityModelOffset = 0x32;   // u16 CEntity::m_nModelIndex

int EntityModel(void* entity) {
    if (!entity) return -1;
    std::uint16_t model;
    std::memcpy(&model,
                reinterpret_cast<const std::uint8_t*>(entity) +
                    kEntityModelOffset,
                sizeof(model));
    return static_cast<int>(model);
}

bool IsHoopModel(int model) {
    return model == 946 || model == 947 || model == 3496 || model == 3497;
}

void* MakeTrampoline(void* target, const std::uint32_t expected[4],
                     void* replacement, const char* name) {
    auto* code = reinterpret_cast<std::uint32_t*>(target);
    std::uint32_t observed[4]{};
    std::memcpy(observed, code, sizeof(observed));
    if (std::memcmp(observed, expected, 16) != 0) {
        LOGE("[braindiag] %s prologue mismatch: %08x %08x %08x %08x",
             name, observed[0], observed[1], observed[2], observed[3]);
        return nullptr;
    }
    const long pageLong = sysconf(_SC_PAGESIZE);
    if (pageLong <= 0) return nullptr;
    const std::size_t pageSize = static_cast<std::size_t>(pageLong);
    void* tramp = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return nullptr;
    auto* t = reinterpret_cast<std::uint32_t*>(tramp);
    std::memcpy(t, code, 16);
    t[4] = 0x58000051u;   // LDR X17, resume literal
    t[5] = 0xD61F0220u;   // BR X17
    *reinterpret_cast<void**>(t + 6) = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(target) + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(t),
                            reinterpret_cast<char*>(t) + 32);

    const std::uintptr_t start =
        reinterpret_cast<std::uintptr_t>(code) & ~(pageSize - 1);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(code) + 16 + pageSize - 1) &
        ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(tramp, pageSize);
        return nullptr;
    }
    code[0] = 0x58000051u;   // LDR X17, replacement literal
    code[1] = 0xD61F0220u;   // BR X17
    *reinterpret_cast<void**>(code + 2) = replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 16);
    LOGI("[braindiag] %s hook installed", name);
    return tramp;
}

// --- CScriptsForBrains::CheckIfNewEntityNeedsScript(entity, type, gen) -----
using CheckFn = void (*)(void*, void*, int, void*);
CheckFn g_origCheck = nullptr;

void OnCheck(void* self, void* entity, int attachType, void* generator) {
    const int model = EntityModel(entity);
    if (IsHoopModel(model)) {
        LOGI("[braindiag] check entity model=%d attach=%d", model, attachType);
    } else {
        static std::uint32_t count = 0;
        if ((++count % 512u) == 1u)
            LOGI("[braindiag] check #%u model=%d attach=%d",
                 count, model, attachType);
    }
    if (g_origCheck) g_origCheck(self, entity, attachType, generator);
}

// --- StartOrRequestNewStreamedScriptBrain(index, entity, type, addWait) ----
using StartOrRequestFn = void (*)(void*, unsigned, void*, int, unsigned);
StartOrRequestFn g_origStartOrRequest = nullptr;

void OnStartOrRequest(void* self, unsigned index, void* entity,
                      int attachType, unsigned addToWaiting) {
    LOGI("[braindiag] start-or-request brain=%u model=%d attach=%d wait=%u",
         index, EntityModel(entity), attachType, addToWaiting);
    if (g_origStartOrRequest)
        g_origStartOrRequest(self, index, entity, attachType, addToWaiting);
}

// --- StartNewStreamedScriptBrain(index, entity, hasBrain) ------------------
using StartFn = void (*)(void*, unsigned, void*, unsigned);
StartFn g_origStart = nullptr;

void OnStart(void* self, unsigned index, void* entity, unsigned hasBrain) {
    static std::uint32_t startCount = 0;
    ++startCount;
    if (startCount <= 12 || (startCount % 500u) == 0)
        LOGI("[braindiag] START #%u brain=%u model=%d hasBrain=%u",
             startCount, index, EntityModel(entity), hasBrain);
    if (g_origStart) g_origStart(self, index, entity, hasBrain);
}

// --- CRunningScript::ShutdownThisScript ------------------------------------
// CRunningScript layout (disasm Init @0x410e70 / ProcessOneCommand @0x412f00):
// name char[8] @+0x10, base IP @+0x18, current IP @+0x20. Logging the IP
// offset at shutdown names the exact SCM instruction a brain died on.
using ShutdownFn = void (*)(void*);
ShutdownFn g_origShutdown = nullptr;
const std::uint8_t* g_scriptSpace = nullptr;   // CTheScripts::ScriptSpace
const std::uint8_t* g_streamingInfo = nullptr; // CStreaming::ms_aInfoForModel

void OnShutdown(void* script) {
    if (script) {
        char name[9]{};
        std::memcpy(name, static_cast<const std::uint8_t*>(script) + 0x10, 8);
        for (char& c : name) {
            if (c != '\0' && (c < 0x20 || c > 0x7e)) c = '?';
        }
        std::uintptr_t base = 0, ip = 0;
        std::memcpy(&base, static_cast<const std::uint8_t*>(script) + 0x18,
                    sizeof(base));
        std::memcpy(&ip, static_cast<const std::uint8_t*>(script) + 0x20,
                    sizeof(ip));
        const long offset = (base && ip >= base)
            ? static_cast<long>(ip - base) : -1;
        const bool basketball =
            std::memcmp(name, "bball", 5) == 0 ||
            std::memcmp(name, "BBALL", 5) == 0;
        static std::uint32_t count = 0;
        ++count;
        // A basketball death at an UNSEEN instruction is the trail head —
        // always log those; the two known storm offsets stay throttled.
        const bool knownStormOffset = offset == 53 || offset == 2753;
        if (basketball ? (!knownStormOffset || count <= 20 ||
                          count % 250 == 0)
                       : (count <= 40 || (count % 500u) == 0)) {
            // Locals live at script+0x6C (StartNewStreamedScriptBrain writes
            // the attached-object handle to local 0 there). basketb.scm:
            // @0000 = object handle, @0017 = matched model, @0018 = accept
            // flag; the $3368 script global is the court-brain handshake.
            std::int32_t local0, local17, local18;
            const auto* locals =
                static_cast<const std::uint8_t*>(script) + 0x6C;
            std::memcpy(&local0, locals, 4);
            std::memcpy(&local17, locals + 0x17 * 4, 4);
            std::memcpy(&local18, locals + 0x18 * 4, 4);
            std::int32_t handshake = -999, claim33cc = -999, progress = -999;
            if (g_scriptSpace) {
                std::memcpy(&handshake, g_scriptSpace + 0x3368, 4);
                std::memcpy(&claim33cc, g_scriptSpace + 0x33cc, 4);
                std::memcpy(&progress, g_scriptSpace + 0x070c, 4);
            }
            const unsigned hoopState = g_streamingInfo
                ? g_streamingInfo[946 * 20 + 0x10] : 255u;
            LOGI("[braindiag] shutdown #%u name=%s ip_off=%ld l0=%d l17=%d "
                 "l18=%d hs3368=%d cc33=%d prog=%d st946=%u",
                 count, name, offset, local0, local17, local18, handshake,
                 claim33cc, progress, hoopState);
        }
    }
    if (g_origShutdown) g_origShutdown(script);
}

// --- CRunningScript::UpdateCompareFlag(uint8) ------------------------------
// Every scripted conditional funnels its result through here. Tracing it for
// the bball brain yields an instruction-exact record of WHICH condition goes
// false on the path to the exit block.
using UpdateFlagFn = void (*)(void*, unsigned);
UpdateFlagFn g_origUpdateFlag = nullptr;

void OnUpdateCompareFlag(void* script, unsigned value) {
    if (script) {
        const auto* bytes = static_cast<const std::uint8_t*>(script);
        if (bytes[0x10] == 'b' && bytes[0x11] == 'b' && bytes[0x12] == 'a' &&
            bytes[0x13] == 'l' && bytes[0x14] == 'l') {
            static std::uint32_t traceCount = 0;
            ++traceCount;
            if (traceCount <= 600 || (traceCount % 20000u) == 0) {
                std::uintptr_t base = 0, ip = 0;
                std::memcpy(&base, bytes + 0x18, sizeof(base));
                std::memcpy(&ip, bytes + 0x20, sizeof(ip));
                const long offset = (base && ip >= base)
                    ? static_cast<long>(ip - base) : -1;
                LOGI("[braindiag] cmp #%u ip_off=%ld val=%u",
                     traceCount, offset, value & 0xffu);
            }
        }
    }
    if (g_origUpdateFlag) g_origUpdateFlag(script, value);
}

} // namespace

void Install(void* handle) {
    if (!handle) return;

    if (void* target = dlsym(handle,
            "_ZN17CScriptsForBrains27CheckIfNewEntityNeedsScriptEP7CEntityaP13CPedGenerator")) {
        static constexpr std::uint32_t kPrologue[4] = {
            0xa9bc7bfdu, 0xa9015ff8u, 0xa90257f6u, 0xa9034ff4u};
        g_origCheck = reinterpret_cast<CheckFn>(MakeTrampoline(
            target, kPrologue, reinterpret_cast<void*>(&OnCheck),
            "CheckIfNewEntityNeedsScript"));
    } else {
        LOGW("[braindiag] CheckIfNewEntityNeedsScript symbol missing");
    }

    if (void* target = dlsym(handle,
            "_ZN17CScriptsForBrains36StartOrRequestNewStreamedScriptBrainEhP7CEntityah")) {
        static constexpr std::uint32_t kPrologue[4] = {
            0xa9bb7bfdu, 0xf9000bf9u, 0xa9025ff8u, 0xa90357f6u};
        g_origStartOrRequest = reinterpret_cast<StartOrRequestFn>(
            MakeTrampoline(target, kPrologue,
                           reinterpret_cast<void*>(&OnStartOrRequest),
                           "StartOrRequestNewStreamedScriptBrain"));
    } else {
        LOGW("[braindiag] StartOrRequestNewStreamedScriptBrain symbol missing");
    }

    if (void* target = dlsym(handle,
            "_ZN17CScriptsForBrains27StartNewStreamedScriptBrainEhP7CEntityh")) {
        static constexpr std::uint32_t kPrologue[4] = {
            0xa9bc7bfdu, 0xa9015ff8u, 0xa90257f6u, 0xa9034ff4u};
        g_origStart = reinterpret_cast<StartFn>(MakeTrampoline(
            target, kPrologue, reinterpret_cast<void*>(&OnStart),
            "StartNewStreamedScriptBrain"));
    } else {
        LOGW("[braindiag] StartNewStreamedScriptBrain symbol missing");
    }

    g_scriptSpace = static_cast<const std::uint8_t*>(
        dlsym(handle, "_ZN11CTheScripts11ScriptSpaceE"));
    g_streamingInfo = static_cast<const std::uint8_t*>(
        dlsym(handle, "_ZN10CStreaming16ms_aInfoForModelE"));

    if (void* target = dlsym(handle,
            "_ZN14CRunningScript17UpdateCompareFlagEh")) {
        static constexpr std::uint32_t kPrologue[4] = {
            0x39448809u, 0x72001c3fu, 0x79424008u, 0x1a9f17eau};
        g_origUpdateFlag = reinterpret_cast<UpdateFlagFn>(MakeTrampoline(
            target, kPrologue, reinterpret_cast<void*>(&OnUpdateCompareFlag),
            "UpdateCompareFlag"));
    } else {
        LOGW("[braindiag] UpdateCompareFlag symbol missing");
    }

    if (void* target = dlsym(handle,
            "_ZN14CRunningScript18ShutdownThisScriptEv")) {
        static constexpr std::uint32_t kPrologue[4] = {
            0xa9be7bfdu, 0xa9014ff4u, 0x910003fdu, 0x39445c08u};
        g_origShutdown = reinterpret_cast<ShutdownFn>(MakeTrampoline(
            target, kPrologue, reinterpret_cast<void*>(&OnShutdown),
            "ShutdownThisScript"));
    } else {
        LOGW("[braindiag] ShutdownThisScript symbol missing");
    }
}

} // namespace savr::braindiag
