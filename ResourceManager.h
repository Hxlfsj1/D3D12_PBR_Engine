#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

#include <stb_image.h>
#include "stdafx.h"
#include "RenderDevice.h"
#include "PBR_Model.h"
#include "IBLBaker.h"
#include "Assets.h"

#include <map>
#include <vector>
#include <string>
#include <unordered_map>
#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>
#include <algorithm>

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
        for (auto& pair : myModels)
        {
            if (pair.second != nullptr)
            {
                delete pair.second;
                pair.second = nullptr;
            }
        }
        myModels.clear();
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
        srvHeapDesc.NumDescriptors = 1024;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = dc->GetDevice()->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mainDescriptorHeap));

        if (FAILED(hr))
        {
            return false;
        }

        srvDescriptorSize = dc->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        srvIdx = 0;

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
                myModels[desc.modelPath] = new Model(dc->GetDevice(), dc->GetCommandList(), resourceUpload, desc.modelPath);
            }

            ModelInstance instance;
            instance.name = desc.name;
            instance.pModel = myModels[desc.modelPath];
            instance.translation = desc.pos;
            instance.rotation = desc.rot;
            instance.scale = desc.scale;

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

        for (int i = 0; i < frameBufferCount; ++i)
        {
            auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto buf = CD3DX12_RESOURCE_DESC::Buffer(2048 * 2048);

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

        std::sort(m_sceneInstances.begin(), m_sceneInstances.end(), [](const ModelInstance& a, const ModelInstance& b)
        {
            return a.pModel < b.pModel;
        });

        return true;
    }

    // What is loaded :
    // 1. A cubemap for drawing the sky box
    // 2. An irradiance map for diffuse
    // 3. A filtered environment map for specular
    // 4. A BRDF LUT for specular
    void InitIBL(RenderDevice* dc, const char* currentHDRPath)
    {
        int w, h, c;
        float* data = stbi_loadf(currentHDRPath, &w, &h, &c, 4);

        if (!data)
        {
            return;
        }

        IBLBaker baker;
        baker.Bake(dc->GetDevice(), dc->GetCommandQueue(), data, w, h);

        texEnvCube = baker.GetEnvCube();
        texPrefilterCube = baker.GetPrefilterCube();
        texBRDFLUT = baker.GetBRDFLUT();

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

        shBuffer = baker.GetSHBuffer();
        stbi_image_free(data);
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
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(dc->GetDevice()->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_shadowDsvHeap)))) {
            return false;
        }

        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_TYPELESS,
            m_shadowMapSize, m_shadowMapSize,
            1, 1, 1, 0,
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
            IID_PPV_ARGS(&m_shadowMap)))) {
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        dc->GetDevice()->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart());

        m_shadowSrvIdx = srvIdx++;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE hSrv(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_shadowSrvIdx, srvDescriptorSize);
        dc->GetDevice()->CreateShaderResourceView(m_shadowMap.Get(), &srvDesc, hSrv);

        return true;
    }

    D3D12_GPU_VIRTUAL_ADDRESS GetSHBufferGPUAddress()
    {
        return shBuffer->GetGPUVirtualAddress();
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

    ID3D12DescriptorHeap* GetMainDescriptorHeap()
    {
        return mainDescriptorHeap.Get();
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

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetShadowDsvHandle()
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    UINT GetShadowSrvIdx()
    {
        return m_shadowSrvIdx;
    }

private:
    UINT srvIdx;
    UINT srvDescriptorSize;

    ComPtr<ID3D12DescriptorHeap> mainDescriptorHeap;

    // Rendering assets and repository
    // A map for storing model data
    std::unordered_map<std::string, Model*> myModels;
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
    const UINT m_shadowMapSize = 2048;
};

#endif