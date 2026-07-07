#ifndef D3D12_MODEL_H
#define D3D12_MODEL_H

#include "stdafx.h"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

#include <wrl/client.h>
#include <ResourceUploadBatch.h>
#include <WICTextureLoader.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/pbrmaterial.h>
#include <string>
#include <vector>
#include <fstream>
#include <DirectXCollision.h>
#include <algorithm>
#include <cfloat>

#include "meshoptimizer.h"

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

enum class TextureType
{
    Albedo,
    Normal,
    ORM,
    Emissive,
    Unknown
};

struct Texture
{
    ComPtr<ID3D12Resource> Resource;
    TextureType type;
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
    std::vector<Vertex> vertices;
    std::vector<std::vector<unsigned int>> lodIndices;
    std::vector<Texture> textures;
    bool isUnlit;
    UINT materialID = 0;

    ComPtr<ID3D12Resource> vertexBufferUploader;
    std::vector<ComPtr<ID3D12Resource>> indexBufferUploaders;

    Mesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, std::vector<Vertex>& vertices, std::vector<std::vector<unsigned int>>& lodIndices, std::vector<Texture>& textures, bool _isUnlit = false)
    {
        this->vertices = std::move(vertices);
        this->lodIndices = std::move(lodIndices);
        this->textures = std::move(textures);
        this->isUnlit = _isUnlit;
        setupMesh(device, cmdList);
    }

    void Draw(ID3D12GraphicsCommandList* cmdList, UINT instanceCount = 1, int lodLevel = 0)
    {
        if (lodLevel >= indexBufferViews.size())
        {
            lodLevel = indexBufferViews.size() - 1;
        }

        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
        cmdList->IASetIndexBuffer(&indexBufferViews[lodLevel]);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawIndexedInstanced(static_cast<UINT>(lodIndices[lodLevel].size()), instanceCount, 0, 0, 0);
    }

private:
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    std::vector<ComPtr<ID3D12Resource>> indexBuffers;
    std::vector<D3D12_INDEX_BUFFER_VIEW> indexBufferViews;

    void setupMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
    {
        UINT vertexBufferSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));

        vertexBuffer = CreateDefaultBuffer(device, cmdList, vertices.data(), vertexBufferSize, vertexBufferUploader);
        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.StrideInBytes = sizeof(Vertex);
        vertexBufferView.SizeInBytes = vertexBufferSize;

        for (size_t i = 0; i < lodIndices.size(); ++i)
        {
            UINT indexBufferSize = static_cast<UINT>(lodIndices[i].size() * sizeof(unsigned int));
            ComPtr<ID3D12Resource> ibUploader;
            ComPtr<ID3D12Resource> iBuffer = CreateDefaultBuffer(device, cmdList, lodIndices[i].data(), indexBufferSize, ibUploader);

            D3D12_INDEX_BUFFER_VIEW ibView;
            ibView.BufferLocation = iBuffer->GetGPUVirtualAddress();
            ibView.Format = DXGI_FORMAT_R32_UINT;
            ibView.SizeInBytes = indexBufferSize;

            indexBuffers.push_back(iBuffer);
            indexBufferViews.push_back(ibView);
            indexBufferUploaders.push_back(ibUploader);
        }
    }

    ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const void* initData, UINT64 byteSize, ComPtr<ID3D12Resource>& uploadBuffer)
    {
        ComPtr<ID3D12Resource> defaultBuffer;
        auto heapPropsDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
        device->CreateCommittedResource(&heapPropsDefault, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&defaultBuffer));

        auto heapPropsUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        device->CreateCommittedResource(&heapPropsUpload, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));

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
    DirectX::BoundingBox boundingBox;

    Model(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, std::string const& path)
    {
        if (path.substr(path.find_last_of(".") + 1) != "glb")
        {
            return;
        }
        loadModel(device, cmdList, upload, path);
    }

    void Draw(ID3D12GraphicsCommandList* cmdList, int lodLevel = 0)
    {
        for (unsigned int i = 0; i < meshes.size(); i++)
        {
            meshes[i].Draw(cmdList, 1, lodLevel);
        }
    }

    void FreeUploadHeaps()
    {
        for (auto& mesh : meshes)
        {
            mesh.vertexBufferUploader.Reset();
            for (auto& uploader : mesh.indexBufferUploaders)
            {
                uploader.Reset();
            }
        }
    }

