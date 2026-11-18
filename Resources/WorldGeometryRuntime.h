#pragma once

#include "VXGIGlobalSDFRuntime.h"
#include "Settings_Manager.h"
#include "SurfaceCacheCards.h"
#include <span>

// Geometry resources shared by future world traces and surface-cache queries.
// The legacy world remains empty; it supplies only the GPU scene envelope scale.
class WorldGeometryRuntime
{
public:
    struct CardAsset
    {
        std::shared_ptr<const VXGIMeshSDFCache::Asset> geometry;
        SurfaceCacheCards::Data data;
    };
    bool Initialize(RenderDevice& device, UINT frameCount)
    {
        m_gpuScene.Initialize(frameCount);
        if (!m_globalSDF.Initialize(device, frameCount))
            return Fail(m_globalSDF.GetLastError());
        m_error.clear();
        return true;
    }

    template<class InstanceRange>
    bool Update(const InstanceRange& instances, const VXGIConfig& config, const SurfaceCacheConfig& cards)
    {
        if (!config.enabled) return true;
        // An empty input sets the envelope cell size without duplicating model
        // triangles or allocating the old sparse world voxel representation.
        if (!m_world.Update(std::span(instances).first(0), config.sdfCellSize))
            return Fail(m_world.GetLastError());
        VXGIMeshSDFCache::BuildSettings settings;
        settings.cellSize = config.sdfCellSize;
        if (!m_scene.UpdateInstances(instances, m_cache, settings))
            return Fail(m_scene.GetLastError());
        const SurfaceCacheCards::Settings cardSettings{cards.cardCellSize,cards.maxCardsPerMesh};
        for (const auto& instance : m_scene.GetInstances())
        {
            auto found=std::find_if(m_cardAssets.begin(),m_cardAssets.end(),
                [&](const CardAsset& asset){return asset.geometry==instance.asset;});
            if(found==m_cardAssets.end())
            {
                m_cardAssets.push_back({instance.asset,SurfaceCacheCards::Build(instance.asset->GetSDF(),cardSettings)});
                ++m_cardBuilds;
            }
            else if(!(found->data.settings==cardSettings))
            {
                found->data=SurfaceCacheCards::Build(instance.asset->GetSDF(),cardSettings);
                ++m_cardBuilds;
            }
        }
        m_error.clear();
        return true;
    }

    bool Record(RenderDevice& device, int slot, const DirectX::XMFLOAT3& cameraCenter,
        const VXGIConfig& config)
    {
        if (!config.enabled) return true;
        if (!m_gpuScene.Prepare(device, slot, m_world, m_scene))
            return Fail("World geometry GPU scene upload failed.");
        if (config.globalSDFEnabled)
        {
            VXGIGlobalSDF::Settings settings;
            settings.cellsPerAxis = static_cast<uint32_t>(config.globalSDFCells);
            settings.clipmapCount = static_cast<uint32_t>(config.globalSDFClipmaps);
            settings.baseCellSize = config.globalSDFCellSize;
            settings.layerCellsPerAxis = config.globalSDFLayerCells;
            if (!m_globalSDF.Record(device, slot, m_scene, m_gpuScene, cameraCenter, settings))
                return Fail(m_globalSDF.GetLastError());
        }
        m_error.clear();
        return true;
    }

    bool OnSubmitted()
    {
        if (!m_globalSDF.OnSubmitted()) return Fail(m_globalSDF.GetLastError());
        m_error.clear();
        return true;
    }

    void CancelRecording() { m_globalSDF.CancelRecording(); }

    const VXGIMeshSDFScene& GetScene() const { return m_scene; }
    const VXGIGPUScene& GetGPUScene() const { return m_gpuScene; }
    const VXGIGlobalSDFRuntime& GetGlobalSDF() const { return m_globalSDF; }
    const VXGIMeshSDFCache& GetCache() const { return m_cache; }
    const VXGIWorld& GetWorld() const { return m_world; }
    const std::vector<CardAsset>& GetCardAssets() const { return m_cardAssets; }
    uint64_t GetCardBuildCount() const { return m_cardBuilds; }
    std::vector<SurfaceCacheCards::DebugCard> BuildDebugCards() const
    {
        std::vector<SurfaceCacheCards::DebugCard> result;
        const auto& instances=m_scene.GetInstances();
        for(uint32_t i=0;i<instances.size();++i)
            for(const auto& asset:m_cardAssets)
                if(asset.geometry==instances[i].asset)
                    for(uint32_t c=0;c<asset.data.cards.size();++c)
                    {
                        const auto& card=asset.data.cards[c];
                        result.push_back({card.origin,i,card.axisX,card.extent.x,card.axisY,card.extent.y,
                            card.axisZ,card.extent.z,card.direction,c,asset.data.cellSize});
                    }
        return result;
    }
    const std::string& GetLastError() const { return m_error; }

private:
    VXGIWorld m_world;
    VXGIMeshSDFCache m_cache;
    VXGIMeshSDFScene m_scene;
    VXGIGPUScene m_gpuScene;
    VXGIGlobalSDFRuntime m_globalSDF;
    std::vector<CardAsset> m_cardAssets;
    uint64_t m_cardBuilds = 0;
    std::string m_error;

    bool Fail(const std::string& message) { m_error = message; return false; }
};
