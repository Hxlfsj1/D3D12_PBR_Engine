// 1. Accepts GLB format only to ensure PBR parameter accuracy
// 2. Leveraging the Assimp library for data processing
// 3. Identify textures unrecognized by Assimp as ORM maps for shader processing
// 4. Static baking is applied to models (thus, only static models can be loaded)

#ifndef D3D12_MODEL_H
#define D3D12_MODEL_H

#include "stdafx.h"
#include <wrl/client.h>
#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/pbrmaterial.h>
#include <string>
#include <vector>

#define MAX_BONE_INFLUENCE 4

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct Vertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoords;
    XMFLOAT3 Tangent;
    XMFLOAT3 Bitangent;
    int m_BoneIDs[MAX_BONE_INFLUENCE];
    float m_Weights[MAX_BONE_INFLUENCE];
};

struct Texture
{
    ComPtr<ID3D12Resource> Resource;
    std::string type;
    std::string path;
};

inline XMMATRIX AssimpToDXMatrix(const aiMatrix4x4& aiMat)
{
    return XMMATRIX(
        aiMat.a1, aiMat.b1, aiMat.c1, aiMat.d1,
        aiMat.a2, aiMat.b2, aiMat.c2, aiMat.d2,
        aiMat.a3, aiMat.b3, aiMat.c3, aiMat.d3,
        aiMat.a4, aiMat.b4, aiMat.c4, aiMat.d4
    );
}

class Mesh
{
public:
    std::vector<Vertex>       vertices;
    std::vector<unsigned int> indices;
    std::vector<Texture>      textures;

    ComPtr<ID3D12Resource> vertexBufferUploader;
    ComPtr<ID3D12Resource> indexBufferUploader;

    Mesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, std::vector<Vertex>& vertices, std::vector<unsigned int>& indices, std::vector<Texture>& textures)
    {
        this->vertices = vertices;
        this->indices = indices;
        this->textures = textures;
        setupMesh(device, cmdList);
    }

    void Draw(ID3D12GraphicsCommandList* cmdList)
    {
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
        cmdList->IASetIndexBuffer(&indexBufferView);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawIndexedInstanced(static_cast<UINT>(indices.size()), 1, 0, 0, 0);
    }

private:
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;

    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;

    void setupMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
    {
        UINT vertexBufferSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        UINT indexBufferSize = static_cast<UINT>(indices.size() * sizeof(unsigned int));

        vertexBuffer = CreateDefaultBuffer(device, cmdList, vertices.data(), vertexBufferSize, vertexBufferUploader);

        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.StrideInBytes = sizeof(Vertex);
        vertexBufferView.SizeInBytes = vertexBufferSize;

        indexBuffer = CreateDefaultBuffer(device, cmdList, indices.data(), indexBufferSize, indexBufferUploader);

        indexBufferView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        indexBufferView.SizeInBytes = indexBufferSize;
    }

    ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const void* initData, UINT64 byteSize, ComPtr<ID3D12Resource>& uploadBuffer)
    {
        ComPtr<ID3D12Resource> defaultBuffer;

        auto heapPropsDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
        device->CreateCommittedResource(
            &heapPropsDefault, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&defaultBuffer));

        auto heapPropsUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        device->CreateCommittedResource(
            &heapPropsUpload, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

        D3D12_SUBRESOURCE_DATA subResourceData = {};
        subResourceData.pData = initData;
        subResourceData.RowPitch = byteSize;
        subResourceData.SlicePitch = subResourceData.RowPitch;

        auto barrierEnter = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->ResourceBarrier(1, &barrierEnter);

        UpdateSubresources<1>(cmdList, defaultBuffer.Get(), uploadBuffer.Get(), 0, 0, 1, &subResourceData);

        auto barrierExit = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmdList->ResourceBarrier(1, &barrierExit);

        return defaultBuffer;
    }
};

class Model
{
public:
    std::vector<Texture> textures_loaded;
    std::vector<Mesh> meshes;

    Model(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, std::string const& path)
    {
        if (path.substr(path.find_last_of(".") + 1) != "glb")
        {
            return;
        }
        loadModel(device, cmdList, upload, path);
    }

    void Draw(ID3D12GraphicsCommandList* cmdList)
    {
        for (unsigned int i = 0; i < meshes.size(); i++)
        {
            meshes[i].Draw(cmdList);
        }
    }

