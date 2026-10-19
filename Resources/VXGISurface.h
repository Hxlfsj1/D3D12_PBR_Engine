#pragma once

#include <DirectXMath.h>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

// Camera-independent source state for on-demand surface shading. This does not
// allocate a uniform Lo value per triangle: UV/TBN material sampling and shadow
// visibility still happen at the actual ray hit in the GPU surface evaluator.
class VXGISurface
{
public:
    // Byte-for-byte layout contract with RenderStructs::MaterialData / HLSL.
    struct MaterialRecord
    {
        uint32_t albedoIdx=0,normalIdx=0,ormIdx=0,emissiveIdx=0;
        DirectX::XMFLOAT4 baseColorFactor={1,1,1,1};
        uint32_t isUnlit=0;
        DirectX::XMFLOAT3 emissiveFactor={};
    };
    static_assert(sizeof(MaterialRecord)==48);
    static_assert(offsetof(MaterialRecord,baseColorFactor)==16);
    static_assert(offsetof(MaterialRecord,isUnlit)==32);
    static_assert(offsetof(MaterialRecord,emissiveFactor)==36);

    struct Parameters
    {
        // Settings.lightDir convention: direction of light propagation. The
        // direction toward the sun for visibility/shading is -lightDirection.
        DirectX::XMFLOAT3 lightDirection={0,-1,0};
        DirectX::XMFLOAT3 lightColor={1,1,1};
        float materialMipLevel=0; // Explicit SampleLevel LOD, independent of camera.
        float maxRayDistance=1000;
        float rayMinDistance=0.0001f; // Reject self hits without relocating the hit.
    };
    struct Stats
    {
        uint64_t materialRecords=0;
        uint64_t memoryBytes=0;
        uint64_t sourceWorldGeometryRevision=0;
        uint64_t sourceSDFGeometryRevision=0;
        uint64_t sourceWorldMaterialRevision=0;
        uint64_t sourceWorldCoverageRevision=0;
        double updateMilliseconds=0;
        bool geometryChanged=false,materialChanged=false,lightingChanged=false,samplingChanged=false;
        bool coverageChanged=false;
    };

    template<class MaterialRange>
    bool Update(const MaterialRange& records,uint64_t worldGeometryRevision,uint64_t sdfGeometryRevision,
        uint64_t worldMaterialRevision,const Parameters& requested,uint64_t worldCoverageRevision=0)
    {
        using InputRecord=std::remove_cvref_t<decltype(*records.begin())>;
        static_assert(std::is_standard_layout_v<InputRecord> && sizeof(InputRecord)==48,
            "VXGI surface material input must use the 48-byte MaterialData layout.");
        static_assert(offsetof(InputRecord,baseColorFactor)==16 && offsetof(InputRecord,isUnlit)==32 &&
            offsetof(InputRecord,emissiveFactor)==36,"MaterialData/HLSL field offsets changed.");
        const auto start=std::chrono::steady_clock::now();
        Parameters parameters=requested;
        if(!ValidateParameters(parameters)) return false;
        try
        {
            bool recordChanges=records.size()!=m_materials.size();
            bool coverageRecordsChanged=records.size()!=m_materials.size();
            size_t index=0;
            for(const auto& source:records)
            {
                const auto record=CopyMaterial(source);
                if(!Finite(record.baseColorFactor) || !Finite(record.emissiveFactor))
                {
                    m_error="VXGI surface material factors must be finite.";
                    return false;
                }
                if(index>=m_materials.size() || std::memcmp(&record,&m_materials[index],sizeof(record))!=0) recordChanges=true;
                if(index>=m_materials.size() || record.albedoIdx!=m_materials[index].albedoIdx ||
                    record.baseColorFactor.w!=m_materials[index].baseColorFactor.w) coverageRecordsChanged=true;
                ++index;
            }
            const bool geometryChanged=!m_initialized || worldGeometryRevision!=m_stats.sourceWorldGeometryRevision ||
                sdfGeometryRevision!=m_stats.sourceSDFGeometryRevision;
            const bool materialChanged=!m_initialized || recordChanges || worldMaterialRevision!=m_stats.sourceWorldMaterialRevision;
            const bool lightingChanged=!m_initialized || !Same(parameters.lightDirection,m_parameters.lightDirection) ||
                !Same(parameters.lightColor,m_parameters.lightColor);
            const bool samplingChanged=!m_initialized || parameters.materialMipLevel!=m_parameters.materialMipLevel ||
                parameters.maxRayDistance!=m_parameters.maxRayDistance || parameters.rayMinDistance!=m_parameters.rayMinDistance;
            // Alpha coverage changes affect occlusion/classification and cached
            // neighbor edges. RGB, emission and lighting only affect source SH.
            const bool coverageChanged=!m_initialized || coverageRecordsChanged ||
                worldCoverageRevision!=m_stats.sourceWorldCoverageRevision ||
                parameters.materialMipLevel!=m_parameters.materialMipLevel;
            if(!geometryChanged && !materialChanged && !lightingChanged && !samplingChanged && !coverageChanged)
            {
                m_error.clear();
                return true;
            }
            if(recordChanges || !m_initialized)
            {
                std::vector<MaterialRecord> next;
                next.reserve(records.size());
                for(const auto& source:records) next.push_back(CopyMaterial(source));
                m_materials=std::move(next);
            }
            if(geometryChanged) ++m_geometryRevision;
            if(materialChanged) ++m_materialRevision;
            if(lightingChanged) ++m_lightingRevision;
            if(samplingChanged) ++m_samplingRevision;
            if(coverageChanged) ++m_coverageRevision;
            ++m_revision;
            m_parameters=parameters;
            m_stats.materialRecords=m_materials.size();
            m_stats.memoryBytes=sizeof(*this)+m_materials.capacity()*sizeof(MaterialRecord);
            m_stats.sourceWorldGeometryRevision=worldGeometryRevision;
            m_stats.sourceSDFGeometryRevision=sdfGeometryRevision;
            m_stats.sourceWorldMaterialRevision=worldMaterialRevision;
            m_stats.sourceWorldCoverageRevision=worldCoverageRevision;
            m_stats.geometryChanged=geometryChanged; m_stats.materialChanged=materialChanged;
            m_stats.lightingChanged=lightingChanged; m_stats.samplingChanged=samplingChanged;
            m_stats.coverageChanged=coverageChanged;
            m_stats.updateMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            m_initialized=true;
            m_error.clear();
            return true;
        }
        catch(const std::exception& exception)
        {
            m_error=std::string("VXGI surface state update failed: ")+exception.what();
            return false;
        }
    }

