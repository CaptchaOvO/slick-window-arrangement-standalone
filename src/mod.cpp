// Build the Windhawk mod source as an ordinary DLL.
//
// The upstream source has its own DllMain, which does not call Wh_ModInit -
// Windhawk's loader does that after mapping the DLL. We rename that DllMain
// and provide our own, then start the mod from a worker thread so that no
// file I/O or hook setup happens under the loader lock.

#include "windhawk.h"

#define DllMain SlickModDllMain
#include "slick-window-arrangement.wh.cpp"
#undef DllMain

extern "C" void SlickOnProcessAttach(HINSTANCE hinstDLL);

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason,
                               LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        SlickOnProcessAttach(hinstDLL);
    }
    return SlickModDllMain(hinstDLL, fdwReason, lpReserved);
}

// Called by the runtime just before enabling the mod.
extern "C" __declspec(dllexport) void SlickPrepareEnable() {
    g_uninitializing = false;
    LoadSettings();
}

// Turning the hooks off and on again is fragile because the mod keeps
// per-thread state. Instead, keep the (inert) hooks installed and mask out
// the three user-visible features.
extern "C" __declspec(dllexport) void SlickDisableFeatures() {
    g_settings.snapWindowsWhenDragging = false;
    g_settings.slidingAnimation = false;
    g_settings.snapWindowsWhenSliding = false;
}
