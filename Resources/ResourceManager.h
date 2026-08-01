#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

#include <stb_image.h>
#include "stdafx.h"
#include "RenderDevice.h"
#include "PBR_Model.h"
#include "IBLBaker.h"
#include "Settings_Manager.h"
#include "RenderStructs.h"
#include "RDGResourceLease.h"

#include <map>
#include <vector>
#include <string>
#include <unordered_map>
#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>
#include <algorithm>
#include <memory>

using Microsoft::WRL::ComPtr;

class ResourceManager
{
public:
    ResourceManager()
    {
        srvIdx = 0;
        srvDescriptorSize = 0;
        iblPrefilterIdx = 0;
        iblBRDFIdx = 0;
        iblEnvCubeIdx = 0;
        dummyAlbedoIdx = 0;
        dummyNormalIdx = 0;
        dummyORMIdx = 0;
    }

    ~ResourceManager()
    {
    }

    // What is loaded :
    // 1. Dummy textures
    // 2. Real textures and model
    // 3. IBL slots (pre-allocate)
    // 4. Constant buffers
    // 5. Sky box' s box
    bool LoadAssets(RenderDevice* dc, const std::vector<InstanceDesc>& sceneConfig, int frameBufferCount)
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = MaxSrvUavDescriptors;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = dc->GetDevice()->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mainDescriptorHeap));

        if (FAILED(hr))
        {
            return false;
        }

        srvDescriptorSize = dc->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        srvIdx = 0;
        m_srvFrameBufferCount = frameBufferCount > 0 ? static_cast<UINT>(frameBufferCount) : 1;

        unsigned char colorWhite[] = { 255, 255, 255, 255 };
        unsigned char colorFlatNormal[] = { 128, 128, 255, 255 };
        unsigned char colorDefaultORM[] = { 255, 128, 0, 255 };
        unsigned char colorBlack[] = { 0, 0, 0, 255 };

        ComPtr<ID3D12Resource> localAlbedoUpload;
        ComPtr<ID3D12Resource> localNormalUpload;
        ComPtr<ID3D12Resource> localORMUpload;
        ComPtr<ID3D12Resource> localEmissiveUpload;

        CreateDummyTexture(dc->GetDevice(), dc->GetCommandList(), colorWhite, dummyAlbedo, localAlbedoUpload);
        CreateDummyTexture(dc->GetDevice(), dc->GetCommandList(), colorFlatNormal, dummyNormal, localNormalUpload);
        CreateDummyTexture(dc->GetDevice(), dc->GetCommandList(), colorDefaultORM, dummyORM, localORMUpload);
        CreateDummyTexture(dc->GetDevice(), dc->GetCommandList(), colorBlack, dummyEmissive, localEmissiveUpload);

        auto CreateSrvForDummy = [&](ComPtr<ID3D12Resource>& tex, UINT& idx)
            {
                idx = srvIdx++;
                CD3DX12_CPU_DESCRIPTOR_HANDLE h(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), idx, srvDescriptorSize);

                D3D12_SHADER_RESOURCE_VIEW_DESC srv = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
                srv.Texture2D.MipLevels = 1;

                dc->GetDevice()->CreateShaderResourceView(tex.Get(), &srv, h);
            };

        CreateSrvForDummy(dummyAlbedo, dummyAlbedoIdx);
        CreateSrvForDummy(dummyNormal, dummyNormalIdx);
        CreateSrvForDummy(dummyORM, dummyORMIdx);
        CreateSrvForDummy(dummyEmissive, dummyEmissiveIdx);

        DirectX::ResourceUploadBatch resourceUpload(dc->GetDevice());
        resourceUpload.Begin();

        for (const auto& desc : sceneConfig)
        {
            // If the model data is not found in the dictionary, fetch it from the hard drive
            // This ensures a single copy of the model data while allowing for multiple unique transformations, which is the essence of instancing
            if (myModels.find(desc.modelPath) == myModels.end())
            {
                myModels[desc.modelPath] = std::make_unique<Model>(dc->GetDevice(), dc->GetCommandList(), resourceUpload, desc.modelPath);
            }

            ModelInstance instance;
            instance.name = desc.name;
            instance.pModel = myModels[desc.modelPath].get();
            instance.translation = desc.pos;
            instance.rotation = desc.rot;
            instance.scale = desc.scale;
            instance.isTransparent = desc.isTransparent;
            instance.isCutout = desc.isCutout;

            m_sceneInstances.push_back(instance);
        }

        auto uploadFinished = resourceUpload.End(dc->GetCommandQueue());
        uploadFinished.wait();

        for (auto& pair : myModels)
        {
            for (auto& tex : pair.second->textures_loaded)
            {
                if (tex.Resource && textureSrvIndices.find(tex.Resource.Get()) == textureSrvIndices.end())
                {
                    CD3DX12_CPU_DESCRIPTOR_HANDLE h(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), srvIdx, srvDescriptorSize);

                    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                    srv.Format = tex.Resource->GetDesc().Format;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Texture2D.MipLevels = tex.Resource->GetDesc().MipLevels;

                    dc->GetDevice()->CreateShaderResourceView(tex.Resource.Get(), &srv, h);
                    textureSrvIndices[tex.Resource.Get()] = srvIdx++;
                }
            }
        }

        iblPrefilterIdx = srvIdx++;
        iblBRDFIdx = srvIdx++;
        iblEnvCubeIdx = srvIdx++;

        constantBufferUploadHeap.resize(frameBufferCount);
        cbvGPUAddress.resize(frameBufferCount);
        m_rdgTransientResourcePools.resize(frameBufferCount);

        for (int i = 0; i < frameBufferCount; ++i)
        {
            auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto buf = CD3DX12_RESOURCE_DESC::Buffer(4096 * 4096);

            dc->GetDevice()->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBufferUploadHeap[i]));
            constantBufferUploadHeap[i]->Map(0, nullptr, reinterpret_cast<void**>(&cbvGPUAddress[i]));
        }

        float cubeVerts[] =
        {
            -1,1,-1, 1,1,-1, 1,-1,-1, -1,1,-1, 1,-1,-1, -1,-1,-1,
            -1,1,1, -1,-1,1, 1,-1,1, -1,1,1, 1,-1,1, 1,1,1,
            -1,1,1, 1,1,1, 1,1,-1, -1,1,1, 1,1,-1, -1,1,-1,
            -1,-1,1, -1,-1,-1, 1,-1,-1, -1,-1,1, 1,-1,-1, 1,-1,1,
            -1,1,1, -1,1,-1, -1,-1,-1, -1,1,1, -1,-1,-1, -1,-1,1,
            1,1,1, 1,-1,1, 1,-1,-1, 1,1,1, 1,-1,-1, 1,1,-1
        };

        auto upHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto vDescC = CD3DX12_RESOURCE_DESC::Buffer(sizeof(cubeVerts));

        dc->GetDevice()->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &vDescC, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&skyboxVB));

        void* pD;
        skyboxVB->Map(0, nullptr, &pD);
        memcpy(pD, cubeVerts, sizeof(cubeVerts));
        skyboxVB->Unmap(0, nullptr);

        skyboxVBV = { skyboxVB->GetGPUVirtualAddress(), sizeof(cubeVerts), 12 };

        dc->GetCommandList()->Close();

        ID3D12CommandList* lists[] = { dc->GetCommandList() };
        dc->GetCommandQueue()->ExecuteCommandLists(1, lists);

        int frameIndex = dc->GetSwapChain()->GetCurrentBackBufferIndex();
        dc->GetFenceValue(frameIndex)++;
        dc->GetCommandQueue()->Signal(dc->GetFence(frameIndex), dc->GetFenceValue(frameIndex));

        if (dc->GetFence(frameIndex)->GetCompletedValue() < dc->GetFenceValue(frameIndex))
        {
            dc->GetFence(frameIndex)->SetEventOnCompletion(dc->GetFenceValue(frameIndex), dc->GetFenceEvent());
            WaitForSingleObject(dc->GetFenceEvent(), INFINITE);
        }

        // Group identical meshes together to facilitate hardware instancing
        std::sort(m_sceneInstances.begin(), m_sceneInstances.end(), [](const ModelInstance& a, const ModelInstance& b)
            {
                return a.pModel < b.pModel;
            });

        return true;
    }

    void BuildGlobalMaterialPool(RenderDevice* dc)
    {
        dc->GetCommandAllocator(0)->Reset();
        dc->GetCommandList()->Reset(dc->GetCommandAllocator(0), nullptr);

        std::vector<MaterialData> globalMaterials;
        auto cmdList = dc->GetCommandList();

        for (auto& pair : myModels)
        {
            for (auto& mesh : pair.second->meshes)
            {
                MaterialData mat = {};
                mat.albedoIdx = dummyAlbedoIdx;
                mat.normalIdx = dummyNormalIdx;
                mat.ormIdx = dummyORMIdx;
                mat.emissiveIdx = dummyEmissiveIdx;
                mat.baseColorFactor = mesh.baseColorFactor;
                mat.isUnlit = mesh.isUnlit;

                for (auto& tex : mesh.textures)
                {
                    switch (tex.type)
                    {
                    case TextureType::Albedo: mat.albedoIdx = textureSrvIndices[tex.Resource.Get()]; break;
                    case TextureType::Normal: mat.normalIdx = textureSrvIndices[tex.Resource.Get()]; break;
                    case TextureType::ORM: mat.ormIdx = textureSrvIndices[tex.Resource.Get()]; break;
                    case TextureType::Emissive: mat.emissiveIdx = textureSrvIndices[tex.Resource.Get()]; break;
                    }
                }

                mesh.materialID = static_cast<UINT>(globalMaterials.size());
                globalMaterials.push_back(mat);
            }
        }

        UINT bufferSize = static_cast<UINT>(globalMaterials.size() * sizeof(MaterialData));

        auto heapPropsDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
        dc->GetDevice()->CreateCommittedResource(&heapPropsDefault, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_materialBuffer));

        auto heapPropsUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        dc->GetDevice()->CreateCommittedResource(&heapPropsUpload, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_materialUploadBuffer));

        D3D12_SUBRESOURCE_DATA subData = {};
        subData.pData = globalMaterials.data();
        subData.RowPitch = bufferSize;
        subData.SlicePitch = subData.RowPitch;

        auto transition1 = CD3DX12_RESOURCE_BARRIER::Transition(m_materialBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->ResourceBarrier(1, &transition1);

        UpdateSubresources(cmdList, m_materialBuffer.Get(), m_materialUploadBuffer.Get(), 0, 0, 1, &subData);

        auto transition2 = CD3DX12_RESOURCE_BARRIER::Transition(m_materialBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &transition2);

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList };
        dc->GetCommandQueue()->ExecuteCommandLists(1, lists);

        int frameIndex = dc->GetSwapChain()->GetCurrentBackBufferIndex();
        dc->GetFenceValue(frameIndex)++;
        dc->GetCommandQueue()->Signal(dc->GetFence(frameIndex), dc->GetFenceValue(frameIndex));

        if (dc->GetFence(frameIndex)->GetCompletedValue() < dc->GetFenceValue(frameIndex))
        {
            dc->GetFence(frameIndex)->SetEventOnCompletion(dc->GetFenceValue(frameIndex), dc->GetFenceEvent());
            WaitForSingleObject(dc->GetFenceEvent(), INFINITE);
        }
    }

    // What is loaded :
    // 1. A cubemap for drawing the sky box
    // 2. An irradiance map for diffuse
    // 3. A filtered environment map for specular
    // 4. A BRDF LUT for specular
    bool InitIBL(RenderDevice* dc, const char* currentHDRPath)
    {
        int w, h, c;
        float* data = stbi_loadf(currentHDRPath, &w, &h, &c, 4);

        if (!data)
        {
            return false;
        }

        IBLBaker baker;
        baker.Bake(dc->GetDevice(), dc->GetCommandQueue(), data, w, h);

        texEnvCube = baker.GetEnvCube();
        texPrefilterCube = baker.GetPrefilterCube();
        texBRDFLUT = baker.GetBRDFLUT();
        shBuffer = baker.GetSHBuffer();

        if (!texEnvCube || !texPrefilterCube || !texBRDFLUT || !shBuffer)
        {
            stbi_image_free(data);
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC envD = {};
        envD.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        envD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        envD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        envD.TextureCube.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE hEnv(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), iblEnvCubeIdx, srvDescriptorSize);
        dc->GetDevice()->CreateShaderResourceView(texEnvCube.Get(), &envD, hEnv);

        D3D12_SHADER_RESOURCE_VIEW_DESC csD = {};
        csD.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        csD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        csD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        csD.TextureCube.MipLevels = 5;
        CD3DX12_CPU_DESCRIPTOR_HANDLE hPre(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), iblPrefilterIdx, srvDescriptorSize);
        dc->GetDevice()->CreateShaderResourceView(texPrefilterCube.Get(), &csD, hPre);

        D3D12_SHADER_RESOURCE_VIEW_DESC lsD = {};
        lsD.Format = DXGI_FORMAT_R16G16_FLOAT;
        lsD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lsD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lsD.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE hBrdf(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), iblBRDFIdx, srvDescriptorSize);
        dc->GetDevice()->CreateShaderResourceView(texBRDFLUT.Get(), &lsD, hBrdf);

        stbi_image_free(data);

        return true;
    }

    // Create a proxy texture and request memory from the Default Heap
    // Transition the resource state to COPY_DEST to prevent read hazards
    // Calculate the size of the upload container
    // Create a staging resource in the Upload Heap to facilitate access for both CPU and GPU
    // Transfer data from the Upload Heap to the GPU's Default Heap
    // Transition the proxy texture's Default Heap state from COPY_DEST to SHADER_RESOURCE
    void CreateDummyTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const unsigned char* colorData, ComPtr<ID3D12Resource>& outTex, ComPtr<ID3D12Resource>& outUpload)
    {
        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
        auto defHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex));

        UINT64 uploadSize = 0;
        device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

        auto upHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outUpload));

        D3D12_SUBRESOURCE_DATA subData = {};
        subData.pData = colorData;
        subData.RowPitch = 4;
        subData.SlicePitch = 4;
        UpdateSubresources(cmdList, outTex.Get(), outUpload.Get(), 0, 0, 1, &subData);

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
    }

    bool InitShadowResources(RenderDevice* dc)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = NUM_CASCADES;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(dc->GetDevice()->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_shadowDsvHeap))))
        {
            return false;
        }

        m_shadowDsvDescriptorSize = dc->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_TYPELESS,
            m_shadowMapSize, m_shadowMapSize,
            NUM_CASCADES, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
        );

        D3D12_CLEAR_VALUE optClear = {};
        optClear.Format = DXGI_FORMAT_D32_FLOAT;
        optClear.DepthStencil.Depth = 1.0f;
        optClear.DepthStencil.Stencil = 0;

        auto defHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        if (FAILED(dc->GetDevice()->CreateCommittedResource(
            &defHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &optClear,
            IID_PPV_ARGS(&m_shadowMap))))
        {
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.ArraySize = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT i = 0; i < NUM_CASCADES; ++i)
        {
            dsvDesc.Texture2DArray.FirstArraySlice = i;
            dc->GetDevice()->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, dsvHandle);
            dsvHandle.Offset(1, m_shadowDsvDescriptorSize);
        }

        m_shadowSrvIdx = srvIdx++;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = NUM_CASCADES;

        CD3DX12_CPU_DESCRIPTOR_HANDLE hSrv(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_shadowSrvIdx, srvDescriptorSize);
        dc->GetDevice()->CreateShaderResourceView(m_shadowMap.Get(), &srvDesc, hSrv);

        return true;
    }

    bool InitPostProcess(
        RenderDevice* dc,
        int sceneWidth,
        int sceneHeight,
        int outputWidth,
        int outputHeight)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dc->GetDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_postRtvHeap))))
        {
            return false;
        }

        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            (UINT64)sceneWidth, (UINT)sceneHeight,
            1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
        );

        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        clearVal.Color[0] = 0.2f; clearVal.Color[1] = 0.3f;
        clearVal.Color[2] = 0.4f; clearVal.Color[3] = 1.0f;

        auto defHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        if (FAILED(dc->GetDevice()->CreateCommittedResource(
            &defHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearVal,
            IID_PPV_ARGS(&m_offscreenRT))))
        {
            return false;
        }

        dc->GetDevice()->CreateRenderTargetView(m_offscreenRT.Get(), nullptr, m_postRtvHeap->GetCPUDescriptorHandleForHeapStart());

        m_offscreenSrvIdx = srvIdx++;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE hSrv(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_offscreenSrvIdx, srvDescriptorSize);
        dc->GetDevice()->CreateShaderResourceView(m_offscreenRT.Get(), &srvDesc, hSrv);

        D3D12_RESOURCE_DESC transparentCopyDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            (UINT64)sceneWidth, (UINT)sceneHeight,
            1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_NONE);

        if (FAILED(dc->GetDevice()->CreateCommittedResource(
            &defHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &transparentCopyDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&m_transparentSceneColorCopy))))
        {
            return false;
        }

        m_transparentSceneColorSrvIdx = srvIdx++;
        CD3DX12_CPU_DESCRIPTOR_HANDLE hTransparentCopySrv(
            mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            m_transparentSceneColorSrvIdx,
            srvDescriptorSize);

        dc->GetDevice()->CreateShaderResourceView(m_transparentSceneColorCopy.Get(), &srvDesc, hTransparentCopySrv);

        D3D12_DESCRIPTOR_HEAP_DESC taaRtvHeapDesc = {};
        taaRtvHeapDesc.NumDescriptors = 2;
        taaRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        dc->GetDevice()->CreateDescriptorHeap(&taaRtvHeapDesc, IID_PPV_ARGS(&m_taaRtvHeap));
        m_rtvDescriptorSize = dc->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_RESOURCE_DESC taaDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, (UINT64)outputWidth, (UINT)outputHeight,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_CLEAR_VALUE taaClearVal = {};
        taaClearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        auto defHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        for (int i = 0; i < 2; ++i)
        {
            dc->GetDevice()->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &taaDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &taaClearVal, IID_PPV_ARGS(&m_taaHistoryRT[i]));

            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_taaRtvHeap->GetCPUDescriptorHandleForHeapStart(), i, m_rtvDescriptorSize);
            dc->GetDevice()->CreateRenderTargetView(m_taaHistoryRT[i].Get(), nullptr, rtvHandle);

            m_taaHistorySrvIdx[i] = srvIdx++;
            CD3DX12_CPU_DESCRIPTOR_HANDLE hSrv(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_taaHistorySrvIdx[i], srvDescriptorSize);
            dc->GetDevice()->CreateShaderResourceView(m_taaHistoryRT[i].Get(), nullptr, hSrv);
        }

        D3D12_RESOURCE_DESC hbaoHistoryDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16_FLOAT, (UINT64)sceneWidth, (UINT)sceneHeight,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_CLEAR_VALUE hbaoHistoryClearValue = {};
        hbaoHistoryClearValue.Format = DXGI_FORMAT_R16_FLOAT;
        hbaoHistoryClearValue.Color[0] = 1.0f;

        for (int i = 0; i < 2; ++i)
        {
            if (FAILED(dc->GetDevice()->CreateCommittedResource(
                &defHeap,
                D3D12_HEAP_FLAG_NONE,
                &hbaoHistoryDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &hbaoHistoryClearValue,
                IID_PPV_ARGS(&m_hbaoHistoryRT[i]))))
            {
                return false;
            }
        }

        D3D12_RESOURCE_DESC hbaoDepthHistoryDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_TYPELESS, (UINT64)sceneWidth, (UINT)sceneHeight,
            1, 1);
        D3D12_RESOURCE_DESC hbaoNormalHistoryDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, (UINT64)sceneWidth, (UINT)sceneHeight,
            1, 1);

        for (int i = 0; i < 2; ++i)
        {
            if (FAILED(dc->GetDevice()->CreateCommittedResource(
                &defHeap,
                D3D12_HEAP_FLAG_NONE,
                &hbaoDepthHistoryDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr,
                IID_PPV_ARGS(&m_hbaoDepthHistoryRT[i]))))
            {
                return false;
            }

            if (FAILED(dc->GetDevice()->CreateCommittedResource(
                &defHeap,
                D3D12_HEAP_FLAG_NONE,
                &hbaoNormalHistoryDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr,
                IID_PPV_ARGS(&m_hbaoNormalHistoryRT[i]))))
            {
                return false;
            }
        }

        return true;
    }

    bool InitDepthBufferSRV(RenderDevice* dc)
    {
        if (dc == nullptr ||
            dc->GetDevice() == nullptr ||
            dc->GetDepthStencilBuffer() == nullptr ||
            !mainDescriptorHeap)
        {
            return false;
        }

        m_depthBufferSrvIdx = srvIdx++;

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE depthSrvHandle(
            mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            m_depthBufferSrvIdx,
            srvDescriptorSize);

        dc->GetDevice()->CreateShaderResourceView(
            dc->GetDepthStencilBuffer(),
            &depthSrvDesc,
            depthSrvHandle);

        return true;
    }

    void FreeUploadHeaps()
    {
        for (auto& pair : myModels)
        {
            if (pair.second != nullptr)
            {
                pair.second->FreeUploadHeaps();
            }
        }
    }

    int GetTAACurrentHistoryIdx()
    {
        return m_taaCurrentHistoryIdx;
    }

    void FlipTAAHistoryIndex()
    {
        m_taaCurrentHistoryIdx = 1 - m_taaCurrentHistoryIdx;
    }

    int GetHBAOCurrentHistoryIdx()
    {
        return m_hbaoCurrentHistoryIdx;
    }

    void FlipHBAOHistoryIndex()
    {
        m_hbaoCurrentHistoryIdx = 1 - m_hbaoCurrentHistoryIdx;
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetSHBufferGPUAddress()
    {
        return shBuffer->GetGPUVirtualAddress();
    }

    ID3D12DescriptorHeap* GetMainDescriptorHeap()
    {
        return mainDescriptorHeap.Get();
    }

    UINT AllocateSrvUavDescriptor()
    {
        return srvIdx++;
    }

    void SealPersistentSrvUavDescriptors()
    {
        persistentSrvUavDescriptorCount = srvIdx;
    }

    void ResetTransientSrvUavDescriptors(int frameIndex)
    {
        if (persistentSrvUavDescriptorCount == 0)
        {
            persistentSrvUavDescriptorCount = srvIdx;
        }

        const UINT transientCapacity = MaxSrvUavDescriptors - persistentSrvUavDescriptorCount;
        const UINT transientCapacityPerFrame = transientCapacity / m_srvFrameBufferCount;
        const UINT frameSlot = frameIndex >= 0 ? static_cast<UINT>(frameIndex) % m_srvFrameBufferCount : 0;

        srvIdx = persistentSrvUavDescriptorCount + frameSlot * transientCapacityPerFrame;
        transientSrvUavDescriptorEnd = (frameSlot + 1 == m_srvFrameBufferCount)
            ? MaxSrvUavDescriptors
            : srvIdx + transientCapacityPerFrame;
    }

    UINT AllocateTransientSrvUavDescriptor()
    {
        if (srvIdx >= transientSrvUavDescriptorEnd)
        {
            return UINT_MAX;
        }

        return srvIdx++;
    }

    bool AllocateTransientSrvUavDescriptor(
        UINT* outDescriptorIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle)
    {
        if (outDescriptorIndex == nullptr || outCpuHandle == nullptr)
        {
            return false;
        }

        const UINT descriptorIndex = AllocateTransientSrvUavDescriptor();
        if (descriptorIndex == UINT_MAX)
        {
            return false;
        }

        *outDescriptorIndex = descriptorIndex;
        *outCpuHandle = GetSrvUavCPUHandle(descriptorIndex);
        return true;
    }

    bool AllocateRDGTransientResource(
        RenderDevice* dc,
        int frameIndex,
        const D3D12_RESOURCE_DESC& resourceDesc,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_STATES finalState,
        const D3D12_CLEAR_VALUE* clearValue,
        RDGTransientResourceLease* outResource)
    {
        if (dc == nullptr ||
            dc->GetDevice() == nullptr ||
            outResource == nullptr ||
            frameIndex < 0 ||
            frameIndex >= static_cast<int>(m_rdgTransientResourcePools.size()))
        {
            return false;
        }

        outResource->reset();

        std::vector<std::shared_ptr<RDGTransientResourcePoolEntry>>& framePool =
            m_rdgTransientResourcePools[frameIndex];

        // Matching final-to-initial state keeps RDG's state model aligned with the reused resource.
        for (const std::shared_ptr<RDGTransientResourcePoolEntry>& entryLease : framePool)
        {
            // The pool owns one reference. Any additional reference means an RDG graph
            // (or another lease holder) still owns this resource.
            if (!entryLease ||
                entryLease.use_count() != 1)
            {
                continue;
            }

            RDGTransientResourcePoolEntry& entry = *entryLease;
            if (entry.usedThisFrame ||
                !entry.reusable ||
                !entry.resource ||
                entry.finalState != initialState ||
                !AreRDGResourceDescsEqual(entry.desc, resourceDesc) ||
                !AreRDGClearValuesEqual(entry, resourceDesc, clearValue))
            {
                continue;
            }

            entry.usedThisFrame = true;
            entry.finalState = finalState;
            entry.idleSinceFrame = InvalidRDGFrame;
            entry.lastUseFenceValue = dc->GetFenceValue(frameIndex) + 1;
            *outResource = entryLease;
            return true;
        }

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> resource;

        const HRESULT hr = dc->GetDevice()->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            initialState,
            clearValue,
            IID_PPV_ARGS(&resource));

        if (FAILED(hr))
        {
            return false;
        }

        std::shared_ptr<RDGTransientResourcePoolEntry> entry =
            std::make_shared<RDGTransientResourcePoolEntry>();
        entry->desc = resourceDesc;
        entry->hasClearValue = clearValue != nullptr;
        if (clearValue != nullptr)
        {
            entry->clearValue = *clearValue;
        }
        entry->resource = resource;
        entry->finalState = finalState;
        entry->usedThisFrame = true;
        entry->lastUseFenceValue = dc->GetFenceValue(frameIndex) + 1;

        framePool.push_back(entry);
        *outResource = entry;
        return true;
    }

    void BeginRDGFrame(RenderDevice* dc, int frameIndex)
    {
        if (dc == nullptr ||
            frameIndex < 0 ||
            frameIndex >= static_cast<int>(m_rdgTransientResourcePools.size()))
        {
            return;
        }

        ID3D12Fence* frameFence = dc->GetFence(frameIndex);
        if (frameFence == nullptr ||
            frameFence->GetCompletedValue() < dc->GetFenceValue(frameIndex))
        {
            return;
        }

        ++m_rdgFrameSerial;

        std::vector<std::shared_ptr<RDGTransientResourcePoolEntry>>& framePool =
            m_rdgTransientResourcePools[frameIndex];
        const UINT64 completedFenceValue = frameFence->GetCompletedValue();

        for (auto iterator = framePool.begin(); iterator != framePool.end();)
        {
            const std::shared_ptr<RDGTransientResourcePoolEntry>& entryLease = *iterator;
            if (!entryLease)
            {
                iterator = framePool.erase(iterator);
                continue;
            }

            RDGTransientResourcePoolEntry& entry = *entryLease;
            if (entryLease.use_count() == 1)
            {
                // Arm the keep-alive countdown once the pool becomes the sole owner.
                if (entry.idleSinceFrame == InvalidRDGFrame)
                {
                    entry.idleSinceFrame = m_rdgFrameSerial;
                }

                const bool idleExpired =
                    m_rdgFrameSerial - entry.idleSinceFrame >= RDGTransientResourceKeepAliveFrames;
                const bool gpuFinished =
                    completedFenceValue >= entry.lastUseFenceValue;

                if (idleExpired && gpuFinished)
                {
                    iterator = framePool.erase(iterator);
                    continue;
                }
            }
            else
            {
                entry.idleSinceFrame = InvalidRDGFrame;
            }

            // BeginFrame reaches this point only after this frame slot's fence has completed.
            entry.usedThisFrame = false;
            ++iterator;
        }
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetSrvUavCPUHandle(UINT descriptorIndex)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(
            mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            descriptorIndex,
            srvDescriptorSize);
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GetSrvUavGPUHandle(UINT descriptorIndex)
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(
            mainDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
            descriptorIndex,
            srvDescriptorSize);
    }

    std::vector<ModelInstance>& GetSceneInstances()
    {
        return m_sceneInstances;
    }

    D3D12_VERTEX_BUFFER_VIEW GetSkyboxVBV()
    {
        return skyboxVBV;
    }

    UINT8* GetCBVAddress(int frameIndex)
    {
        return cbvGPUAddress[frameIndex];
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetCBVGPUAddress(int frameIndex)
    {
        return constantBufferUploadHeap[frameIndex]->GetGPUVirtualAddress();
    }

    UINT GetSrvDescriptorSize()
    {
        return srvDescriptorSize;
    }

    UINT GetIblPrefilterIdx()
    {
        return iblPrefilterIdx;
    }

    UINT GetIblBRDFIdx()
    {
        return iblBRDFIdx;
    }

    UINT GetIblEnvCubeIdx()
    {
        return iblEnvCubeIdx;
    }

    UINT GetDummyAlbedoIdx()
    {
        return dummyAlbedoIdx;
    }

    UINT GetDummyNormalIdx()
    {
        return dummyNormalIdx;
    }

    UINT GetDummyORMIdx()
    {
        return dummyORMIdx;
    }

    UINT GetDummyEmissiveIdx()
    {
        return dummyEmissiveIdx;
    }

    UINT GetTextureSrvIdx(ID3D12Resource* tex)
    {
        return textureSrvIndices[tex];
    }

    ID3D12Resource* GetShadowMap()
    {
        return m_shadowMap.Get();
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetShadowDsvHandle(UINT cascadeIndex = 0)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart(), cascadeIndex, m_shadowDsvDescriptorSize);
    }

    UINT GetShadowSrvIdx()
    {
        return m_shadowSrvIdx;
    }

    ID3D12Resource* GetPostProcessRT()
    {
        return m_offscreenRT.Get();
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetPostProcessRtvHandle()
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_postRtvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    UINT GetPostProcessSrvIdx()
    {
        return m_offscreenSrvIdx;
    }

    ID3D12Resource* GetTransparentSceneColorCopy()
    {
        return m_transparentSceneColorCopy.Get();
    }

    UINT GetTransparentSceneColorSrvIdx()
    {
        return m_transparentSceneColorSrvIdx;
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetMaterialBufferGPUAddress()
    {
        return m_materialBuffer->GetGPUVirtualAddress();
    }

    UINT GetDepthBufferSrvIdx()
    {
        return m_depthBufferSrvIdx;
    }

    ID3D12Resource* GetTAAHistoryRT(int idx)
    {
        return m_taaHistoryRT[idx].Get();
    }

    UINT GetTAAHistorySrvIdx(int idx)
    {
        return m_taaHistorySrvIdx[idx];
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetTAARtvHandle(int idx)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_taaRtvHeap->GetCPUDescriptorHandleForHeapStart(), idx, m_rtvDescriptorSize);
    }

    ID3D12Resource* GetHBAOHistoryRT(int idx)
    {
        return m_hbaoHistoryRT[idx].Get();
    }

    ID3D12Resource* GetHBAODepthHistoryRT(int idx)
    {
        return m_hbaoDepthHistoryRT[idx].Get();
    }

    ID3D12Resource* GetHBAONormalHistoryRT(int idx)
    {
        return m_hbaoNormalHistoryRT[idx].Get();
    }

private:
    struct RDGTransientResourcePoolEntry : public RDGTransientResourceLeaseState
    {
        D3D12_RESOURCE_DESC desc = {};
        bool hasClearValue = false;
        D3D12_CLEAR_VALUE clearValue = {};
        D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
        bool usedThisFrame = false;
        uint64_t idleSinceFrame = UINT64_MAX;
        UINT64 lastUseFenceValue = 0;
    };

    static bool AreRDGResourceDescsEqual(
        const D3D12_RESOURCE_DESC& lhs,
        const D3D12_RESOURCE_DESC& rhs)
    {
        return lhs.Dimension == rhs.Dimension &&
            lhs.Alignment == rhs.Alignment &&
            lhs.Width == rhs.Width &&
            lhs.Height == rhs.Height &&
            lhs.DepthOrArraySize == rhs.DepthOrArraySize &&
            lhs.MipLevels == rhs.MipLevels &&
            lhs.Format == rhs.Format &&
            lhs.SampleDesc.Count == rhs.SampleDesc.Count &&
            lhs.SampleDesc.Quality == rhs.SampleDesc.Quality &&
            lhs.Layout == rhs.Layout &&
            lhs.Flags == rhs.Flags;
    }

    static bool AreRDGClearValuesEqual(
        const RDGTransientResourcePoolEntry& entry,
        const D3D12_RESOURCE_DESC& resourceDesc,
        const D3D12_CLEAR_VALUE* clearValue)
    {
        const bool hasClearValue = clearValue != nullptr;
        if (entry.hasClearValue != hasClearValue)
        {
            return false;
        }

        if (!hasClearValue)
        {
            return true;
        }

        if (entry.clearValue.Format != clearValue->Format)
        {
            return false;
        }

        if ((resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
        {
            return entry.clearValue.DepthStencil.Depth == clearValue->DepthStencil.Depth &&
                entry.clearValue.DepthStencil.Stencil == clearValue->DepthStencil.Stencil;
        }

        for (UINT componentIndex = 0; componentIndex < 4; ++componentIndex)
        {
            if (entry.clearValue.Color[componentIndex] != clearValue->Color[componentIndex])
            {
                return false;
            }
        }

        return true;
    }

    static constexpr UINT MaxSrvUavDescriptors = 1024;
    static constexpr uint64_t InvalidRDGFrame = UINT64_MAX;
    static constexpr uint64_t RDGTransientResourceKeepAliveFrames = 120;
    UINT persistentSrvUavDescriptorCount = 0;
    UINT transientSrvUavDescriptorEnd = MaxSrvUavDescriptors;
    UINT m_srvFrameBufferCount = 1;
    UINT srvIdx;
    UINT srvDescriptorSize;
    uint64_t m_rdgFrameSerial = 0;
    std::vector<std::vector<std::shared_ptr<RDGTransientResourcePoolEntry>>> m_rdgTransientResourcePools;

    ComPtr<ID3D12DescriptorHeap> mainDescriptorHeap;

    // Rendering assets and repository
    // A map for storing model data
    std::unordered_map<std::string, std::unique_ptr<Model>> myModels;
    std::vector<ModelInstance> m_sceneInstances;
    std::map<ID3D12Resource*, UINT> textureSrvIndices;

    UINT iblPrefilterIdx;
    UINT iblBRDFIdx;
    UINT iblEnvCubeIdx;

    ComPtr<ID3D12Resource> texEnvCube;
    ComPtr<ID3D12Resource> texPrefilterCube;
    ComPtr<ID3D12Resource> texBRDFLUT;

    ComPtr<ID3D12Resource> dummyAlbedo;
    UINT dummyAlbedoIdx;

    ComPtr<ID3D12Resource> dummyNormal;
    UINT dummyNormalIdx;

    ComPtr<ID3D12Resource> dummyORM;
    UINT dummyORMIdx;

    ComPtr<ID3D12Resource> dummyEmissive;
    UINT dummyEmissiveIdx;

    std::vector<ComPtr<ID3D12Resource>> constantBufferUploadHeap;
    std::vector<UINT8*> cbvGPUAddress;

    ComPtr<ID3D12Resource> skyboxVB;
    D3D12_VERTEX_BUFFER_VIEW skyboxVBV;

    ComPtr<ID3D12Resource> shBuffer;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    UINT m_shadowSrvIdx;
    UINT m_shadowDsvDescriptorSize = 0;
    const UINT m_shadowMapSize = 4096;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_offscreenRT;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_postRtvHeap;
    UINT m_offscreenSrvIdx;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_transparentSceneColorCopy;
    UINT m_transparentSceneColorSrvIdx = 0;

    ComPtr<ID3D12Resource> m_materialBuffer;
    ComPtr<ID3D12Resource> m_materialUploadBuffer;

    UINT m_rtvDescriptorSize = 0;

    UINT m_depthBufferSrvIdx = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_taaHistoryRT[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_taaRtvHeap;
    UINT m_taaHistorySrvIdx[2];
    int m_taaCurrentHistoryIdx = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_hbaoHistoryRT[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hbaoDepthHistoryRT[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hbaoNormalHistoryRT[2];
    int m_hbaoCurrentHistoryIdx = 0;
};

#endif
