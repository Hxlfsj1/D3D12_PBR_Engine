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

#if defined(LEARNDIRECTX_ENABLE_STREAMLINE)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#else
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#endif
#pragma comment(lib, "dxcompiler.lib")
#pragma comment(lib, "dxguid.lib")
