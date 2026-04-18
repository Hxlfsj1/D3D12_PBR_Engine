// The sole purpose of this header and its encapsulated classes is to compile shaders from specified file paths

#ifndef PBR_SHADER_H
#define PBR_SHADER_H

#include "stdafx.h"
#include <fstream>
#include <sstream>
#include <string>
#include <wrl/client.h>

class ShaderCompiler
{
public:

    static Microsoft::WRL::ComPtr<ID3DBlob> CompileFromFile(std::wstring fileName, std::string entryPoint, std::string target)
    {   
        // A container for the compiled shader bytecode
        Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
        // A container for shader compilation error messages
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

        UINT compileFlags = 0;

// Toggle between Debug and Release modes
#if defined(DEBUG) || defined(_DEBUG)
        compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        // Compile shaders to binary code (Bytecode)
        HRESULT hr = D3DCompileFromFile
        (
            fileName.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint.c_str(),
            target.c_str(),
            compileFlags,
            0,
            &shaderBlob,
            &errorBlob
        );

        // Abort execution if an error code is received
        if (FAILED(hr))
        {
            if (errorBlob)
            {
                OutputDebugStringA((char*)errorBlob->GetBufferPointer());
                MessageBoxA(NULL, (char*)errorBlob->GetBufferPointer(), "Shader Compilation Error", MB_OK | MB_ICONERROR);
            }

            else
            {
                MessageBoxA(NULL, "Shaders not found!", "Error", MB_OK | MB_ICONERROR);
            }

            exit(1);
        }

        return shaderBlob;
    }
};

#endif