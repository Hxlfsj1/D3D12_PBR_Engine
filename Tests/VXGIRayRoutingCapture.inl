    // Included in Capture. All resources, extra dispatches and readbacks below
    // exist only with --ray-routing-stats; normal rendering is unchanged.
    static constexpr UINT RoutingCounterCount = 64;
    static constexpr uint64_t RoutingBytes = RoutingCounterCount * sizeof(uint32_t);
    ComPtr<ID3D12RootSignature> routingRoot;
    ComPtr<ID3D12PipelineState> routingPipeline;
    ComPtr<ID3D12Resource> routingBuffer, routingZero, routingReadback;
    D3D12_GPU_VIRTUAL_ADDRESS routingConstants = 0;
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 6> routingAddresses{};
    SSGITraceConstants routingTrace{};
    Readback routingRawCopy;
    D3D12_RESOURCE_STATES routingState = D3D12_RESOURCE_STATE_COMMON;

    void InitializeRouting()
    {
        CD3DX12_ROOT_PARAMETER parameters[9];
        parameters[0].InitAsConstantBufferView(0);
        parameters[1].InitAsShaderResourceView(0);
        parameters[2].InitAsShaderResourceView(6);
        parameters[3].InitAsShaderResourceView(7);
        parameters[4].InitAsShaderResourceView(0, 1);
        parameters[5].InitAsShaderResourceView(1, 1);
        parameters[6].InitAsShaderResourceView(3, 1);
        parameters[7].InitAsShaderResourceView(4, 1);
        parameters[8].InitAsUnorderedAccessView(0, 2);
        CD3DX12_ROOT_SIGNATURE_DESC desc(9, parameters, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        ComPtr<ID3DBlob> blob;
        Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"serialize routing root");
        Check(device.GetDevice()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            IID_PPV_ARGS(&routingRoot)),"create routing root");
        const auto shader = ShaderCompiler::CompileFromFile(L"Tests/SSGIRayRoutingDiagnostics.hlsl",
            L"CSMain_Count",L"cs_6_6",{},true);
        Require(shader != nullptr,"compile routing diagnostic");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature=routingRoot.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
        Check(device.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&routingPipeline)),"create routing pipeline");
        routingBuffer=Buffer(RoutingBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        routingZero=Buffer(RoutingBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        routingReadback=Buffer(RoutingBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        void* mapped=nullptr;Check(routingZero->Map(0,nullptr,&mapped),"map routing zeros");
        std::memset(mapped,0,static_cast<size_t>(RoutingBytes));routingZero->Unmap(0,nullptr);
        report["ray_routing"]={{"enabled",true},{"counter_groups",{"all","above_ground"}},
            {"scope","Logical production rays replayed on identical live trace inputs; no GI output writes. All rays first attempt SSGI. World fallback is subsequent work, not a separate initial ray population."},
            {"geometry_mask","Unbiased receiver reconstructed with invView; world Y > 0.03 approximates the car, not an object ID."},
            {"performance_valid",false},{"frames",Json::array()}};
    }

    void RecordRouting(ID3D12GraphicsCommandList* command)
    {
        Require(routingConstants!=0,"routing trace constants missing");
        if(routingState!=D3D12_RESOURCE_STATE_COPY_DEST)
            Transition(routingBuffer.Get(),routingState,D3D12_RESOURCE_STATE_COPY_DEST);
        command->CopyBufferRegion(routingBuffer.Get(),0,routingZero.Get(),0,RoutingBytes);
        Transition(routingBuffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]={resources.GetMainDescriptorHeap()};
        command->SetDescriptorHeaps(1,heaps);
        command->SetComputeRootSignature(routingRoot.Get());
        command->SetPipelineState(routingPipeline.Get());
        command->SetComputeRootConstantBufferView(0,routingConstants);
        for(UINT index=0;index<6;++index)command->SetComputeRootShaderResourceView(1+index,routingAddresses[index]);
        command->SetComputeRootShaderResourceView(7,routingConstants);
        command->SetComputeRootUnorderedAccessView(8,routingBuffer->GetGPUVirtualAddress());
        command->Dispatch((routingTrace.outputWidth+7)/8,(routingTrace.outputHeight+7)/8,1);
        Transition(routingBuffer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        command->CopyBufferRegion(routingReadback.Get(),0,routingBuffer.Get(),0,RoutingBytes);
        routingState=D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    void CollectRouting(int cameraIndex,int mode,uint32_t sample)
    {
        static constexpr std::array<const char*,32> names={
            "dispatch_texels","sky_skipped","unlit_skipped","valid_receivers","launched_rays",
            "screen_miss","screen_hit","screen_out_of_screen","screen_radius_limit","screen_iteration_limit","screen_invalid",
            "screen_source_valid","screen_positive","screen_black","reserved14","reserved15",
            "fallback_entered","fallback_disabled","segment_invalid",
            "world_miss","world_hit","world_invalid","world_exhausted","world_stalled","world_unknown",
            "probe_support","world_source_valid","world_positive","world_no_support","world_black","reserved30","reserved31"};
        void* mapped=nullptr;Check(routingReadback->Map(0,nullptr,&mapped),"map routing counters");
        std::array<uint32_t,RoutingCounterCount> values{};std::memcpy(values.data(),mapped,static_cast<size_t>(RoutingBytes));
        routingReadback->Unmap(0,nullptr);
        // The real, uninstrumented Trace writes the exact binary hit fraction
        // in alpha. Check every frame's replay total against that live output.
        double productionHits=0;
        for(const auto& pixel:Half(routingRawCopy))
        {
            Require(std::isfinite(pixel.w)&&pixel.w>=0&&pixel.w<=1,"invalid production hit fraction");
            productionHits+=double(pixel.w)*routingTrace.rayCount;
        }
        Require(productionHits==double(uint64_t(values[6])+values[20]),"routing replay disagrees with production hit alpha");
        Json groups=Json::object();
        for(UINT group=0;group<2;++group)
        {
            const auto* v=values.data()+32*group;
            Require(uint64_t(v[3])*routingTrace.rayCount==v[4],"routing launched rays mismatch");
            uint64_t screenTotal=0;for(UINT i=5;i<=10;++i)screenTotal+=v[i];
            Require(screenTotal==v[4] && uint64_t(v[6])+v[16]+v[17]==v[4],"routing screen partition mismatch");
            uint64_t worldTotal=v[18];for(UINT i=19;i<=24;++i)worldTotal+=v[i];
            Require(worldTotal==v[16],"routing fallback partition mismatch");
            Require(uint64_t(v[25])+v[28]==v[20] && uint64_t(v[27])+v[29]==v[20],"routing hit partition mismatch");
            Require(v[25]<=v[20] && v[26]<=v[25] && v[27]<=v[26],"routing probe counters inconsistent");
            if(mode==0)Require(v[16]==0 && worldTotal==0,"screen-only routing performed fallback");
            const char* key=group==0?"all":"above_ground";
            Json named=Json::object();
            for(UINT i=0;i<32;++i)
            {
                named[names[i]]=v[i];
                auto& sum=report["ray_routing"]["totals"][key][names[i]];
                sum=(sum.is_number_unsigned()?sum.get<uint64_t>():uint64_t(0))+v[i];
            }
            groups[key]=std::move(named);
        }
        report["ray_routing"]["frames"].push_back({{"camera",cameraIndex},{"mode",mode},{"sample",sample},
            {"ray_count",routingTrace.rayCount},{"groups",groups},{"partition_checks_passed",true},
            {"production_hit_alpha_matches",true},{"production_hits_from_alpha",productionHits}});
    }