    void FreeUploadHeaps()
    {
        for (auto& mesh : meshes)
        {
            mesh.vertexBufferUploader.Reset();
            mesh.indexBufferUploader.Reset();
        }
    }

private:
    void loadModel(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, std::string const& path)
    {
        Assimp::Importer importer;
        unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
            aiProcess_MakeLeftHanded | aiProcess_FlipWindingOrder | aiProcess_FlipUVs;

        const aiScene* scene = importer.ReadFile(path, flags);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            return;
        }
        processNode(device, cmdList, upload, scene->mRootNode, scene, XMMatrixIdentity());
    }

    void processNode(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, aiNode* node, const aiScene* scene, XMMATRIX parentTransform)
    {
        XMMATRIX nodeTransform = AssimpToDXMatrix(node->mTransformation) * parentTransform;

        for (unsigned int i = 0; i < node->mNumMeshes; i++)
        {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            meshes.push_back(processMesh(device, cmdList, upload, mesh, scene, nodeTransform));
        }
        for (unsigned int i = 0; i < node->mNumChildren; i++)
        {
            processNode(device, cmdList, upload, node->mChildren[i], scene, nodeTransform);
        }
    }

    Mesh processMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, aiMesh* mesh, const aiScene* scene, XMMATRIX nodeTransform)
    {
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        std::vector<Texture> textures;

        for (unsigned int i = 0; i < mesh->mNumVertices; i++)
        {
            Vertex vertex = {};

            XMVECTOR pos = XMVectorSet(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 1.0f);
            pos = XMVector4Transform(pos, nodeTransform);
            XMStoreFloat3(&vertex.Position, pos);

            if (mesh->HasNormals())
            {
                XMVECTOR norm = XMVectorSet(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z, 0.0f);
                norm = XMVector3Normalize(XMVector4Transform(norm, nodeTransform));
                XMStoreFloat3(&vertex.Normal, norm);
            }

            if (mesh->mTextureCoords[0])
            {
                vertex.TexCoords = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };

                XMVECTOR tan = XMVectorSet(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z, 0.0f);
                tan = XMVector3Normalize(XMVector4Transform(tan, nodeTransform));
                XMStoreFloat3(&vertex.Tangent, tan);

                XMVECTOR bitan = XMVectorSet(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z, 0.0f);
                bitan = XMVector3Normalize(XMVector4Transform(bitan, nodeTransform));
                XMStoreFloat3(&vertex.Bitangent, bitan);
            }
            vertices.push_back(vertex);
        }

        for (unsigned int i = 0; i < mesh->mNumFaces; i++)
        {
            for (unsigned int j = 0; j < mesh->mFaces[i].mNumIndices; j++)
            {
                indices.push_back(mesh->mFaces[i].mIndices[j]);
            }
        }

        if (mesh->mMaterialIndex >= 0)
        {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

            loadMaterialTextures(device, upload, material, aiTextureType_DIFFUSE, "texture_diffuse", textures, scene);
            loadMaterialTextures(device, upload, material, aiTextureType_NORMALS, "texture_normal", textures, scene);

            loadMaterialTextures(device, upload, material, aiTextureType_UNKNOWN, "texture_metallicRoughness", textures, scene);
            loadMaterialTextures(device, upload, material, aiTextureType_METALNESS, "texture_metallicRoughness", textures, scene);
            loadMaterialTextures(device, upload, material, aiTextureType_DIFFUSE_ROUGHNESS, "texture_metallicRoughness", textures, scene);

            if (material->GetTextureCount(aiTextureType_SPECULAR) > 0)
            {
                loadMaterialTextures(device, upload, material, aiTextureType_SPECULAR, "texture_metallicRoughness", textures, scene);
            }

            loadMaterialTextures(device, upload, material, aiTextureType_LIGHTMAP, "texture_ao", textures, scene);
        }

        return Mesh(device, cmdList, vertices, indices, textures);
    }

    void loadMaterialTextures(ID3D12Device* device, DirectX::ResourceUploadBatch& upload, aiMaterial* mat, aiTextureType type, std::string typeName, std::vector<Texture>& textures, const aiScene* scene)
    {
        for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
        {
            aiString str;
            mat->GetTexture(type, i, &str);

            bool skip = false;
            for (unsigned int j = 0; j < textures_loaded.size(); j++)
            {
                if (std::strcmp(textures_loaded[j].path.data(), str.C_Str()) == 0)
                {
                    textures.push_back(textures_loaded[j]);
                    skip = true;
                    break;
                }
            }

            if (!skip)
            {
                Texture texture;
                const aiTexture* embeddedTex = scene->GetEmbeddedTexture(str.C_Str());
                if (embeddedTex)
                {
                    TextureFromMemory(device, upload, embeddedTex, texture);
                    texture.type = typeName;
                    texture.path = str.C_Str();
                    textures.push_back(texture);
                    textures_loaded.push_back(texture);
                }
            }
        }
    }

    void TextureFromMemory(ID3D12Device* device, DirectX::ResourceUploadBatch& upload, const aiTexture* aiTex, Texture& outTex)
    {
        size_t dataSize = aiTex->mHeight == 0 ? aiTex->mWidth : aiTex->mWidth * aiTex->mHeight * 4;
        DirectX::CreateWICTextureFromMemory(
            device,
            upload,
            reinterpret_cast<const uint8_t*>(aiTex->pcData),
            dataSize,
            outTex.Resource.GetAddressOf(),
            true
        );
    }
};
#endif