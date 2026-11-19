// Minimal stand-in for Windhawk's mod API, implemented by windhawk_runtime.cpp.
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install an inline hook at targetFunction. On success, *originalFunction
// receives a pointer that can be used to call the original function.
BOOL Wh_SetFunctionHook(void* targetFunction, void* hookFunction,
                        void** originalFunction);

// Read an integer from the standalone configuration file.
int Wh_GetIntSetting(const wchar_t* settingName);

// Write a diagnostic message to the log file and the debugger.
void Wh_Log(const wchar_t* format, ...);

// Mod lifecycle, called by the runtime. Implemented by the mod source.
BOOL Wh_ModInit();
void Wh_ModUninit();
void Wh_ModSettingsChanged();

#ifdef __cplusplus
}
#endif
