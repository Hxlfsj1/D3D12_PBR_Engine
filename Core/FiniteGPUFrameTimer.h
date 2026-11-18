#pragma once
#include "RenderDevice.h"
#include <vector>
#include <map>
#include <string>

// Only explicit finite command-line runs opt in. Read a slot after the caller's
// normal frame fence wait; never stall or enable profiling in interactive runs.
class FiniteGPUFrameTimer
{
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        bool pending=false,keep=false;
        UINT count=2;
        std::vector<std::string> names;
    };
public:
    bool Begin(RenderDevice& device,UINT slot,UINT slots,uint64_t frame,bool keep)
    {
        if(!m_queries)
        {
            char option[2]{};
            m_profilePasses=GetEnvironmentVariableA("LEARNDX_PROFILE_PASSES",option,2)==1 && (option[0]=='1' || option[0]=='2');
            m_coarse=option[0]=='2';
            m_stride=m_profilePasses?512u:2u;
            D3D12_QUERY_HEAP_DESC desc{};desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;desc.Count=slots*m_stride;
            if(FAILED(device.GetDevice()->CreateQueryHeap(&desc,IID_PPV_ARGS(&m_queries))) ||
                FAILED(device.GetCommandQueue()->GetTimestampFrequency(&m_frequency))) return false;
            m_slots.resize(slots);
            for(auto& item:m_slots)
            {
                const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
                const auto buffer=CD3DX12_RESOURCE_DESC::Buffer(m_stride*sizeof(uint64_t));
                if(FAILED(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&item.readback)))) return false;
            }
        }
        Collect(slot);
        m_slots[slot].keep=frame>=32 && keep;
        m_slots[slot].count=2;m_slots[slot].names.clear();m_slot=slot;
        s_active=m_profilePasses?this:nullptr;
        device.GetCommandList()->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*m_stride);
        return true;
    }
    void End(ID3D12GraphicsCommandList* cmd,UINT slot)
    {
        if(!m_queries) return;
        s_active=nullptr;
        cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*m_stride+1);
        cmd->ResolveQueryData(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*m_stride,m_slots[slot].count,m_slots[slot].readback.Get(),0);
        m_slots[slot].pending=true;
    }
    void Collect(UINT slot)
    {
        if(slot>=m_slots.size() || !m_slots[slot].pending) return;
        auto& item=m_slots[slot];void* mapped=nullptr;const D3D12_RANGE reads{0,item.count*sizeof(uint64_t)},noWrites{0,0};
        if(SUCCEEDED(item.readback->Map(0,&reads,&mapped)))
        {
            const auto* ticks=static_cast<const uint64_t*>(mapped);
            if(item.keep && ticks[1]>=ticks[0]) m_samples.push_back(double(ticks[1]-ticks[0])*1000.0/double(m_frequency));
            if(item.keep) for(size_t i=0;i<item.names.size();++i)
                if(ticks[3+i*2]>=ticks[2+i*2]) m_passSamples[item.names[i]].push_back(double(ticks[3+i*2]-ticks[2+i*2])*1000.0/double(m_frequency));
            item.readback->Unmap(0,&noWrites);
        }
        item.pending=false;
    }
    const std::vector<double>& Samples() const { return m_samples; }
    const auto& PassSamples() const { return m_passSamples; }
    const auto& WallSamples() const { return m_wallSamples; }
    void RecordWall(double milliseconds) { m_wallSamples.push_back(milliseconds); }
    static UINT BeginPass(ID3D12GraphicsCommandList* cmd,const std::string& name)
    {
        if(!s_active) return UINT_MAX;
        auto& timer=*s_active;auto& slot=timer.m_slots[timer.m_slot];
        if(slot.count+2>timer.m_stride) return UINT_MAX;
        const UINT index=timer.m_slot*timer.m_stride+slot.count;slot.count+=2;slot.names.push_back(name);
        cmd->EndQuery(timer.m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index);return index;
    }
    static void EndPass(ID3D12GraphicsCommandList* cmd,UINT index)
    { if(s_active && index!=UINT_MAX) cmd->EndQuery(s_active->m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index+1); }
    static std::string PassGroup(const std::string& name)
    {
        if(!s_active) return {};
        if(!s_active->m_coarse) return name;
        if(name.starts_with("SSGI.") || name.starts_with("ScreenProbes.")) return "SSGI total";
        if(name.starts_with("HBAO") || name=="ScalarTemporalAO") return "HBAO total";
        if(name=="DLSS" || name=="DeferredLighting" || name=="ShadowMap" || name=="GBuffer" || name=="PostProcess") return name;
        return "Other RDG";
    }
private:
    inline static FiniteGPUFrameTimer* s_active=nullptr;
    bool m_profilePasses=false;
    bool m_coarse=false;
    UINT m_stride=2,m_slot=0;
    std::map<std::string,std::vector<double>> m_passSamples;
    std::vector<double> m_wallSamples;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queries;
    uint64_t m_frequency=0;
    std::vector<Slot> m_slots;
    std::vector<double> m_samples;
};
