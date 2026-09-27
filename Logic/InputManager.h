#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include "stdafx.h"
#include "Camera.h"

class InputManager
{
public:
    InputManager()
    {
        lastX = 0.0f;
        lastY = 0.0f;
        isMouseDown = false;
    }

    void Init(int width, int height)
    {
        lastX = width / 2.0f;
        lastY = height / 2.0f;
    }

    static bool ConfirmExit()
    {
        return MessageBox(0, L"Are you sure you want to exit?", L"Really?", MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    bool ProcessWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, Camera& camera)
    {
        // Handle discrete input in MsgProc
        switch (msg)
        {
        case WM_KEYDOWN:
        {
            if (wParam == VK_ESCAPE)
            {
                if (ConfirmExit())
                {
                    return false;
                }
            }
            return true;
        }

        case WM_RBUTTONDOWN:
        {
            camera.ResetFlySpeed();
            isMouseDown = true;
            lastX = (float)GET_X_LPARAM(lParam);
            lastY = (float)GET_Y_LPARAM(lParam);

            return true;
        }

        case WM_RBUTTONUP:
        {
            EndMouseLook(camera);

            return true;
        }

        case WM_MOUSEMOVE:
        {
            if (isMouseDown)
            {
                float xpos = (float)GET_X_LPARAM(lParam);
                float ypos = (float)GET_Y_LPARAM(lParam);
                float xoffset = xpos - lastX;
                float yoffset = lastY - ypos;

                lastX = xpos;
                lastY = ypos;

                camera.ProcessMouseMovement(xoffset, -yoffset);
            }

            return true;
        }

        case WM_MOUSEWHEEL:
        {
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            float scrollValue = (float)zDelta / WHEEL_DELTA;

            XMVECTOR pos = XMLoadFloat3(&camera.Position);
            XMVECTOR front = XMLoadFloat3(&camera.Front);

            pos = XMVectorAdd(pos, XMVectorScale(front, scrollValue * 1.0f));
            XMStoreFloat3(&camera.Position, pos);

            return true;
        }
        }

        return true;
    }

    void EndMouseLook(Camera& camera) { isMouseDown = false; camera.ResetFlySpeed(); }

    void Update(float deltaTime, Camera& camera, bool allowNavigation = true)
    {
        // Reserve W/E for editor tools; camera navigation requires right mouse.
        if (!allowNavigation || !isMouseDown || !(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
        {
            EndMouseLook(camera);
            return;
        }
        const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) ? 1.0f : 0.0f; };
        camera.ProcessFlyMovement(down('D') - down('A'), down('Q') - down('E'),
            down('W') - down('S'), deltaTime, down(VK_SHIFT) != 0.0f);
    }

private:
    float lastX;
    float lastY;
    bool isMouseDown;
};

#endif
