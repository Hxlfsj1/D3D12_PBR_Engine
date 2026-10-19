    // Included inside Capture's private section. Test-only replay resources are
    // submitted and retired before Frame(), outside every performance sample.
    struct CavityRayInput
    {
        XMFLOAT3 worldOrigin; float rayLength;
        XMFLOAT3 worldDirection; uint32_t padding;
    };
    struct CavityConstants
    {
        uint32_t nodeCount, assetCount, instanceCount, bvhNodeCount;
        float voxelSize; uint32_t probeCount, queryCount, padding;
    };
    struct CavityProbeQuery
    {
        XMFLOAT3 radiance; float supportWeight;
        XMFLOAT3 unclampedRadiance; uint32_t negativeMask;
        uint32_t candidateCount, acceptedCount, hasSupport, traceFailures;
    };
    struct CavityRayResult
    {
        XMFLOAT3 hitPosition; float hitDistance;
        float fieldDistance; uint32_t traceStatus, instanceIndex, instanceId;
        uint32_t sdfSampleCount, modelCandidateCount, rayInputValid, padding;
        CavityProbeQuery hitQuery, originQuery;
    };
    static_assert(sizeof(CavityRayInput) == 32 && offsetof(CavityRayInput, rayLength) == 12 &&
        offsetof(CavityRayInput, worldDirection) == 16 && offsetof(CavityRayInput, padding) == 28);
    static_assert(sizeof(CavityConstants) == 32 && offsetof(CavityConstants, voxelSize) == 16 &&
        offsetof(CavityConstants, probeCount) == 20 && offsetof(CavityConstants, queryCount) == 24);
    static_assert(sizeof(CavityProbeQuery) == 48 && offsetof(CavityProbeQuery, supportWeight) == 12 &&
        offsetof(CavityProbeQuery, unclampedRadiance) == 16 && offsetof(CavityProbeQuery, negativeMask) == 28 &&
        offsetof(CavityProbeQuery, candidateCount) == 32 && offsetof(CavityProbeQuery, acceptedCount) == 36 &&
        offsetof(CavityProbeQuery, hasSupport) == 40 && offsetof(CavityProbeQuery, traceFailures) == 44);
    static_assert(sizeof(CavityRayResult) == 144 && offsetof(CavityRayResult, hitDistance) == 12 &&
        offsetof(CavityRayResult, fieldDistance) == 16 && offsetof(CavityRayResult, traceStatus) == 20 &&
        offsetof(CavityRayResult, instanceIndex) == 24 && offsetof(CavityRayResult, instanceId) == 28 &&
        offsetof(CavityRayResult, sdfSampleCount) == 32 && offsetof(CavityRayResult, modelCandidateCount) == 36 &&
        offsetof(CavityRayResult, rayInputValid) == 40 && offsetof(CavityRayResult, hitQuery) == 48 &&
        offsetof(CavityRayResult, originQuery) == 96);

    void InspectCavityRays()
    {
        Require(!ColdStart && !ReuseRaw.empty() && raw != nullptr,
            "cavity diagnostics require imported frozen RAW without cold-start capture");
        Require(!CavityRaysPath.empty(), "missing cavity ray input path");
        const uint64_t inputBytes = std::filesystem::file_size(CavityRaysPath);
        Require(inputBytes > 0 && inputBytes % sizeof(CavityRayInput) == 0 &&
            inputBytes / sizeof(CavityRayInput) <= 262144, "cavity rays must contain 1..262144 records of exactly 32 bytes");
        const auto queryCount = static_cast<uint32_t>(inputBytes / sizeof(CavityRayInput));
        std::vector<CavityRayInput> inputs(queryCount);
        std::ifstream input(CavityRaysPath, std::ios::binary);
        input.read(reinterpret_cast<char*>(inputs.data()), static_cast<std::streamsize>(inputBytes));
        Require(bool(input), "read cavity finite rays");
        const auto finite = [](XMFLOAT3 value)
            { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
        for (const auto& ray : inputs)
            Require(finite(ray.worldOrigin) && finite(ray.worldDirection) && std::isfinite(ray.rayLength) &&
                ray.rayLength > 0 && (ray.worldDirection.x != 0 || ray.worldDirection.y != 0 || ray.worldDirection.z != 0),
                "cavity ray input contains a nonfinite, zero-direction or nonpositive-length ray");
        Require(world.GetOccupiedCenters().size() <= UINT32_MAX, "cavity probe count exceeds shader addressing");

        CD3DX12_ROOT_PARAMETER roots[10];
        roots[0].InitAsConstants(8, 0);
        for (UINT i = 0; i < 4; ++i) roots[i + 1].InitAsShaderResourceView(i);
        for (UINT i = 0; i < 4; ++i) roots[i + 5].InitAsShaderResourceView(i, 1);
        roots[9].InitAsUnorderedAccessView(0);
        const CD3DX12_ROOT_SIGNATURE_DESC rootDescription(10, roots, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);
        ComPtr<ID3DBlob> rootBlob, rootErrors;
        Check(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors),
            "serialize cavity replay root signature");
        ComPtr<ID3D12RootSignature> root;
        Check(device.GetDevice()->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
            IID_PPV_ARGS(&root)), "create cavity replay root signature");
        const auto shader = ShaderCompiler::CompileFromFile(L"Shaders/VXGICavityProbeValidate.hlsl", L"CSMain", L"cs_6_6");
        Require(shader != nullptr, "compile cavity replay shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
        pipelineDescription.pRootSignature = root.Get();
        pipelineDescription.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pipeline;
        Check(device.GetDevice()->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&pipeline)),
            "create cavity replay pipeline");

        const uint64_t resultBytes = uint64_t(queryCount) * sizeof(CavityRayResult);
        auto rayUpload = Buffer(inputBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        Check(rayUpload->Map(0, &noRead, &mapped), "map cavity ray upload");
        std::memcpy(mapped, inputs.data(), static_cast<size_t>(inputBytes));
        const D3D12_RANGE written{0, static_cast<SIZE_T>(inputBytes)};
        rayUpload->Unmap(0, &written);
        auto output = Buffer(resultBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback = Buffer(resultBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        const int slot = Reset();
        Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Require(shared.Prepare(device, slot, world, meshScene, &topology), "prepare cavity replay scene buffers");
        for (const UINT buffer : {0u, 5u, 7u, 8u, 9u, 10u})
            Require(shared.Get(slot, buffer) != nullptr, "cavity replay scene buffer missing");
        const CavityConstants constants{shared.GetDistanceNodeCount(slot), shared.GetAssetCount(slot),
            shared.GetInstanceCount(slot), shared.GetInstanceBVHNodeCount(slot), world.GetVoxelSize(),
            static_cast<uint32_t>(world.GetOccupiedCenters().size()), queryCount, 0};
        auto* commands = device.GetCommandList();
        ID3D12DescriptorHeap* heaps[] = {resources.GetMainDescriptorHeap()};
        Require(heaps[0] != nullptr, "cavity replay descriptor heap missing");
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetComputeRoot32BitConstants(0, 8, &constants, 0);
        commands->SetComputeRootShaderResourceView(1, shared.Get(slot, 0)->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(2, shared.Get(slot, 5)->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(3, raw->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(4, rayUpload->GetGPUVirtualAddress());
        for (UINT i = 0; i < 4; ++i)
            commands->SetComputeRootShaderResourceView(i + 5, shared.Get(slot, i + 7)->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(9, output->GetGPUVirtualAddress());
        commands->Dispatch((queryCount + 63) / 64, 1, 1);
        Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, resultBytes);
        Submit(slot);
        Wait(slot);

        std::vector<CavityRayResult> results(queryCount);
        const D3D12_RANGE resultRange{0, static_cast<SIZE_T>(resultBytes)};
        Check(readback->Map(0, &resultRange, &mapped), "map cavity replay results");
        std::memcpy(results.data(), mapped, static_cast<size_t>(resultBytes));
        readback->Unmap(0, &noRead);
        const std::string prefix = "tmp/vxgi-stage75-performance/" + Stem();
        const std::string resultPath = prefix + "-cavity-results.bin";
        const std::string lookupPath = prefix + "-probe-lookup.bin";
        const std::string centersPath = prefix + "-probe-centers.bin";
        const auto write = [](const std::string& path, const void* data, size_t bytes)
        {
            std::ofstream file(path, std::ios::binary);
            file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
            Require(bool(file), "save cavity diagnostic binary " + path);
        };
        write(resultPath, results.data(), static_cast<size_t>(resultBytes));
        const auto& lookup = topology.GetLookup();
        const auto& centers = world.GetOccupiedCenters();
        static_assert(sizeof(VXGIProbeTopology::LookupRecord) == 16 && sizeof(XMFLOAT3) == 12);
        write(lookupPath, lookup.data(), lookup.size() * sizeof(lookup[0]));
        write(centersPath, centers.data(), centers.size() * sizeof(centers[0]));

        uint64_t hit = 0, zeroDistance = 0, noSupport = 0, shBlack = 0, negativeSHBlack = 0;
        uint64_t originNoSupport = 0, originSHBlack = 0, bothNoSupport = 0, invalid = 0, queryFailures = 0;
        uint64_t totalSDFSamples = 0;
        std::map<uint32_t, uint64_t> statuses;
        constexpr float blackEpsilon = 1e-8f, zeroDistanceEpsilon = 1e-6f;
        const auto black = [](XMFLOAT3 value)
            { return value.x <= blackEpsilon && value.y <= blackEpsilon && value.z <= blackEpsilon; };
        for (const auto& result : results)
        {
            Require(finite(result.hitPosition) && std::isfinite(result.hitDistance) && std::isfinite(result.fieldDistance) &&
                finite(result.hitQuery.radiance) && finite(result.hitQuery.unclampedRadiance) && std::isfinite(result.hitQuery.supportWeight) &&
                finite(result.originQuery.radiance) && finite(result.originQuery.unclampedRadiance) && std::isfinite(result.originQuery.supportWeight),
                "cavity replay produced nonfinite output");
            ++statuses[result.traceStatus];
            totalSDFSamples += result.sdfSampleCount;
            invalid += result.rayInputValid == 0;
            queryFailures += result.hitQuery.traceFailures + result.originQuery.traceFailures;
            originNoSupport += result.originQuery.hasSupport == 0;
            originSHBlack += result.originQuery.hasSupport != 0 && black(result.originQuery.radiance);
            if (result.traceStatus != 1) continue; // VXGI_THRESHOLD_HIT.
            ++hit;
            zeroDistance += result.hitDistance <= zeroDistanceEpsilon;
            noSupport += result.hitQuery.hasSupport == 0;
            const bool supportedBlack = result.hitQuery.hasSupport != 0 && black(result.hitQuery.radiance);
            shBlack += supportedBlack;
            negativeSHBlack += supportedBlack && result.hitQuery.negativeMask != 0;
            bothNoSupport += result.hitQuery.hasSupport == 0 && result.originQuery.hasSupport == 0;
        }
        Json statusCounts = Json::array();
        for (const auto& [status, count] : statuses) statusCounts.push_back({{"status", status}, {"count", count}});
        report["cavity_diagnostics"] = {
            {"input_path", CavityRaysPath}, {"input_stride", sizeof(CavityRayInput)}, {"query_count", queryCount},
            {"results_path", resultPath}, {"result_stride", sizeof(CavityRayResult)},
            {"probe_lookup_path", lookupPath}, {"probe_lookup_stride", sizeof(VXGIProbeTopology::LookupRecord)},
            {"probe_centers_path", centersPath}, {"probe_centers_stride", sizeof(XMFLOAT3)}, {"probe_count", centers.size()},
            {"hit_query_offset", offsetof(CavityRayResult, hitQuery)}, {"origin_query_offset", offsetof(CavityRayResult, originQuery)},
            {"threshold_hit", 1}, {"trace_status_counts", statusCounts}, {"hit", hit}, {"zero_distance_hit", zeroDistance},
            {"zero_distance_epsilon", zeroDistanceEpsilon}, {"hit_no_support", noSupport}, {"hit_supported_sh_black", shBlack},
            {"hit_supported_black_with_negative_sh", negativeSHBlack}, {"black_epsilon", blackEpsilon},
            {"origin_no_support", originNoSupport}, {"origin_supported_sh_black", originSHBlack},
            {"hit_and_origin_no_support", bothNoSupport}, {"invalid_ray_inputs", invalid},
            {"query_trace_failures", queryFailures}, {"total_sdf_samples", totalSDFSamples},
            {"scope", "Test-only finite world-ray replay before raster/performance frames. Uses unchanged production closest-two Mesh SDF threshold and raw eight-neighbor directional SH query, with a separate origin-position cache control. Origins and finite lengths are consumed unchanged; the input manifest defines their sampling and bias. This replay does not extend rays, move origins, alter geometry/probe validity, or change production shaders. Aggregates describe supplied rays, not pixel coverage or GPU timings."}
        };
        std::cout << "Cavity replay: " << queryCount << " rays, " << hit << " hits, " << zeroDistance <<
            " near-zero-distance hits, " << noSupport << " unsupported hits, " << shBlack << " supported SH-black hits.\n";
    }
