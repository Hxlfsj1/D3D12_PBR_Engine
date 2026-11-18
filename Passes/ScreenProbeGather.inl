// Included inside SSGIPass. Uniform grid and storage atlas are separate domains.
static TraceOutput AddScreenProbesToGraph(RDGBuilder& graph, ResourceManager* resources,
    PipelineManager* pipelines, int frameIndex, SSGITraceConstants trace,
    const RDGPassParameters& sourceParams, const SSGIConfig& config, bool previewHistory,
    const SurfaceCacheGIData::Bindings& world, RDGTextureHandle emissive, bool captureDiagnostics)
{
    ScreenProbeLayout layout;
    if (!ScreenProbeLayout::Calculate(trace.width, trace.height, trace.traceDownsampleFactor,
        config.screenProbeAdaptiveEnabled, config.screenProbeAdaptiveMaxFraction, layout)) return {};
    const bool adaptive = layout.adaptiveCapacity > 0u;
    const auto feedback=graph.RegisterExternalBuffer(world.feedback,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"SurfaceCache.CardFeedback");
    const int index = resources->GetSSGICurrentHistoryIdx(previewHistory);
    const auto& current = resources->GetSSGIHistoryTextures(index, previewHistory);
    const auto& previous = resources->GetSSGIHistoryTextures(1 - index, previewHistory);
    if (current.probeLayout != layout || previous.probeLayout != layout ||
        !current.probeRadiance || !previous.probeRadiance || !current.probeAnchor ||
        !previous.probeAnchor || !current.probeSeed || !previous.probeSeed) return {};
    auto import = [&](ID3D12Resource* resource, const char* name) {
        return graph.RegisterExternalTexture(resource, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON, name);
    };
    const auto position = import(current.probePosition.Get(), "ScreenProbes.Position");
    const auto normal = import(current.probeNormal.Get(), "ScreenProbes.NormalState");
    const auto receiver = import(current.probeReceiver.Get(), "ScreenProbes.Receiver");
    const auto radiance = import(current.probeRadiance.Get(), "ScreenProbes.RawDirectionalHistory");
    const auto anchor = import(current.probeAnchor.Get(), "ScreenProbes.StableAnchor");
    const auto seed = import(current.probeSeed.Get(), "ScreenProbes.SamplingKey");
    const auto oldAnchor = graph.CreateTextureSRVView(import(previous.probeAnchor.Get(), "ScreenProbes.PreviousAnchor"));
    const auto oldPosition = graph.CreateTextureSRVView(import(previous.probePosition.Get(), "ScreenProbes.PreviousPosition"));
    const auto oldNormal = graph.CreateTextureSRVView(import(previous.probeNormal.Get(), "ScreenProbes.PreviousNormal"));
    const auto oldRadiance = graph.CreateTextureSRVView(import(previous.probeRadiance.Get(), "ScreenProbes.PreviousDirections"));
    const auto oldSeed = graph.CreateTextureSRVView(import(previous.probeSeed.Get(), "ScreenProbes.PreviousSamplingKey"));
    const auto positionUav = graph.CreateTextureUAVView(position), normalUav = graph.CreateTextureUAVView(normal);
    const auto receiverUav = graph.CreateTextureUAVView(receiver), radianceUav = graph.CreateTextureUAVView(radiance);
    const auto anchorUav = graph.CreateTextureUAVView(anchor), seedUav = graph.CreateTextureUAVView(seed);
    const auto positionSrv = graph.CreateTextureSRVView(position), normalSrv = graph.CreateTextureSRVView(normal);
    const auto receiverSrv = graph.CreateTextureSRVView(receiver), radianceSrv = graph.CreateTextureSRVView(radiance);
    const auto seedSrv = graph.CreateTextureSRVView(seed);
    auto filtered = CreateUAVTexture(graph, layout.atlasWidth * 8u, layout.atlasHeight * 8u,
        RenderFormats::IndirectLighting, "ScreenProbes.FilteredDirections");
    auto irradianceSH = CreateUAVTexture(graph, layout.atlasWidth * 3u, layout.atlasHeight * 3u,
        RenderFormats::IndirectLighting, "ScreenProbes.IrradianceSH2");
    auto historyLink = CreateUAVTexture(graph, layout.atlasWidth, layout.atlasHeight,
        DXGI_FORMAT_R32G32B32A32_UINT, "ScreenProbes.HistoryLinks");
    const auto filteredUav = graph.CreateTextureUAVView(filtered), shUav = graph.CreateTextureUAVView(irradianceSH);
    const auto filteredSrv = graph.CreateTextureSRVView(filtered), shSrv = graph.CreateTextureSRVView(irradianceSH);
    const auto linkUav = graph.CreateTextureUAVView(historyLink);
    const auto linkSrv = graph.CreateTextureSRVView(historyLink);
    const UINT denoiseWidth = TraceExtent(trace.width, config.DenoiseFactor());
    const UINT denoiseHeight = TraceExtent(trace.height, config.DenoiseFactor());
    auto raw = CreateUAVTexture(graph, denoiseWidth, denoiseHeight, RenderFormats::IndirectLighting, "ScreenProbes.IntegratedGI");
    auto reprojected = CreateUAVTexture(graph, denoiseWidth, denoiseHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, "ScreenProbes.ReprojectedGI");
    auto reprojectedFast = CreateUAVTexture(graph, denoiseWidth, denoiseHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, "ScreenProbes.ReprojectedFastGI");
    const auto rawUav = graph.CreateTextureUAVView(raw), reprojUav = graph.CreateTextureUAVView(reprojected);
    const auto fastUav = graph.CreateTextureUAVView(reprojectedFast);
    if (!oldPosition.IsValid() || !oldNormal.IsValid() || !oldRadiance.IsValid() || !oldAnchor.IsValid() || !oldSeed.IsValid() ||
        !positionUav.IsValid() || !normalUav.IsValid() || !receiverUav.IsValid() || !radianceUav.IsValid() ||
        !anchorUav.IsValid() || !seedUav.IsValid() || !positionSrv.IsValid() || !normalSrv.IsValid() ||
        !receiverSrv.IsValid() || !radianceSrv.IsValid() || !seedSrv.IsValid() ||
        !filteredUav.IsValid() || !filteredSrv.IsValid() || !shUav.IsValid() || !shSrv.IsValid() ||
        !linkUav.IsValid() || !linkSrv.IsValid() || !rawUav.IsValid() || !reprojUav.IsValid() || !fastUav.IsValid()) return {};
    trace.probeAtlasSize = {layout.atlasWidth, layout.atlasHeight};
    trace.adaptiveCapacity = layout.adaptiveCapacity;
    trace.probePositionIndex = positionSrv.descriptorIndex; trace.probeNormalIndex = normalSrv.descriptorIndex;
    trace.probeReceiverIndex = receiverSrv.descriptorIndex;
    trace.previousProbePositionIndex = oldPosition.descriptorIndex; trace.previousProbeNormalIndex = oldNormal.descriptorIndex;
    trace.previousProbeRadianceIndex = oldRadiance.descriptorIndex; trace.previousProbeAnchorIndex = oldAnchor.descriptorIndex;
    trace.probeRadianceUavIndex = radianceUav.descriptorIndex; trace.probeRadianceIndex = radianceSrv.descriptorIndex;
    trace.probeFilteredUavIndex = filteredUav.descriptorIndex; trace.probeFilteredIndex = filteredSrv.descriptorIndex;
    trace.probeHistoryLimit = static_cast<uint32_t>(config.screenProbeHistoryUpdates);
    trace.probeScreenRays = static_cast<uint32_t>(config.screenRaysPerProbe);
    trace.probeSHUavIndex = shUav.descriptorIndex; trace.probeSHIndex = shSrv.descriptorIndex;
    trace.probePositionUavIndex = positionUav.descriptorIndex; trace.probeNormalUavIndex = normalUav.descriptorIndex;
    trace.probeReceiverUavIndex = receiverUav.descriptorIndex; trace.probeAnchorUavIndex = anchorUav.descriptorIndex;
    trace.probeSeedIndex = seedSrv.descriptorIndex; trace.probeSeedUavIndex = seedUav.descriptorIndex;
    trace.previousProbeSeedIndex = oldSeed.descriptorIndex;
    trace.probeHistoryLinkIndex = linkSrv.descriptorIndex; trace.probeHistoryLinkUavIndex = linkUav.descriptorIndex;
    trace.probeJitterWidth = config.screenProbeJitterWidthFraction;
    trace.probeCameraFadeEnabled = config.cameraFadeEnabled ? 1u : 0u; trace.probeCameraFadeEnd = config.cameraFadeEnd;
    trace.outputIndex = rawUav.descriptorIndex; trace.reprojectedGIIndex = reprojUav.descriptorIndex;
    trace.reprojectedFastIndex = fastUav.descriptorIndex;
    RDGTextureHandle count, header, indices, diagnostics;
    RDGTextureSRVHandle countSrv, headerSrv, indicesSrv, maskSrv, oldCountSrv, oldHeaderSrv, oldIndicesSrv;
    RDGTextureUAVHandle countUav, headerUav, indicesUav, maskUav, diagnosticUav;
    RDGBufferHandle args;
    if (adaptive)
    {
        if (!current.probeAdaptiveCount || !current.probeTileHeader || !current.probeTileIndices ||
            !previous.probeAdaptiveCount || !previous.probeTileHeader || !previous.probeTileIndices) return {};
        count = import(current.probeAdaptiveCount.Get(), "ScreenProbes.AdaptiveCount");
        header = import(current.probeTileHeader.Get(), "ScreenProbes.AdaptiveTileHeader");
        indices = import(current.probeTileIndices.Get(), "ScreenProbes.AdaptiveTileIndices");
        countSrv = graph.CreateTextureSRVView(count); headerSrv = graph.CreateTextureSRVView(header);
        indicesSrv = graph.CreateTextureSRVView(indices);
        countUav = graph.CreateTextureUAVView(count); headerUav = graph.CreateTextureUAVView(header);
        indicesUav = graph.CreateTextureUAVView(indices);
        oldCountSrv = graph.CreateTextureSRVView(import(previous.probeAdaptiveCount.Get(), "ScreenProbes.PreviousAdaptiveCount"));
        oldHeaderSrv = graph.CreateTextureSRVView(import(previous.probeTileHeader.Get(), "ScreenProbes.PreviousAdaptiveTileHeader"));
        oldIndicesSrv = graph.CreateTextureSRVView(import(previous.probeTileIndices.Get(), "ScreenProbes.PreviousAdaptiveTileIndices"));
        auto mask = CreateUAVTexture(graph, layout.uniformWidth, layout.uniformHeight, DXGI_FORMAT_R32_UINT, "ScreenProbes.AdaptivePlacementMask");
        maskSrv = graph.CreateTextureSRVView(mask); maskUav = graph.CreateTextureUAVView(mask);
        RDGBufferDesc desc;
        desc.sizeInBytes = sizeof(D3D12_DISPATCH_ARGUMENTS) * 2u; desc.structureByteStride = sizeof(uint32_t);
        desc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        args = graph.CreateBuffer(desc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, "ScreenProbes.DispatchArgs");
        auto argsUav = graph.CreateBufferUAVView(args);
        if (!countSrv.IsValid() || !headerSrv.IsValid() || !indicesSrv.IsValid() || !countUav.IsValid() ||
            !headerUav.IsValid() || !indicesUav.IsValid() || !oldCountSrv.IsValid() || !oldHeaderSrv.IsValid() ||
            !oldIndicesSrv.IsValid() || !maskSrv.IsValid() || !maskUav.IsValid() || !argsUav.IsValid()) return {};
        trace.adaptiveCountIndex = countSrv.descriptorIndex; trace.adaptiveCountUavIndex = countUav.descriptorIndex;
        trace.adaptiveHeaderIndex = headerSrv.descriptorIndex; trace.adaptiveHeaderUavIndex = headerUav.descriptorIndex;
        trace.adaptiveIndicesIndex = indicesSrv.descriptorIndex; trace.adaptiveIndicesUavIndex = indicesUav.descriptorIndex;
        trace.adaptiveMaskIndex = maskSrv.descriptorIndex; trace.adaptiveMaskUavIndex = maskUav.descriptorIndex;
        trace.previousAdaptiveCountIndex = oldCountSrv.descriptorIndex;
        trace.previousAdaptiveHeaderIndex = oldHeaderSrv.descriptorIndex; trace.previousAdaptiveIndicesIndex = oldIndicesSrv.descriptorIndex;
        trace.adaptiveArgsUavIndex = argsUav.descriptorIndex;
    }
    if (captureDiagnostics)
    {
        diagnostics = CreateUAVTexture(graph, 32u, 1u, DXGI_FORMAT_R32_UINT, "ScreenProbes.Diagnostics");
        diagnosticUav = graph.CreateTextureUAVView(diagnostics);
        if (!diagnosticUav.IsValid()) return {};
        trace.probeDiagnosticsIndexPlusOne = diagnosticUav.descriptorIndex + 1u;
    }
    RDGTextureSRVHandle emissiveSrv, lightListSrv, lightCountSrv;
    RDGTextureUAVHandle lightListUav, lightCountUav;
    trace.emissiveSamples = static_cast<uint32_t>(config.emissiveSamplesPerProbe);
    if (!config.lumen && config.emissiveSamplesPerProbe > 0)
    {
        if (!emissive.IsValid()) return {};
        emissiveSrv = graph.CreateTextureSRVView(emissive);
        auto list = CreateUAVTexture(graph, trace.width, trace.height, DXGI_FORMAT_R32_UINT, "ScreenProbes.EmissivePixels");
        auto lights = CreateUAVTexture(graph, 1u, 1u, DXGI_FORMAT_R32_UINT, "ScreenProbes.EmissivePixelCount");
        lightListSrv = graph.CreateTextureSRVView(list); lightCountSrv = graph.CreateTextureSRVView(lights);
        lightListUav = graph.CreateTextureUAVView(list); lightCountUav = graph.CreateTextureUAVView(lights);
        if (!emissiveSrv.IsValid() || !lightListSrv.IsValid() || !lightCountSrv.IsValid() ||
            !lightListUav.IsValid() || !lightCountUav.IsValid()) return {};
        trace.emissiveSourceIndex = emissiveSrv.descriptorIndex; trace.emissiveListUavIndex = lightListUav.descriptorIndex;
        trace.emissiveCountUavIndex = lightCountUav.descriptorIndex; trace.emissiveListIndex = lightListSrv.descriptorIndex;
        trace.emissiveCountIndex = lightCountSrv.descriptorIndex;
    }
    auto add = [&](const char* name, ID3D12PipelineState* pso, RDGPassParameters params,
        UINT groupsX, UINT groupsY, bool indirect = false, UINT argumentIndex = 0u,
        UINT dispatchBase = 0u, bool counters = false) {
        auto constants = trace; constants.probeDispatchBase = dispatchBase;
        if(pso==pipelines->GetScreenProbeTracePSO())params.WriteUAV(feedback);
        if (counters && diagnosticUav.IsValid()) params.WriteUAV(diagnosticUav);
        if (indirect) params.ReadIndirectArgs(args);
        const auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(constants));
        if (!allocation || !pso) return RDGPassHandle{};
        memcpy(allocation.cpuAddress, &constants, sizeof(constants));
        const auto address = allocation.gpuAddress;
        auto* arguments = indirect ? graph.GetBufferResource(args) : nullptr;
        return graph.AddPass(name, ERDGPassFlags::Compute, params, [=](ID3D12GraphicsCommandList* commands) {
            if (!indirect)
                ExecuteComputeNoBarrier(commands, resources, pipelines, pso, address, groupsX * 8u, groupsY * 8u,&world);
            else
            {
                ID3D12DescriptorHeap* heaps[] = {resources->GetMainDescriptorHeap()};
                commands->SetDescriptorHeaps(1, heaps);
                commands->SetComputeRootSignature(pipelines->GetSSGIRootSignature());
                commands->SetPipelineState(pso);
                commands->SetComputeRootConstantBufferView(PipelineManager::ConstantBufferBinding::Constants, address);
                world.Bind(commands);
                commands->ExecuteIndirect(pipelines->GetScreenProbeDispatchSignature(), 1u, arguments,
                    uint64_t(argumentIndex) * sizeof(D3D12_DISPATCH_ARGUMENTS), nullptr, 0u);
            }
        });
    };
    const UINT gridX = layout.uniformWidth, gridY = layout.uniformHeight;
    const auto groups = [](UINT n) { return (n + 7u) / 8u; };
    auto readAdaptive = [&](RDGPassParameters& params, bool tileLists) {
        if (adaptive)
        {
            params.ReadComputeSRV(countSrv);
            if (tileLists) { params.ReadComputeSRV(headerSrv); params.ReadComputeSRV(indicesSrv); }
        }
    };
    if (adaptive || captureDiagnostics)
    {
        RDGPassParameters clear;
        if (adaptive) { clear.WriteUAV(countUav); clear.WriteUAV(headerUav); }
        if (!add("ScreenProbes.ClearAdaptive", pipelines->GetScreenProbeClearPSO(), clear,
            groups(gridX), groups(gridY), false, 0u, 0u, true).IsValid()) return {};
    }
    if (!config.lumen && config.emissiveSamplesPerProbe > 0)
    {
        RDGPassParameters clear; clear.WriteUAV(lightCountUav);
        if (!add("ScreenProbes.ClearEmissivePixels", pipelines->GetScreenEmissiveClearPSO(), clear, 1u, 1u).IsValid()) return {};
        auto build = sourceParams; build.ReadComputeSRV(emissiveSrv);
        build.WriteUAV(lightListUav); build.WriteUAV(lightCountUav);
        if (!add("ScreenProbes.BuildEmissivePixels", pipelines->GetScreenEmissiveBuildPSO(), build,
            groups(trace.width), groups(trace.height)).IsValid()) return {};
    }
    auto place = sourceParams;
    place.WriteUAV(positionUav); place.WriteUAV(normalUav); place.WriteUAV(receiverUav); place.WriteUAV(anchorUav);
    if (!add("ScreenProbes.Place", pipelines->GetScreenProbePlacePSO(), place, gridX, gridY).IsValid()) return {};
    auto prepare = sourceParams;
    prepare.WriteUAV(positionUav); prepare.WriteUAV(normalUav); prepare.WriteUAV(anchorUav);
    prepare.WriteUAV(linkUav); prepare.WriteUAV(seedUav); prepare.WriteUAV(receiverUav);
    for (auto srv : {oldPosition, oldNormal, oldRadiance, oldAnchor, oldSeed}) prepare.ReadComputeSRV(srv);
    readAdaptive(prepare, false);
    if (adaptive) for (auto srv : {oldCountSrv, oldHeaderSrv, oldIndicesSrv}) prepare.ReadComputeSRV(srv);
    if (!add("ScreenProbes.PrepareUniformHistory", pipelines->GetScreenProbePrepareHistoryPSO(), prepare,
        gridX, gridY, false, 0u, 0u, true).IsValid()) return {};
    if (adaptive)
    {
        auto mark = sourceParams;
        mark.ReadComputeSRV(positionSrv); mark.ReadComputeSRV(normalSrv); mark.WriteUAV(maskUav);
        if (!add("ScreenProbes.MarkAdaptive", pipelines->GetScreenProbeAdaptiveMarkPSO(), mark,
            gridX, groups(gridY), false, 0u, 0u, true).IsValid()) return {};
        auto spawn = sourceParams;
        spawn.ReadComputeSRV(maskSrv); spawn.WriteUAV(countUav); spawn.WriteUAV(headerUav); spawn.WriteUAV(indicesUav);
        spawn.WriteUAV(positionUav); spawn.WriteUAV(normalUav); spawn.WriteUAV(anchorUav); spawn.WriteUAV(receiverUav);
        if (!add("ScreenProbes.SpawnAdaptive", pipelines->GetScreenProbeAdaptiveSpawnPSO(), spawn,
            gridX, groups(gridY), false, 0u, 0u, true).IsValid()) return {};
        RDGPassParameters setup; setup.ReadComputeSRV(countSrv); setup.WriteUAV(args);
        if (!add("ScreenProbes.SetupDispatchArgs", pipelines->GetScreenProbeAdaptiveArgsPSO(), setup, 1u, 1u).IsValid()) return {};
        if (!add("ScreenProbes.PrepareAdaptiveHistory", pipelines->GetScreenProbePrepareHistoryPSO(), prepare,
            0u, 0u, true, 1u, layout.uniformCount, true).IsValid()) return {};
    }
    auto rays = sourceParams;
    if(lightListSrv.IsValid()){rays.ReadComputeSRV(lightListSrv);rays.ReadComputeSRV(lightCountSrv);}
    rays.ReadComputeSRV(positionSrv); rays.ReadComputeSRV(receiverSrv); rays.ReadComputeSRV(linkSrv);
    rays.ReadComputeSRV(seedSrv); rays.ReadComputeSRV(oldRadiance);
    rays.WriteUAV(normalUav); rays.WriteUAV(radianceUav); readAdaptive(rays, false);
    if (!add("ScreenProbes.TraceDirections", pipelines->GetScreenProbeTracePSO(), rays, gridX, gridY, adaptive,0u,0u,true).IsValid()) return {};
    auto filter = sourceParams;
    for (auto srv : {positionSrv, normalSrv, receiverSrv, radianceSrv}) filter.ReadComputeSRV(srv);
    filter.WriteUAV(filteredUav); readAdaptive(filter, true);
    if (!add("ScreenProbes.FilterDirections", pipelines->GetScreenProbeFilterPSO(), filter, gridX, gridY, adaptive).IsValid()) return {};
    auto integrate = sourceParams;
    for (auto srv : {positionSrv, normalSrv, filteredSrv}) integrate.ReadComputeSRV(srv);
    integrate.WriteUAV(shUav); readAdaptive(integrate, false);
    if (!add("ScreenProbes.ProjectIrradianceSH2", pipelines->GetScreenProbeIntegratePSO(), integrate, gridX, gridY, adaptive).IsValid()) return {};
    auto resolve = sourceParams;
    for (auto srv : {positionSrv, normalSrv, receiverSrv, shSrv}) resolve.ReadComputeSRV(srv);
    resolve.WriteUAV(rawUav); resolve.WriteUAV(reprojUav); resolve.WriteUAV(fastUav); readAdaptive(resolve, true);
    auto pass = add(config.DenoiseFactor() == 1u ? "ScreenProbes.ResolveFullResolution" : "ScreenProbes.ResolveLowResolution",
        pipelines->GetScreenProbeResolvePSO(), resolve, groups(denoiseWidth), groups(denoiseHeight), false, 0u, 0u, true);
    if (!pass.IsValid()) return {};
    for (auto texture : {position, normal, receiver, radiance, anchor, seed, count, header, indices})
        if (texture.IsValid()) graph.MarkTextureAsOutput(texture);
    ProbeData probes{position, normal, receiver, irradianceSH, count, header, indices, diagnostics, {}};
    return {raw, pass, raw, pass, reprojected, reprojectedFast, radiance, receiver, position, normal, filtered, probes};
}
