#pragma once

#include <android/log.h>

// Everything this mod prints goes under one tag so `logcat -s SAVR` is the whole
// picture and nothing of ours hides in the game's own noise.
#define SAVR_TAG "SAVR"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  SAVR_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  SAVR_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SAVR_TAG, __VA_ARGS__)
