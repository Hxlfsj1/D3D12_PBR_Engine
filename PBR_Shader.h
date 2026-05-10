#ifndef PBR_SHADER_H
#define PBR_SHADER_H

#include "stdafx.h"
#include <string>
#include <vector>
#include <wrl/client.h>

class ShaderCompiler
{
public:
    static Microsoft::WRL::ComPtr<IDxcBlob> CompileFromFile(
        std::wstring fileName,
        std::wstring entryPoint,
        std::wstring target,
        const std::vector<std::wstring>& defines = {})
    {
        Microsoft::WRL::ComPtr<IDxcUtils> pUtils;
        DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils));

        Microsoft::WRL::ComPtr<IDxcCompiler3> pCompiler;
        DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler));

        Microsoft::WRL::ComPtr<IDxcIncludeHandler> pIncludeHandler;
        pUtils->CreateDefaultIncludeHandler(&pIncludeHandler);

        Microsoft::WRL::ComPtr<IDxcBlobEncoding> pSource;
        HRESULT hr = pUtils->LoadFile(fileName.c_str(), nullptr, &pSource);
        if (FAILED(hr))
        {
            MessageBoxA(NULL, "Shaders not found!", "Error", MB_OK | MB_ICONERROR);
            exit(1);
        }

        std::vector<LPCWSTR> arguments;
        arguments.push_back(fileName.c_str());

        arguments.push_back(L"-E");
        arguments.push_back(entryPoint.c_str());

        arguments.push_back(L"-T");
        arguments.push_back(target.c_str());

        for (const auto& define : defines)
        {
            arguments.push_back(L"-D");
            arguments.push_back(define.c_str());
        }

#if defined(DEBUG) || defined(_DEBUG)
        arguments.push_back(DXC_ARG_DEBUG);
        arguments.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
#endif

        DxcBuffer sourceBuffer;
        sourceBuffer.Ptr = pSource->GetBufferPointer();
        sourceBuffer.Size = pSource->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_ACP;

        Microsoft::WRL::ComPtr<IDxcResult> pResults;
        pCompiler->Compile(
            &sourceBuffer,
            arguments.data(),
            (UINT32)arguments.size(),
            pIncludeHandler.Get(),
            IID_PPV_ARGS(&pResults)
        );

        Microsoft::WRL::ComPtr<IDxcBlobUtf8> pErrors;
        pResults->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
        if (pErrors != nullptr && pErrors->GetStringLength() > 0)
        {
            OutputDebugStringA(pErrors->GetStringPointer());
            MessageBoxA(NULL, pErrors->GetStringPointer(), "DXC Compilation Error", MB_OK | MB_ICONERROR);
            exit(1);
        }

        Microsoft::WRL::ComPtr<IDxcBlob> pShader;
        pResults->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pShader), nullptr);

        return pShader;
    }
};

#endif