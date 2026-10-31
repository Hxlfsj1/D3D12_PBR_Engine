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
#include <stdexcept>
#include <charconv>
#include <string_view>
#include <cstring>

#include "meshoptimizer.h"
#include "MaterialTextureLoader.h"

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
    TextureType type = TextureType::Unknown;
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
    XMFLOAT4 baseColorFactor;
    XMFLOAT3 emissiveFactor = { 0, 0, 0 };
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.5f;
    bool isUnlit;
    UINT materialID = 0;

    ComPtr<ID3D12Resource> vertexBufferUploader;
    std::vector<ComPtr<ID3D12Resource>> indexBufferUploaders;

    Mesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, std::vector<Vertex>& vertices, std::vector<std::vector<unsigned int>>& lodIndices, std::vector<Texture>& textures, XMFLOAT4 _baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f }, bool _isUnlit = false, XMFLOAT3 _emissiveFactor = { 0, 0, 0 }, float _metallicFactor = 1.0f, float _roughnessFactor = 1.0f, float _occlusionStrength = 1.0f, float _alphaCutoff = 0.5f)
    {
        this->vertices = std::move(vertices);
        this->lodIndices = std::move(lodIndices);
        this->textures = std::move(textures);
        this->baseColorFactor = _baseColorFactor;
        this->isUnlit = _isUnlit;
        this->emissiveFactor = _emissiveFactor;
        this->metallicFactor = _metallicFactor;
        this->roughnessFactor = _roughnessFactor;
        this->occlusionStrength = _occlusionStrength;
        this->alphaCutoff = _alphaCutoff;
        setupMesh(device, cmdList);
    }

    void Draw(ID3D12GraphicsCommandList* cmdList, UINT instanceCount = 1, int lodLevel = 0)
    {
        if (indexBufferViews.empty()) return;
        lodLevel = std::clamp(lodLevel, 0, static_cast<int>(indexBufferViews.size()) - 1);

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
        if (vertices.empty() || vertices.size() > UINT_MAX / sizeof(Vertex) || lodIndices.empty())
            throw std::runtime_error("Empty or oversized mesh vertex data.");
        UINT vertexBufferSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));

        vertexBuffer = CreateDefaultBuffer(device, cmdList, vertices.data(), vertexBufferSize, vertexBufferUploader);
        if (vertexBuffer == nullptr)
        {
            ErrorLog::Write("Model: failed to create a mesh vertex buffer.");
        }
        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.StrideInBytes = sizeof(Vertex);
        vertexBufferView.SizeInBytes = vertexBufferSize;

        for (size_t i = 0; i < lodIndices.size(); ++i)
        {
            if (lodIndices[i].empty() || lodIndices[i].size() > UINT_MAX / sizeof(unsigned int))
                throw std::runtime_error("Empty or oversized mesh index data.");
            UINT indexBufferSize = static_cast<UINT>(lodIndices[i].size() * sizeof(unsigned int));
            ComPtr<ID3D12Resource> ibUploader;
            ComPtr<ID3D12Resource> iBuffer = CreateDefaultBuffer(device, cmdList, lodIndices[i].data(), indexBufferSize, ibUploader);
            if (iBuffer == nullptr)
            {
                ErrorLog::Write(
                    "Model: failed to create a mesh index buffer for LOD " +
                    std::to_string(i) + '.');
            }

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
        if (!initData || byteSize == 0) throw std::runtime_error("Mesh upload data is empty.");
        ComPtr<ID3D12Resource> defaultBuffer;
        auto heapPropsDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
        HRESULT hr = device->CreateCommittedResource(
            &heapPropsDefault,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&defaultBuffer));
        if (FAILED(hr))
        {
            ErrorLog::HRESULT("Model: failed to create the default mesh buffer.", hr);
            throw std::runtime_error("Mesh GPU buffer allocation failed.");
        }

        auto heapPropsUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        hr = device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadBuffer));
        if (FAILED(hr))
        {
            ErrorLog::HRESULT("Model: failed to create the mesh upload buffer.", hr);
            throw std::runtime_error("Mesh upload buffer allocation failed.");
        }

        D3D12_SUBRESOURCE_DATA subResourceData = {};
        subResourceData.pData = initData;
        subResourceData.RowPitch = byteSize;
        subResourceData.SlicePitch = subResourceData.RowPitch;

        auto barrierEnter = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->ResourceBarrier(1, &barrierEnter);
        if (UpdateSubresources<1>(cmdList, defaultBuffer.Get(), uploadBuffer.Get(), 0, 0, 1, &subResourceData) == 0)
        {
            ErrorLog::Write("Model: failed to upload mesh buffer data.");
            throw std::runtime_error("Mesh buffer upload failed.");
        }
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
    DirectX::BoundingBox boundingBox = {};
    bool IsLoaded() const { return m_loaded; }

    Model(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, std::string const& path)
    {
        if (path.substr(path.find_last_of(".") + 1) != "glb")
        {
            ErrorLog::Write("Model: unsupported model format: " + path);
            return;
        }
        try { loadModel(device, cmdList, upload, path); }
        catch (const std::exception& error)
        {
            meshes.clear();
            ErrorLog::Write("Model: loading failed for " + path + ": " + error.what());
        }
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
    bool m_loaded = false;
    inline static constexpr char MaterialTagPrefix[] = "__LearnDirectXMaterial_";
    tinygltf::Model gltfModel;
    XMFLOAT3 minBoundary;
    XMFLOAT3 maxBoundary;

    void loadModel(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DirectX::ResourceUploadBatch& upload, std::string const& path)
    {
        std::vector<unsigned char> modelData = ReadFileToBuffer(path);
        if (modelData.empty())
        {
            ErrorLog::Write("Model: model file is missing or empty: " + path);
            return;
        }

        minBoundary = { FLT_MAX, FLT_MAX, FLT_MAX };
        maxBoundary = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

        tinygltf::TinyGLTF loader;
        std::string err, warn;
        const bool gltfLoaded = loader.LoadBinaryFromMemory(
            &gltfModel,
            &err,
            &warn,
            modelData.data(),
            static_cast<unsigned int>(modelData.size()));
        if (!warn.empty())
        {
            ErrorLog::Write("Model: tinygltf warning for " + path + ": " + warn);
        }
        if (!gltfLoaded || !err.empty())
        {
            ErrorLog::Write("Model: tinygltf failed to load " + path + ": " + err);
            return;
        }

        // Assimp may reorder materials, while glTF names are neither required
        // nor unique. Tag the import-only JSON with stable source IDs and keep
        // the original tinygltf metadata for authored material values.
        TagAssimpMaterialNames(modelData);
        Assimp::Importer importer;
        unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
            aiProcess_MakeLeftHanded | aiProcess_FlipWindingOrder | aiProcess_FlipUVs;

        const aiScene* scene = importer.ReadFileFromMemory(modelData.data(), modelData.size(), flags, path.c_str());
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            ErrorLog::Write(
                "Model: Assimp failed to load " + path + ": " +
                importer.GetErrorString());
            return;
        }

        processNode(device, cmdList, upload, scene->mRootNode, scene, XMMatrixIdentity());
        if (meshes.empty())
        {
            ErrorLog::Write("Model: no triangle meshes were imported: " + path);
            return;
        }

        XMVECTOR vMin = XMLoadFloat3(&minBoundary);
        XMVECTOR vMax = XMLoadFloat3(&maxBoundary);

        XMVECTOR vCenter = XMVectorScale(XMVectorAdd(vMin, vMax), 0.5f);
        XMVECTOR vExtents = XMVectorScale(XMVectorSubtract(vMax, vMin), 0.5f);

        XMStoreFloat3(&boundingBox.Center, vCenter);
        XMStoreFloat3(&boundingBox.Extents, vExtents);
        m_loaded = true;
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
        XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
        bool isUnlit = false;
        XMFLOAT3 emissiveFactor = { 0, 0, 0 };
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float occlusionStrength = 1.0f;
        float alphaCutoff = 0.5f;
        if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
            throw std::runtime_error("Mesh contains no triangles.");

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
            // Missing UVs/tangents are valid for untextured meshes. Leave the
            // directions absent; the shared shader helper builds a safe basis
            // only if a material actually needs tangent-space normal mapping.
            vertex.Tangent = {};
            vertex.Bitangent = {};

            if (mesh->mTextureCoords[0])
            {
                vertex.TexCoords = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
            }

            if (mesh->HasTangentsAndBitangents())
            {
                XMVECTOR tan = XMVectorSet(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z, 0.0f);
                tan = XMVector3Normalize(XMVector3TransformNormal(tan, nodeTransform));
                XMStoreFloat3(&vertex.Tangent, tan);

                XMVECTOR bitan = XMVectorSet(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z, 0.0f);
                bitan = XMVector3Normalize(XMVector3TransformNormal(bitan, nodeTransform));
                XMStoreFloat3(&vertex.Bitangent, bitan);
            }
            vertices.push_back(vertex);
        }

        const bool mirroredNode = XMVectorGetX(XMMatrixDeterminant(nodeTransform)) < 0.0f;
        for (unsigned int i = 0; i < mesh->mNumFaces; i++)
        {
            const auto& face = mesh->mFaces[i];
            if (face.mNumIndices != 3) throw std::runtime_error("Only triangle mesh primitives are supported.");
            const unsigned int order[3] = {0, mirroredNode ? 2u : 1u, mirroredNode ? 1u : 2u};
            for (const auto vertexIndex : order)
            {
                const auto index = face.mIndices[vertexIndex];
                if (index >= vertices.size()) throw std::runtime_error("Mesh index is outside its vertex data.");
                indices.push_back(index);
            }
        }

        std::vector<std::vector<unsigned int>> allLodIndices;
        allLodIndices.push_back(indices);

        size_t target_count_lod1 = size_t(indices.size() * 0.5f);
        std::vector<unsigned int> indices_lod1(indices.size());
        size_t lod1_count = meshopt_simplify(indices_lod1.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(), sizeof(Vertex), target_count_lod1, 1e-2f);
        indices_lod1.resize(lod1_count);
        if (indices_lod1.empty()) indices_lod1 = indices;
        allLodIndices.push_back(indices_lod1);

        size_t target_count_lod2 = size_t(indices.size() * 0.1f);
        std::vector<unsigned int> indices_lod2(indices.size());
        size_t lod2_count = meshopt_simplify(indices_lod2.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(), sizeof(Vertex), target_count_lod2, 1e-2f);
        indices_lod2.resize(lod2_count);
        if (indices_lod2.empty()) indices_lod2 = indices_lod1;
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
                LoadAssimpTexture(device, upload, material, aiTextureType_UNKNOWN, TextureType::MetallicRoughness, textures, scene);
            }
            else if (material->GetTextureCount(aiTextureType_METALNESS) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_METALNESS, TextureType::MetallicRoughness, textures, scene);
            }

            if (material->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_AMBIENT_OCCLUSION, TextureType::Occlusion, textures, scene);
            }
            else if (material->GetTextureCount(aiTextureType_LIGHTMAP) > 0)
            {
                LoadAssimpTexture(device, upload, material, aiTextureType_LIGHTMAP, TextureType::Occlusion, textures, scene);
            }

            bool foundGltfMaterial = false;
            auto applyGltfMaterial = [&](const tinygltf::Material& gltfMaterial)
                {
                    // glTF factors multiply the texture channels; both default
                    // to one, including when the material has no MR texture.
                    metallicFactor = static_cast<float>(std::clamp(
                        gltfMaterial.pbrMetallicRoughness.metallicFactor, 0.0, 1.0));
                    roughnessFactor = static_cast<float>(std::clamp(
                        gltfMaterial.pbrMetallicRoughness.roughnessFactor, 0.0, 1.0));
                    occlusionStrength = static_cast<float>(std::clamp(gltfMaterial.occlusionTexture.strength, 0.0, 1.0));
                    alphaCutoff = static_cast<float>((std::max)(gltfMaterial.alphaCutoff, 0.0));
                    const std::vector<double>& factor = gltfMaterial.pbrMetallicRoughness.baseColorFactor;
                    if (factor.size() >= 4)
                    {
                        baseColorFactor = {
                            static_cast<float>(factor[0]),
                            static_cast<float>(factor[1]),
                            static_cast<float>(factor[2]),
                            static_cast<float>(factor[3])
                        };
                    }

                    double emissiveStrength = 1.0;
                    auto extension = gltfMaterial.extensions.find("KHR_materials_emissive_strength");
                    if (extension != gltfMaterial.extensions.end() && extension->second.Has("emissiveStrength"))
                        emissiveStrength = extension->second.Get("emissiveStrength").GetNumberAsDouble();
                    if (gltfMaterial.emissiveFactor.size() >= 3)
                        emissiveFactor = {
                            float(gltfMaterial.emissiveFactor[0] * emissiveStrength),
                            float(gltfMaterial.emissiveFactor[1] * emissiveStrength),
                            float(gltfMaterial.emissiveFactor[2] * emissiveStrength)
                        };

                    isUnlit = gltfMaterial.extensions.find("KHR_materials_unlit") != gltfMaterial.extensions.end();
                    foundGltfMaterial = true;
                };

            aiString importedName;
            material->Get(AI_MATKEY_NAME, importedName);
            std::string_view materialTag(importedName.C_Str(), importedName.length);
            if (materialTag.starts_with(MaterialTagPrefix))
            {
                materialTag.remove_prefix(sizeof(MaterialTagPrefix) - 1);
                size_t sourceIndex = 0;
                const auto parsed = std::from_chars(materialTag.data(), materialTag.data() + materialTag.size(), sourceIndex);
                if (parsed.ec != std::errc{} || parsed.ptr != materialTag.data() + materialTag.size() ||
                    sourceIndex >= gltfModel.materials.size())
                    throw std::runtime_error("Imported material source ID is invalid.");
                applyGltfMaterial(gltfModel.materials[sourceIndex]);
            }

            if (!foundGltfMaterial)
            {
                material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor);
                material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor);
                aiColor3D emission(0, 0, 0);
                if (material->Get(AI_MATKEY_COLOR_EMISSIVE, emission) == AI_SUCCESS)
                    emissiveFactor = { emission.r, emission.g, emission.b };
            }

            aiColor4D assimpBaseColor;
            if (!foundGltfMaterial && material->Get(AI_MATKEY_BASE_COLOR, assimpBaseColor) == AI_SUCCESS)
            {
                baseColorFactor.x = assimpBaseColor.r;
                baseColorFactor.y = assimpBaseColor.g;
                baseColorFactor.z = assimpBaseColor.b;
                baseColorFactor.w = assimpBaseColor.a;
            }

            float opacity = 1.0f;
            if (!foundGltfMaterial && material->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
            {
                baseColorFactor.w = opacity;
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

        return Mesh(device, cmdList, vertices, allLodIndices, textures, baseColorFactor, isUnlit, emissiveFactor,
            metallicFactor, roughnessFactor, occlusionStrength, alphaCutoff);
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
                // Color and data uses need different formats and mip chains.
                // Albedo/emissive can share; normal/MR/AO can share separately.
                if (textures_loaded[j].path == path &&
                    IsColorTexture(textures_loaded[j].type) == IsColorTexture(typeEnum))
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
                        TextureFromMemory(device, upload, embeddedTex, typeEnum, texture);
                    }
                    else
                    {
                        ErrorLog::Write(
                            "Model: embedded texture index is out of range: " + path);
                    }
                }
                else if (!path.empty())
                {
                    ErrorLog::Write(
                        "Model: external texture is not embedded and was not loaded: " + path);
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

    void TextureFromMemory(ID3D12Device* device, DirectX::ResourceUploadBatch& upload, const aiTexture* aiTex, TextureType type, Texture& outTex)
    {
        size_t dataSize = aiTex->mHeight == 0 ? aiTex->mWidth : aiTex->mWidth * aiTex->mHeight * 4;
        const HRESULT hr = CreateEmbeddedMaterialTexture(device, upload,
            reinterpret_cast<const uint8_t*>(aiTex->pcData), dataSize, type,
            outTex.Resource.GetAddressOf());
        if (FAILED(hr))
        {
            ErrorLog::HRESULT("Model: failed to decode an embedded texture.", hr);
            throw std::runtime_error("Embedded texture decoding failed.");
        }
    }

    static void TagAssimpMaterialNames(std::vector<unsigned char>& data)
    {
        if (data.size() < 20) throw std::runtime_error("GLB JSON chunk header is incomplete.");
        uint32_t oldLength = 0, chunkType = 0;
        std::memcpy(&oldLength, data.data() + 12, sizeof(oldLength));
        std::memcpy(&chunkType, data.data() + 16, sizeof(chunkType));
        if (chunkType != 0x4E4F534A || oldLength > data.size() - 20)
            throw std::runtime_error("GLB JSON chunk is invalid.");
        auto json = nlohmann::json::parse(data.begin() + 20, data.begin() + 20 + oldLength);
        if (!json.contains("materials")) return;
        auto& materials = json["materials"];
        for (size_t i = 0; i < materials.size(); ++i)
            materials[i]["name"] = std::string(MaterialTagPrefix) + std::to_string(i);
        std::string encoded = json.dump();
        encoded.append((4 - encoded.size() % 4) % 4, ' ');
        const size_t total = data.size() - oldLength + encoded.size();
        if (total > UINT32_MAX) throw std::runtime_error("GLB material mapping exceeds the format size limit.");
        const auto jsonLength = static_cast<uint32_t>(encoded.size());
        const auto totalLength = static_cast<uint32_t>(total);
        std::vector<unsigned char> tagged;
        tagged.reserve(total);
        tagged.insert(tagged.end(), data.begin(), data.begin() + 20);
        tagged.insert(tagged.end(), encoded.begin(), encoded.end());
        tagged.insert(tagged.end(), data.begin() + 20 + oldLength, data.end());
        std::memcpy(tagged.data() + 8, &totalLength, sizeof(totalLength));
        std::memcpy(tagged.data() + 12, &jsonLength, sizeof(jsonLength));
        data.swap(tagged);
    }

    std::vector<unsigned char> ReadFileToBuffer(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            ErrorLog::Write("Model: failed to open model file: " + path);
            return {};
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<unsigned char> buffer(size);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        if (!file)
        {
            ErrorLog::Write("Model: failed to read the complete model file: " + path);
        }
        return buffer;
    }
};

#endif
