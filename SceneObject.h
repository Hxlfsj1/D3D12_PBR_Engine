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

    XMMATRIX GetWorldMatrix() const
    {
        return XMMatrixScaling(scale.x, scale.y, scale.z) *
               XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
               XMMatrixTranslation(translation.x, translation.y, translation.z);
    }
};