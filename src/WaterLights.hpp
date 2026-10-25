#pragma once
#include "LightList.hpp"
#include "wxl/PluginApi.h"
#include <windows.h>

// Seyris Living Azeroth's light list (LightList.hpp), when it publishes one: looked up at most once a second until it
// is found (the provider may load after Modern Water), then read as it is each frame. No client hooks or memory reads.
namespace water_lights
{
    constexpr uint32_t kMaxLights = 4096;
    inline const WXL_Api* api = nullptr;
    inline const WxlLightListV1* list = nullptr;
    inline DWORD lastProbe = 0;
    inline bool probed = false;

    inline void Init(const WXL_Api* value)
    {
        api = value;
        list = nullptr;
        probed = false;
    }

    // This frame's lights: their count, *lights the first and *stride the array's step in bytes; 0 without the list.
    inline uint32_t Lights(const WxlLightV1** lights, uint32_t* stride)
    {
        *lights = nullptr;
        *stride = 0;
        if (!list && api && api->GetInterface)
        {
            const DWORD now = GetTickCount();
            if (!probed || now - lastProbe >= 1000)
            {
                lastProbe = now;
                probed = true;
                const auto* found = static_cast<const WxlLightListV1*>(api->GetInterface(kWxlLightListName, kWxlLightListVersion));
                if (found && found->size >= sizeof(WxlLightListV1) && found->lightSize >= sizeof(WxlLightV1) && found->Lights)
                    list = found;
            }
        }
        if (!list)
            return 0;
        const WxlLightV1* first = nullptr;
        const uint32_t count = list->Lights(&first);
        if (!first || !count)
            return 0;
        *lights = first;
        *stride = list->lightSize;
        return count < kMaxLights ? count : kMaxLights;
    }

    // Whether the list is there and holds lights this frame.
    inline bool HasLights()
    {
        const WxlLightV1* lights = nullptr;
        uint32_t stride = 0;
        return Lights(&lights, &stride) > 0;
    }
}
