#include "MeshImportSettings.h"
#include "AppConfig.h"

namespace
{
    // In-process cache. -1 means "not loaded yet from AppConfig".
    int g_quality = -1;
}

MeshImportSettings::Quality MeshImportSettings::GetQuality()
{
    if (g_quality < 0) {
        const int v = AppConfig::LoadInt("meshImportQuality", (int)Quality::Normal);
        // Clamp out-of-range values (forward compat if enum expands).
        if (v < (int)Quality::Off || v > (int)Quality::High)
            g_quality = (int)Quality::Normal;
        else
            g_quality = v;
    }
    return (Quality)g_quality;
}

void MeshImportSettings::SetQuality(Quality q)
{
    g_quality = (int)q;
    AppConfig::SaveInt("meshImportQuality", g_quality);
}

uint32_t MeshImportSettings::TargetTriangleCount(Quality q)
{
    switch (q) {
    case Quality::Off:    return 0;       // sentinel: no decimation
    case Quality::Draft:  return 2000;
    case Quality::Normal: return 10000;
    case Quality::High:   return 50000;
    }
    return 10000;  // defensive
}
