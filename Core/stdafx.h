#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxcapi.h>
#include <DirectXMath.h>

#include "d3dx12.h"

#if !defined(_WIN64)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#endif
#pragma comment(lib, "dxcompiler.lib")
#pragma comment(lib, "dxguid.lib")
