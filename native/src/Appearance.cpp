#include "Appearance.h"

#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace savr::appearance {
namespace {

constexpr int kDefaultHandSkin = HAND_SKIN_DARK;
const char* const kPath =
    "/sdcard/Android/data/com.rockstargames.gtasa/files/vr_appearance.ini";

std::once_flag g_initOnce;
std::mutex g_saveMutex;
std::atomic<int> g_handSkin{kDefaultHandSkin};

int ClampSkin(int skin) {
    return std::clamp(skin, 0, HAND_SKIN_COUNT - 1);
}

void Save() {
    std::lock_guard<std::mutex> lock(g_saveMutex);
    FILE* file = std::fopen(kPath, "w");
    if (!file) {
        LOGW("[appearance] could not save %s", kPath);
        return;
    }
    std::fprintf(file, "HandSkin=%d\n", g_handSkin.load(std::memory_order_relaxed));
    std::fclose(file);
}

void Load() {
    int handSkin = kDefaultHandSkin;
    if (FILE* file = std::fopen(kPath, "r")) {
        char line[96];
        while (std::fgets(line, sizeof(line), file)) {
            int value = 0;
            if (std::sscanf(line, "HandSkin=%d", &value) == 1)
                handSkin = value;
        }
        std::fclose(file);
    }
    g_handSkin.store(ClampSkin(handSkin), std::memory_order_relaxed);
    LOGI("[appearance] hand skin=%s", HandSkinName());
}

void EnsureInit() {
    std::call_once(g_initOnce, Load);
}

} // namespace

void Init() {
    EnsureInit();
}

int GetHandSkin() {
    EnsureInit();
    return g_handSkin.load(std::memory_order_relaxed);
}

void SetHandSkin(int skin) {
    EnsureInit();
    const int next = ClampSkin(skin);
    if (g_handSkin.exchange(next, std::memory_order_relaxed) != next)
        Save();
}

void CycleHandSkin(int direction) {
    EnsureInit();
    if (direction == 0) return;
    const int current = g_handSkin.load(std::memory_order_relaxed);
    const int next = (current + (direction > 0 ? 1 : HAND_SKIN_COUNT - 1)) % HAND_SKIN_COUNT;
    SetHandSkin(next);
}

const char* HandSkinName() {
    const int skin = g_handSkin.load(std::memory_order_relaxed);
    return skin == HAND_SKIN_DARK ? "DARK" : "LIGHT";
}

} // namespace savr::appearance
