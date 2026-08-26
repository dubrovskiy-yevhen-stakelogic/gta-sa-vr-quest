#pragma once

#include <android/log.h>

// Everything this mod prints goes under one tag so `logcat -s SAVR` is the whole
// picture and nothing of ours hides in the game's own noise.
#define SAVR_TAG "SAVR"

// Developer builds (-DSAVR_DEV=ON) log everything; player builds stay quiet
// except for real errors, so a shipped headset does not spend time or storage
// narrating internals.
#ifdef SAVR_DEV
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  SAVR_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  SAVR_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#endif
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SAVR_TAG, __VA_ARGS__)
