#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// Temporal histories currently have camera-only motion vectors. Scene edits
// therefore invalidate them even when VXGI is disabled. This small snapshot
// tracks registered instance sources, independently of culling and camera LOD.
// Loaded mesh contents remain immutable; asset reloads must replace the model.
class SceneEditState
{
public:
    template<class InstanceRange>
    bool Update(InstanceRange& instances)
    {
        std::vector<Snapshot> next;
        next.reserve(instances.size());
        for(auto& instance:instances)
        {
            const Snapshot source={instance.editorId,reinterpret_cast<uintptr_t>(instance.pModel),
                {Bits(instance.translation.x),Bits(instance.translation.y),Bits(instance.translation.z),
                 Bits(instance.rotation.x),Bits(instance.rotation.y),Bits(instance.rotation.z),
                 Bits(instance.scale.x),Bits(instance.scale.y),Bits(instance.scale.z)},
                instance.customMaterialID,instance.isTransparent,instance.isCutout};
            const auto previous=std::lower_bound(m_sources.begin(),m_sources.end(),source.instanceId,
                [](const Snapshot& candidate,uint32_t identity){ return candidate.instanceId<identity; });
            // Direct TRS changes must also rebuild the raster matrix. An editor
            // setter normally marks this already, but scene scripts need not.
            if(previous==m_sources.end() || previous->instanceId!=source.instanceId || previous->transform!=source.transform)
                instance.isDirty=true;
            next.push_back(source);
        }
        std::sort(next.begin(),next.end(),[](const Snapshot& a,const Snapshot& b){ return a.instanceId<b.instanceId; });
        const bool changed=!m_initialized || next!=m_sources;
        m_sources=std::move(next);
        m_initialized=true;
        return changed;
    }

private:
    struct Snapshot
    {
        uint32_t instanceId=0;
        uintptr_t model=0;
        std::array<uint32_t,9> transform={};
        uint32_t customMaterialId=UINT32_MAX;
        bool transparent=false,cutout=false;
        bool operator==(const Snapshot&) const = default;
    };
    static uint32_t Bits(float value)
    {
        uint32_t bits;
        std::memcpy(&bits,&value,sizeof(bits));
        return bits;
    }
    std::vector<Snapshot> m_sources;
    bool m_initialized=false;
};
