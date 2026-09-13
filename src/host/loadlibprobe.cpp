// Load qwebp.dll via LoadLibrary to surface the real load failure.
#include <cstdio>
#include <windows.h>

int main()
{
    fprintf(stderr, "LOADLIB-START\n");
    // Must load Qt5Core/Qt5Gui first so the plugin's imports resolve here.
    HMODULE core = LoadLibraryA("Qt5Core.dll");
    fprintf(stderr, "Qt5Core:%p\n", (void*)core);
    HMODULE gui = LoadLibraryA("Qt5Gui.dll");
    fprintf(stderr, "Qt5Gui:%p\n", (void*)gui);
    SetLastError(0);
    HMODULE webp = LoadLibraryA("libwebp-7.dll");
    fprintf(stderr, "libwebp-7:%p err=%lu\n", (void*)webp, GetLastError());
    HMODULE webpdemux = LoadLibraryA("libwebpdemux-2.dll");
    fprintf(stderr, "libwebpdemux-2:%p err=%lu\n", (void*)webpdemux, GetLastError());
    HMODULE webpmux = LoadLibraryA("libwebpmux-3.dll");
    fprintf(stderr, "libwebpmux-3:%p err=%lu\n", (void*)webpmux, GetLastError());
    SetLastError(0);
    HMODULE q = LoadLibraryA("imageformats/qwebp.dll");
    fprintf(stderr, "qwebp:%p err=%lu\n", (void*)q, GetLastError());
    fprintf(stderr, "LOADLIB-END\n");
    return 0;
}
