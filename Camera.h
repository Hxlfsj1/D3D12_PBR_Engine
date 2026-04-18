#ifndef CAMERA_H
#define CAMERA_H

#include <DirectXMath.h>

using namespace DirectX;

enum Camera_Movement
{
    FORWARD,
    BACKWARD,
    LEFT,
    RIGHT
};

const float YAW = 0.0f;
const float PITCH = 0.0f;
const float SPEED = 2.5f;
const float SENSITIVITY = 0.25f;
const float ZOOM = 45.0f;

class Camera
{
public:

    XMFLOAT3 Position;
    XMFLOAT3 Front;
    XMFLOAT3 Up;
    XMFLOAT3 Right;
    XMFLOAT3 WorldUp;

    float Yaw;
    float Pitch;

    XMFLOAT4 Orientation;

    float MovementSpeed;
    float MouseSensitivity;
    float Zoom;

    Camera(XMFLOAT3 position = XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3 up = XMFLOAT3(0.0f, 1.0f, 0.0f), float yaw = YAW, float pitch = PITCH)
        : MovementSpeed(SPEED), MouseSensitivity(SENSITIVITY), Zoom(ZOOM)
    {
        Position = position;
        WorldUp = up;
        Yaw = yaw;
        Pitch = pitch;
        updateCameraVectors();
    }

    Camera(float posX, float posY, float posZ, float upX, float upY, float upZ, float yaw, float pitch)
        : MovementSpeed(SPEED), MouseSensitivity(SENSITIVITY), Zoom(ZOOM)
    {
        Position = XMFLOAT3(posX, posY, posZ);
        WorldUp = XMFLOAT3(upX, upY, upZ);
        Yaw = yaw;
        Pitch = pitch;
        updateCameraVectors();
    }

    XMMATRIX GetViewMatrix()
    {
        XMVECTOR pos = XMLoadFloat3(&Position);
        XMVECTOR front = XMLoadFloat3(&Front);
        XMVECTOR up = XMLoadFloat3(&Up);

        return XMMatrixLookAtLH(pos, XMVectorAdd(pos, front), up);
    }

    void ProcessKeyboard(Camera_Movement direction, float deltaTime)
    {
        float velocity = MovementSpeed * deltaTime;

        XMVECTOR pos = XMLoadFloat3(&Position);
        XMVECTOR front = XMLoadFloat3(&Front);
        XMVECTOR right = XMLoadFloat3(&Right);

        if (direction == FORWARD)
        {
            pos = XMVectorAdd(pos, XMVectorScale(front, velocity));
        }
        if (direction == BACKWARD)
        {
            pos = XMVectorSubtract(pos, XMVectorScale(front, velocity));
        }
        if (direction == LEFT)
        {
            pos = XMVectorSubtract(pos, XMVectorScale(right, velocity));
        }
        if (direction == RIGHT)
        {
            pos = XMVectorAdd(pos, XMVectorScale(right, velocity));
        }

        XMStoreFloat3(&Position, pos);
    }

    void ProcessMouseMovement(float xoffset, float yoffset, bool constrainPitch = true)
    {
        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        Yaw += xoffset;
        Pitch += yoffset;

        if (constrainPitch)
        {
            if (Pitch > 89.0f)
            {
                Pitch = 89.0f;
            }
            if (Pitch < -89.0f)
            {
                Pitch = -89.0f;
            }
        }

        updateCameraVectors();
    }

    void ProcessMouseScroll(float yoffset)
    {
        Zoom -= yoffset;
        if (Zoom < 1.0f)
        {
            Zoom = 1.0f;
        }
        if (Zoom > 45.0f)
        {
            Zoom = 45.0f;
        }
    }

private:

    void updateCameraVectors()
    {
        XMVECTOR quat = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(Pitch),
            XMConvertToRadians(Yaw),
            0.0f
        );

        quat = XMQuaternionNormalize(quat);
        XMStoreFloat4(&Orientation, quat);

        XMVECTOR defaultFront = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        XMVECTOR defaultRight = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        XMVECTOR defaultUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        XMVECTOR front = XMVector3Rotate(defaultFront, quat);
        XMVECTOR right = XMVector3Rotate(defaultRight, quat);
        XMVECTOR up = XMVector3Rotate(defaultUp, quat);

        XMStoreFloat3(&Front, XMVector3Normalize(front));
        XMStoreFloat3(&Right, XMVector3Normalize(right));
        XMStoreFloat3(&Up, XMVector3Normalize(up));
    }
};

#endif