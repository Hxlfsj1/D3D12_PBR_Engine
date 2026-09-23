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
            isMouseDown = true;
            lastX = (float)GET_X_LPARAM(lParam);
            lastY = (float)GET_Y_LPARAM(lParam);

            return true;
        }

        case WM_RBUTTONUP:
        {
            isMouseDown = false;

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

    void EndMouseLook() { isMouseDown = false; }

    void Update(float deltaTime, Camera& camera)
    {
        // Reserve W/E for editor tools; camera navigation requires right mouse.
        if (!isMouseDown || !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;
        if (GetAsyncKeyState('W') & 0x8000)
        {
            camera.ProcessKeyboard(FORWARD, deltaTime);
        }

        if (GetAsyncKeyState('S') & 0x8000)
        {
            camera.ProcessKeyboard(BACKWARD, deltaTime);
        }

        if (GetAsyncKeyState('A') & 0x8000)
        {
            camera.ProcessKeyboard(LEFT, deltaTime);
        }

        if (GetAsyncKeyState('D') & 0x8000)
        {
            camera.ProcessKeyboard(RIGHT, deltaTime);
        }
    }

private:
    float lastX;
    float lastY;
    bool isMouseDown;
};

#endif
