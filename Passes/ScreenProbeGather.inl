// Included inside SSGIPass, alongside the legacy trace path for controlled A/B.
static TraceOutput AddScreenProbesToGraph(RDGBuilder& graph, ResourceManager* resources,
    PipelineManager* pipelines, int frameIndex, SSGITraceConstants trace,
    const RDGPassParameters& sourceParams, const WorldAddresses& worldAddresses,
    const SSGIConfig& config, bool previewHistory)
{
    const int index = resources->GetSSGICurrentHistoryIdx(previewHistory);
    const auto& current = resources->GetSSGIHistoryTextures(index, previewHistory);
    const auto& previous = resources->GetSSGIHistoryTextures(1 - index, previewHistory);
    if (!current.probeRadiance || !previous.probeRadiance) return {};
    auto import = [&](ID3D12Resource* resource, const char* name) {
        return graph.RegisterExternalTexture(resource, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COMMON, name);
    };
    const auto position = import(current.probePosition.Get(), "ScreenProbes.Position");
    const auto normal = import(current.probeNormal.Get(), "ScreenProbes.NormalState");
    const auto receiver = import(current.probeReceiver.Get(), "ScreenProbes.Receiver");
    const auto radiance = import(current.probeRadiance.Get(), "ScreenProbes.RawDirectionalHistory");
    const auto oldPosition = graph.CreateTextureSRVView(import(previous.probePosition.Get(), "ScreenProbes.PreviousPosition"));
    const auto oldNormal = graph.CreateTextureSRVView(import(previous.probeNormal.Get(), "ScreenProbes.PreviousNormal"));
    const auto oldRadiance = graph.CreateTextureSRVView(import(previous.probeRadiance.Get(), "ScreenProbes.PreviousDirections"));
    const auto positionUav = graph.CreateTextureUAVView(position), normalUav = graph.CreateTextureUAVView(normal);
    const auto receiverUav = graph.CreateTextureUAVView(receiver), radianceUav = graph.CreateTextureUAVView(radiance);
    const auto positionSrv = graph.CreateTextureSRVView(position), normalSrv = graph.CreateTextureSRVView(normal);
    const auto receiverSrv = graph.CreateTextureSRVView(receiver), radianceSrv = graph.CreateTextureSRVView(radiance);
    auto filtered = CreateUAVTexture(graph, trace.outputWidth * 8u, trace.outputHeight * 8u,
        RenderFormats::IndirectLighting, "ScreenProbes.FilteredDirections");
    const auto filteredUav = graph.CreateTextureUAVView(filtered);
    const auto filteredSrv = graph.CreateTextureSRVView(filtered);
    auto irradianceSH = CreateUAVTexture(graph, trace.outputWidth * 3u, trace.outputHeight * 3u,
        RenderFormats::IndirectLighting, "ScreenProbes.IrradianceSH2");
    const auto shUav = graph.CreateTextureUAVView(irradianceSH);
    const auto shSrv = graph.CreateTextureSRVView(irradianceSH);
    auto raw = CreateUAVTexture(graph, trace.width, trace.height,
        RenderFormats::IndirectLighting, "ScreenProbes.IntegratedGI");
    auto reprojected = CreateUAVTexture(graph, trace.width, trace.height,
        DXGI_FORMAT_R32G32B32A32_FLOAT, "ScreenProbes.ReprojectedGI");
    auto reprojectedFast = CreateUAVTexture(graph, trace.width, trace.height,
        DXGI_FORMAT_R32G32B32A32_FLOAT, "ScreenProbes.ReprojectedFastGI");
    auto rawUav = graph.CreateTextureUAVView(raw), reprojUav = graph.CreateTextureUAVView(reprojected);
    auto fastUav = graph.CreateTextureUAVView(reprojectedFast);
    if (!oldPosition.IsValid() || !oldNormal.IsValid() || !oldRadiance.IsValid() ||
        !positionUav.IsValid() || !normalUav.IsValid() || !receiverUav.IsValid() || !radianceUav.IsValid() ||
        !positionSrv.IsValid() || !normalSrv.IsValid() || !receiverSrv.IsValid() || !radianceSrv.IsValid() ||
        !filteredUav.IsValid() || !filteredSrv.IsValid() || !shUav.IsValid() || !shSrv.IsValid() ||
        !rawUav.IsValid() || !reprojUav.IsValid() || !fastUav.IsValid()) return {};
    trace.levels[0].y = positionSrv.descriptorIndex;
    trace.levels[0].z = normalSrv.descriptorIndex;
    trace.levels[0].w = receiverSrv.descriptorIndex;
    trace.levels[1].y = oldPosition.descriptorIndex;
    trace.levels[1].z = oldNormal.descriptorIndex;
    trace.levels[1].w = oldRadiance.descriptorIndex;
    trace.levels[2].y = radianceUav.descriptorIndex;
    trace.levels[2].w = radianceSrv.descriptorIndex;
    trace.levels[3].y = filteredUav.descriptorIndex;
    trace.levels[3].z = filteredSrv.descriptorIndex;
    trace.levels[3].w = static_cast<uint32_t>(config.screenProbeHistoryUpdates);
    trace.levels[4].y = static_cast<uint32_t>(config.raysPerScreenProbe);
    trace.levels[4].w = shUav.descriptorIndex;
    trace.levels[5].y = shSrv.descriptorIndex;
    trace.levels[5].z = positionUav.descriptorIndex;
    trace.levels[5].w = normalUav.descriptorIndex;
    trace.levels[6].y = receiverUav.descriptorIndex;
    trace.outputIndex = rawUav.descriptorIndex;
    trace.reprojectedGIIndex = reprojUav.descriptorIndex;
    trace.reprojectedFastIndex = fastUav.descriptorIndex;
    auto add = [&](const char* name, ID3D12PipelineState* pso, RDGPassParameters params) {
        const auto allocation = resources->AllocatePassConstants(frameIndex, sizeof(trace));
        if (!allocation) return RDGPassHandle{};
        memcpy(allocation.cpuAddress, &trace, sizeof(trace));
        auto address = allocation.gpuAddress;
        return graph.AddPass(name, ERDGPassFlags::Compute, params, [=](ID3D12GraphicsCommandList* cmdList) {
            // Probe shaders use one 64-lane group per receiver, not one lane per receiver.
            ExecuteComputeNoBarrier(cmdList, resources, pipelines, pso, address,
                pso == pipelines->GetScreenProbeResolvePSO() ? trace.width : trace.outputWidth * 8u,
                pso == pipelines->GetScreenProbeResolvePSO() ? trace.height : trace.outputHeight * 8u, &worldAddresses);
        });
    };
    RDGPassParameters place;
    place = sourceParams;
    place.WriteUAV(positionUav); place.WriteUAV(normalUav); place.WriteUAV(receiverUav);
    if (!add("ScreenProbes.Place", pipelines->GetScreenProbePlacePSO(), place).IsValid()) return {};
    auto rays = sourceParams;
    rays.ReadComputeSRV(oldPosition); rays.ReadComputeSRV(oldNormal); rays.ReadComputeSRV(oldRadiance);
    rays.ReadComputeSRV(receiverSrv);
    rays.WriteUAV(positionUav); rays.WriteUAV(normalUav); rays.WriteUAV(radianceUav);
    if (!add("ScreenProbes.TraceDirections", pipelines->GetScreenProbeTracePSO(), rays).IsValid()) return {};
    RDGPassParameters filter = sourceParams;
    filter.ReadComputeSRV(positionSrv); filter.ReadComputeSRV(normalSrv);
    filter.ReadComputeSRV(receiverSrv); filter.ReadComputeSRV(radianceSrv); filter.WriteUAV(filteredUav);
    if (!add("ScreenProbes.FilterDirections", pipelines->GetScreenProbeFilterPSO(), filter).IsValid()) return {};
    auto integrate = sourceParams;
    integrate.ReadComputeSRV(positionSrv); integrate.ReadComputeSRV(normalSrv); integrate.ReadComputeSRV(receiverSrv);
    integrate.ReadComputeSRV(filteredSrv); integrate.ReadComputeSRV(oldPosition); integrate.ReadComputeSRV(oldNormal);
    integrate.WriteUAV(shUav);
    if (!add("ScreenProbes.ProjectIrradianceSH2", pipelines->GetScreenProbeIntegratePSO(), integrate).IsValid()) return {};
    auto resolve = sourceParams;
    resolve.ReadComputeSRV(positionSrv); resolve.ReadComputeSRV(normalSrv); resolve.ReadComputeSRV(shSrv);
    resolve.WriteUAV(rawUav); resolve.WriteUAV(reprojUav); resolve.WriteUAV(fastUav);
    auto pass = add("ScreenProbes.ResolveFullResolution", pipelines->GetScreenProbeResolvePSO(), resolve);
    if (!pass.IsValid()) return {};
    for (auto texture : {position, normal, receiver, radiance}) graph.MarkTextureAsOutput(texture);
    return {raw, pass, raw, pass, reprojected, reprojectedFast, {}, radiance, receiver, position, normal, filtered};
}