private:
    tinygltf::Model gltfModel;
    XMFLOAT3 minBoundary;
    XMFLOAT3 maxBoundary;

    void loadModel(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, std::string const& path)
    {
        std::vector<unsigned char> modelData = ReadFileToBuffer(path);
        if (modelData.empty())
        {
            return;
        }

        minBoundary = { FLT_MAX, FLT_MAX, FLT_MAX };
        maxBoundary = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

        tinygltf::TinyGLTF loader;
        std::string err, warn;
        loader.LoadBinaryFromMemory(&gltfModel, &err, &warn, modelData.data(), static_cast<unsigned int>(modelData.size()));

        Assimp::Importer importer;
        unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
            aiProcess_MakeLeftHanded | aiProcess_FlipWindingOrder | aiProcess_FlipUVs;

        const aiScene* scene = importer.ReadFileFromMemory(modelData.data(), modelData.size(), flags, path.c_str());
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            return;
        }

        processNode(device, cmdList, upload, scene->mRootNode, scene, XMMatrixIdentity());

        XMVECTOR vMin = XMLoadFloat3(&minBoundary);
        XMVECTOR vMax = XMLoadFloat3(&maxBoundary);

        XMVECTOR vCenter = XMVectorScale(XMVectorAdd(vMin, vMax), 0.5f);
        XMVECTOR vExtents = XMVectorScale(XMVectorSubtract(vMax, vMin), 0.5f);

        XMStoreFloat3(&boundingBox.Center, vCenter);
        XMStoreFloat3(&boundingBox.Extents, vExtents);
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
        bool isUnlit = false;

        DirectX::XMMATRIX invTranspose = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, nodeTransform));

        for (unsigned int i = 0; i < mesh->mNumVertices; i++)
        {
            Vertex vertex = {};
            XMVECTOR pos = XMVectorSet(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 1.0f);
            pos = XMVector4Transform(pos, nodeTransform);
            XMStoreFloat3(&vertex.Position, pos);

            minBoundary.x = (std::min)(minBoundary.x, vertex.Position.x);
            minBoundary.y = (std::min)(minBoundary.y, vertex.Position.y);
            minBoundary.z = (std::min)(minBoundary.z, vertex.Position.z);

            maxBoundary.x = (std::max)(maxBoundary.x, vertex.Position.x);
            maxBoundary.y = (std::max)(maxBoundary.y, vertex.Position.y);
            maxBoundary.z = (std::max)(maxBoundary.z, vertex.Position.z);

            if (mesh->HasNormals())
            {
                XMVECTOR norm = XMVectorSet(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z, 0.0f);
                norm = XMVector3Normalize(XMVector3TransformNormal(norm, invTranspose));
                XMStoreFloat3(&vertex.Normal, norm);
            }

            vertex.TexCoords = { 0.0f, 0.0f };
            vertex.Tangent = { 1.0f, 0.0f, 0.0f };
            vertex.Bitangent = { 0.0f, 1.0f, 0.0f };

            if (mesh->mTextureCoords[0])
            {
                vertex.TexCoords = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
            }

            if (mesh->HasTangentsAndBitangents())
            {
                XMVECTOR tan = XMVectorSet(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z, 0.0f);
                tan = XMVector3Normalize(XMVector3TransformNormal(tan, invTranspose));
                XMStoreFloat3(&vertex.Tangent, tan);

                XMVECTOR bitan = XMVectorSet(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z, 0.0f);
                bitan = XMVector3Normalize(XMVector3TransformNormal(bitan, invTranspose));
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

        std::vector<std::vector<unsigned int>> allLodIndices;
        allLodIndices.push_back(indices);

        size_t target_count_lod1 = size_t(indices.size() * 0.5f);
        std::vector<unsigned int> indices_lod1(indices.size());
        size_t lod1_count = meshopt_simplify(indices_lod1.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(), sizeof(Vertex), target_count_lod1, 1e-2f);
        indices_lod1.resize(lod1_count);
        allLodIndices.push_back(indices_lod1);

        size_t target_count_lod2 = size_t(indices.size() * 0.1f);
        std::vector<unsigned int> indices_lod2(indices.size());
        size_t lod2_count = meshopt_simplify(indices_lod2.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(), sizeof(Vertex), target_count_lod2, 1e-2f);
        indices_lod2.resize(lod2_count);
        allLodIndices.push_back(indices_lod2);

        if (mesh->mMaterialIndex >= 0)
        {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

            if (material->GetTextureCount(aiTextureType_BASE_COLOR) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_BASE_COLOR, TextureType::Albedo, textures, scene);
            }
            else if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_DIFFUSE, TextureType::Albedo, textures, scene);
            }

            if (material->GetTextureCount(aiTextureType_NORMALS) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_NORMALS, TextureType::Normal, textures, scene);
            }
            else if (material->GetTextureCount(aiTextureType_NORMAL_CAMERA) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_NORMAL_CAMERA, TextureType::Normal, textures, scene);
            }

            if (material->GetTextureCount(aiTextureType_UNKNOWN) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_UNKNOWN, TextureType::ORM, textures, scene);
            }
            else if (material->GetTextureCount(aiTextureType_METALNESS) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_METALNESS, TextureType::ORM, textures, scene);
            }

            if (material->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_AMBIENT_OCCLUSION, TextureType::ORM, textures, scene);
            }
            else if (material->GetTextureCount(aiTextureType_LIGHTMAP) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_LIGHTMAP, TextureType::ORM, textures, scene);
            }

            aiString matName;
            material->Get(AI_MATKEY_NAME, matName);

            for (size_t k = 0; k < gltfModel.materials.size(); k++)
            {
                if (gltfModel.materials[k].name == matName.C_Str())
                {
                    isUnlit = gltfModel.materials[k].extensions.find("KHR_materials_unlit") != gltfModel.materials[k].extensions.end();
                    break;
                }
            }

            if (material->GetTextureCount(aiTextureType_EMISSION_COLOR) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_EMISSION_COLOR, TextureType::Emissive, textures, scene);
            }
            else if (material->GetTextureCount(aiTextureType_EMISSIVE) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_EMISSIVE, TextureType::Emissive, textures, scene);
            }
        }

        return Mesh(device, cmdList, vertices, allLodIndices, textures, isUnlit);
    }

    void LoadAssimpTexture(ID3D12Device* device, DirectX::ResourceUploadBatch& upload, aiMaterial* mat, aiTextureType type, TextureType typeEnum, std::vector<Texture>& textures, const aiScene* scene)
    {
        if (mat->GetTextureCount(type) > 0)
        {
            aiString str;
            mat->GetTexture(type, 0, &str);
            std::string path = str.C_Str();

            bool skip = false;
            for (unsigned int j = 0; j < textures_loaded.size(); j++)
            {
                if (textures_loaded[j].path == path)
                {
                    Texture cachedTexture = textures_loaded[j];
                    cachedTexture.type = typeEnum;
                    textures.push_back(cachedTexture);
                    skip = true;
                    break;
                }
            }

            if (!skip)
            {
                Texture texture;
                if (!path.empty() && path[0] == '*')
                {
                    int imageIndex = std::stoi(path.substr(1));
                    if (imageIndex >= 0 && (unsigned int)imageIndex < scene->mNumTextures)
                    {
                        const aiTexture* embeddedTex = scene->mTextures[imageIndex];
                        TextureFromMemory(device, upload, embeddedTex, texture);
                    }
                }

                if (texture.Resource != nullptr)
                {
                    texture.type = typeEnum;
                    texture.path = path;
                    textures.push_back(texture);
                    textures_loaded.push_back(texture);
                }
            }
        }
    }

    void TextureFromMemory(ID3D12Device* device, DirectX::ResourceUploadBatch& upload, const aiTexture* aiTex, Texture& outTex)
    {
        size_t dataSize = aiTex->mHeight == 0 ? aiTex->mWidth : aiTex->mWidth * aiTex->mHeight * 4;
        DirectX::CreateWICTextureFromMemory(device, upload, reinterpret_cast<const uint8_t*>(aiTex->pcData), dataSize, outTex.Resource.GetAddressOf(), true);
    }

    std::vector<unsigned char> ReadFileToBuffer(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            return {};
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<unsigned char> buffer(size);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        return buffer;
    }
};

#endif
