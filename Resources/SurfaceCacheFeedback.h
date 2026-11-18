#pragma once

#include "ResourceManager.h"

// One feedback word per instance Card, independent of its Atlas rectangle.
// Bit 0: sampled resident data. Bit 1: a ray requested an evicted Card.
class SurfaceCacheFeedback
{
public:
    static constexpr uint64_t KeepUnusedFrames=256;
    struct Entry {uint64_t lastUsed=0;bool resident=false,inRange=false,requested=false,everResident=false,needsAdmission=false;};
    void Initialize(UINT frameCount){m_frames.resize(frameCount);}
    bool Begin(RenderDevice& device,ResourceManager& resources,int slot,size_t count,bool reset)
    {
        if(reset){m_entries.assign(count,{});++m_generation;m_completed=0;}
        auto& frame=m_frames[slot];
        if(frame.pending && frame.generation==m_generation)
        {
            const D3D12_RANGE range{0,frame.count*sizeof(uint32_t)};void* mapped=nullptr;
            if(!Check(frame.readback->Map(0,&range,&mapped),"map feedback"))return false;
            const auto* flags=static_cast<const uint32_t*>(mapped);
            for(size_t i=0;i<frame.count;++i)
            {
                if(flags[i]&1u)m_entries[i].lastUsed=(std::max)(m_entries[i].lastUsed,frame.number);
                if(flags[i]&2u)m_entries[i].requested=true;
            }
            const D3D12_RANGE noWrites{0,0};frame.readback->Unmap(0,&noWrites);
            m_completed=(std::max)(m_completed,frame.number);
        }
        frame.pending=false;
        const uint64_t bytes=(std::max)(uint64_t(4),uint64_t(count)*4);
        if(frame.bytes!=bytes)
        {
            const auto gpuHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if(!Check(device.GetDevice()->CreateCommittedResource(&gpuHeap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(frame.gpu.ReleaseAndGetAddressOf())),"create feedback"))return false;
            const auto readHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
            const auto readDesc=CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if(!Check(device.GetDevice()->CreateCommittedResource(&readHeap,D3D12_HEAP_FLAG_NONE,&readDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(frame.readback.ReleaseAndGetAddressOf())),"create feedback readback"))return false;
            D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=1;
            if(!Check(device.GetDevice()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(frame.views.ReleaseAndGetAddressOf())),"create feedback view"))return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
            uav.Format=DXGI_FORMAT_R32_UINT;uav.Buffer.NumElements=UINT(bytes/4);
            device.GetDevice()->CreateUnorderedAccessView(frame.gpu.Get(),nullptr,&uav,frame.views->GetCPUDescriptorHandleForHeapStart());
            frame.bytes=bytes;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE target{};
        if(!resources.AllocateTransientSrvUavDescriptor(&frame.descriptor,&target))return false;
        device.GetDevice()->CopyDescriptorsSimple(1,target,frame.views->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto* cmd=device.GetCommandList();
        // The buffer decays to COMMON after the submitted readback copy.
        const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(frame.gpu.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1,&barrier);
        ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};cmd->SetDescriptorHeaps(1,heaps);
        const auto gpu=CD3DX12_GPU_DESCRIPTOR_HANDLE(heaps[0]->GetGPUDescriptorHandleForHeapStart(),frame.descriptor,
            device.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        const uint32_t zero[4]={};cmd->ClearUnorderedAccessViewUint(gpu,frame.views->GetCPUDescriptorHandleForHeapStart(),frame.gpu.Get(),zero,0,nullptr);
        const auto clear=CD3DX12_RESOURCE_BARRIER::UAV(frame.gpu.Get());cmd->ResourceBarrier(1,&clear);
        m_activeSlot=slot;m_recorded=true;frame.count=count;frame.number=m_frame+1;frame.generation=m_generation;
        return true;
    }
    bool Select(size_t index,bool inRange)
    {
        auto& entry=m_entries[index];
        if(!inRange){entry.inRange=false;entry.resident=false;entry.requested=false;entry.needsAdmission=false;return false;}
        if(!entry.inRange || (!entry.resident && entry.requested))
        {
            entry.resident=true;entry.lastUsed=m_frame+1;entry.needsAdmission=true;
        }
        entry.inRange=true;entry.requested=false;
        // Age against completed feedback, so in-flight uses cannot expire early.
        if(entry.resident && m_completed>=entry.lastUsed+KeepUnusedFrames)
        {entry.resident=false;++m_evictions;}
        return entry.resident;
    }
    void Accept(size_t index)
    {
        auto& entry=m_entries[index];
        if(entry.needsAdmission){if(entry.everResident)++m_reloads;entry.everResident=true;entry.needsAdmission=false;}
    }
    void Reject(size_t index)
    {
        auto& entry=m_entries[index];entry.resident=false;entry.requested=false;entry.needsAdmission=false;
    }
    void RecordReadback(RenderDevice& device)
    {
        if(!m_recorded)return;
        auto& frame=m_frames[m_activeSlot];auto* cmd=device.GetCommandList();
        const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(frame.gpu.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1,&barrier);cmd->CopyResource(frame.readback.Get(),frame.gpu.Get());
    }
    void OnSubmitted(){if(m_recorded){m_frames[m_activeSlot].pending=true;++m_frame;m_recorded=false;}}
    void CancelRecording(){m_recorded=false;}
    ID3D12Resource* GetBuffer(int slot)const{return m_frames[slot].gpu.Get();}
    uint32_t GetDescriptor(int slot)const{return m_frames[slot].descriptor;}
    uint64_t GetEvictions()const{return m_evictions;}
    uint64_t GetReloads()const{return m_reloads;}
    uint64_t GetCompletedFrame()const{return m_completed;}
    const std::vector<Entry>& GetEntries()const{return m_entries;}
private:
    struct Frame
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> gpu,readback;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views;
        uint64_t bytes=0,number=0,generation=0;size_t count=0;uint32_t descriptor=0;bool pending=false;
    };
    std::vector<Frame> m_frames;std::vector<Entry> m_entries;
    uint64_t m_frame=0,m_generation=0,m_completed=0,m_evictions=0,m_reloads=0;
    int m_activeSlot=0;bool m_recorded=false;
    static bool Check(HRESULT hr,const char* operation)
    {if(SUCCEEDED(hr))return true;ErrorLog::HRESULT(std::string("Surface Cache: ")+operation,hr);return false;}
};
