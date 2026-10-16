#pragma once
#include "RainSignal.hpp"
#include "wxl/PluginApi.h"
#include <windows.h>
#include <cmath>

namespace water_rain
{
    inline const WXL_Api* api = nullptr;
    inline const WxlRainSignalV1* signal = nullptr;
    inline DWORD lastProbe = 0;
    inline bool probed = false;
    inline void Init(const WXL_Api* value) { api=value; signal=nullptr; probed=false; }
    inline float Intensity()
    {
        // The provider may load alphabetically after Modern Water. No new client
        // hooks or memory probes: this only reads Living Azeroth's cached weather.
        if (!signal && api && api->GetInterface)
        {
            const DWORD now=GetTickCount();
            if (!probed || now-lastProbe>=1000)
            {
                lastProbe=now;probed=true;
                const auto* found=static_cast<const WxlRainSignalV1*>(api->GetInterface(kWxlRainSignalName,kWxlRainSignalVersion));
                if (found && found->size>=sizeof(WxlRainSignalV1) && found->Intensity) signal=found;
            }
        }
        const float value=signal ? signal->Intensity() : 0.0f;
        if (!std::isfinite(value) || value<=0.0f) return 0.0f;
        return value<1.0f ? value : 1.0f;
    }
}
