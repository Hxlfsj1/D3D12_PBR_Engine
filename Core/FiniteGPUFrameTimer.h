#pragma once
#include "RenderDevice.h"
#include <vector>

// Only explicit finite command-line runs opt in. Read a slot after the caller's
// normal frame fence wait; never stall or enable profiling in interactive runs.
class FiniteGPUFrameTimer
{
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        bool pending=false,keep=false;
    };
public:
    bool Begin(RenderDevice& device,UINT slot,UINT slots,uint64_t frame,bool keep)
    {
        if(!m_queries)
        {
            D3D12_QUERY_HEAP_DESC desc{};desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;desc.Count=slots*2;
            if(FAILED(device.GetDevice()->CreateQueryHeap(&desc,IID_PPV_ARGS(&m_queries))) ||
                FAILED(device.GetCommandQueue()->GetTimestampFrequency(&m_frequency))) return false;
            m_slots.resize(slots);
            for(auto& item:m_slots)
            {
                const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
                const auto buffer=CD3DX12_RESOURCE_DESC::Buffer(16);
                if(FAILED(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&item.readback)))) return false;
            }
        }
        Collect(slot);
        m_slots[slot].keep=frame>=32 && keep;
        device.GetCommandList()->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*2);
        return true;
    }
    void End(ID3D12GraphicsCommandList* cmd,UINT slot)
    {
        if(!m_queries) return;
        cmd->EndQuery(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*2+1);
        cmd->ResolveQueryData(m_queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*2,2,m_slots[slot].readback.Get(),0);
        m_slots[slot].pending=true;
    }
    void Collect(UINT slot)
    {
        if(slot>=m_slots.size() || !m_slots[slot].pending) return;
        auto& item=m_slots[slot];void* mapped=nullptr;const D3D12_RANGE reads{0,16},noWrites{0,0};
        if(SUCCEEDED(item.readback->Map(0,&reads,&mapped)))
        {
            const auto* ticks=static_cast<const uint64_t*>(mapped);
            if(item.keep && ticks[1]>=ticks[0]) m_samples.push_back(double(ticks[1]-ticks[0])*1000.0/double(m_frequency));
            item.readback->Unmap(0,&noWrites);
        }
        item.pending=false;
    }
    const std::vector<double>& Samples() const { return m_samples; }
private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queries;
    uint64_t m_frequency=0;
    std::vector<Slot> m_slots;
    std::vector<double> m_samples;
};