    const std::vector<MaterialRecord>& GetMaterials() const { return m_materials; }
    const Parameters& GetParameters() const { return m_parameters; }
    const Stats& GetStats() const { return m_stats; }
    uint64_t GetRevision() const { return m_revision; }
    uint64_t GetGeometryRevision() const { return m_geometryRevision; }
    uint64_t GetMaterialRevision() const { return m_materialRevision; }
    uint64_t GetLightingRevision() const { return m_lightingRevision; }
    uint64_t GetSamplingRevision() const { return m_samplingRevision; }
    uint64_t GetCoverageRevision() const { return m_coverageRevision; }
    const std::string& GetLastError() const { return m_error; }

private:
    static bool Finite(DirectX::XMFLOAT3 value) { return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z); }
    static bool Finite(DirectX::XMFLOAT4 value) { return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::isfinite(value.w); }
    static bool Same(DirectX::XMFLOAT3 a,DirectX::XMFLOAT3 b) { return a.x==b.x&&a.y==b.y&&a.z==b.z; }
    bool ValidateParameters(Parameters& parameters)
    {
        if(!Finite(parameters.lightDirection)||!Finite(parameters.lightColor)||parameters.lightColor.x<0||parameters.lightColor.y<0||
            parameters.lightColor.z<0||!std::isfinite(parameters.materialMipLevel)||parameters.materialMipLevel<0||
            !std::isfinite(parameters.maxRayDistance)||!std::isfinite(parameters.rayMinDistance)||parameters.rayMinDistance<0||
            parameters.maxRayDistance<=parameters.rayMinDistance)
        {
            m_error="VXGI surface requires finite sun parameters, nonnegative explicit material LOD and 0<=rayMinDistance<maxRayDistance.";
            return false;
        }
        const auto& direction=parameters.lightDirection;
        const double length=std::sqrt(double(direction.x)*direction.x+double(direction.y)*direction.y+double(direction.z)*direction.z);
        if(length==0) { m_error="VXGI sun direction must be nonzero."; return false; }
        parameters.lightDirection={float(direction.x/length),float(direction.y/length),float(direction.z/length)};
        return true;
    }
    template<class InputRecord>
    static MaterialRecord CopyMaterial(const InputRecord& source)
    {
        return {source.albedoIdx,source.normalIdx,source.ormIdx,source.emissiveIdx,source.baseColorFactor,source.isUnlit,source.emissiveFactor};
    }
    std::vector<MaterialRecord> m_materials;
    Parameters m_parameters;
    Stats m_stats;
    std::string m_error;
    uint64_t m_revision=0,m_geometryRevision=0,m_materialRevision=0,m_lightingRevision=0,m_samplingRevision=0;
    uint64_t m_coverageRevision=0;
    bool m_initialized=false;
};
