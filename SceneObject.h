#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>

using namespace DirectX;

struct Model;

struct ModelInstance
{
    std::string name;
    // Pointer to the model data
    Model* pModel;

    XMFLOAT3 translation = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 rotation = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };

    bool isTransparent = false;

    // Dirty Flag Mechanism
    XMMATRIX cachedWorldMat;
    XMMATRIX cachedNormalMat;
    bool isDirty = true;

    void SetTranslation(float x, float y, float z) { translation = { x, y, z }; isDirty = true; }
    void SetRotation(float x, float y, float z) { rotation = { x, y, z }; isDirty = true; }
    void SetScale(float x, float y, float z) { scale = { x, y, z }; isDirty = true; }

    // Calculate in CPU only if it is dirty
    void UpdateTransform()
    {
        if (isDirty)
        {
            cachedWorldMat = XMMatrixScaling(scale.x, scale.y, scale.z) *
                XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
                XMMatrixTranslation(translation.x, translation.y, translation.z);

            XMVECTOR det = XMMatrixDeterminant(cachedWorldMat);
            XMMATRIX invWorld = XMMatrixInverse(&det, cachedWorldMat);
            cachedNormalMat = XMMatrixTranspose(invWorld);

            isDirty = false;
        }
    }
};