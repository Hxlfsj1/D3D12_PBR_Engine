#ifndef DLSS_MANAGER_H
#define DLSS_MANAGER_H

#include <Windows.h>
#include <d3d12.h>

#if defined(_WIN64)

#include <cstdio>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

class DLSSManager
{
public:
    struct EvaluationInput
    {
        ID3D12Resource* color = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* motionVectors = nullptr;
        ID3D12Resource* output = nullptr;
        float jitterOffsetX = 0.0f;
        float jitterOffsetY = 0.0f;
        float frameTimeDeltaInMsec = 0.0f;
        bool reset = false;
    };

    DLSSManager() = default;

    ~DLSSManager()
    {
        Shutdown();
    }

    DLSSManager(const DLSSManager&) = delete;
    DLSSManager& operator=(const DLSSManager&) = delete;

    bool Initialize(ID3D12Device* device)
    {
        if (m_initialized)
        {
            return true;
        }

        if (device == nullptr)
        {
            OutputDebugStringA("DLSS: cannot initialize NGX with a null D3D12 device.\n");
            return false;
        }

        m_applicationDataPath = BuildApplicationDataPath();
        if (m_applicationDataPath.empty())
        {
            OutputDebugStringA("DLSS: failed to create the writable NGX application-data directory.\n");
            return false;
        }

        m_lastResult = NVSDK_NGX_D3D12_Init_with_ProjectID(
            "94b5c74b-94cb-436b-af74-2734687aae70",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "1.0.0",
            m_applicationDataPath.c_str(),
            device);

        if (NVSDK_NGX_FAILED(m_lastResult))
        {
            LogResult("NVSDK_NGX_D3D12_Init_with_ProjectID", m_lastResult);
            return false;
        }

        m_device = device;
        m_initialized = true;
        OutputDebugStringA("DLSS: NGX initialized and bound to the D3D12 device.\n");
        return true;
    }

    bool QueryDLSSCapability()
    {
        ResetFeatureConfiguration();
        m_dlssAvailable = false;
        m_needsUpdatedDriver = false;
        m_minDriverVersionMajor = 0;
        m_minDriverVersionMinor = 0;

        if (!m_initialized)
        {
            OutputDebugStringA("DLSS: cannot query capabilities before NGX initialization.\n");
            return false;
        }

        DestroyCapabilityParameters();
        m_lastResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_capabilityParameters);
        if (NVSDK_NGX_FAILED(m_lastResult) || m_capabilityParameters == nullptr)
        {
            LogResult("NVSDK_NGX_D3D12_GetCapabilityParameters", m_lastResult);
            return false;
        }

        int dlssSupported = 0;
        int needsUpdatedDriver = 0;
        unsigned int minDriverVersionMajor = 0;
        unsigned int minDriverVersionMinor = 0;

        const NVSDK_NGX_Result availabilityResult = m_capabilityParameters->Get(
            NVSDK_NGX_Parameter_SuperSampling_Available,
            &dlssSupported);
        const NVSDK_NGX_Result driverUpdateResult = m_capabilityParameters->Get(
            NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
            &needsUpdatedDriver);
        const NVSDK_NGX_Result minDriverMajorResult = m_capabilityParameters->Get(
            NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
            &minDriverVersionMajor);
        const NVSDK_NGX_Result minDriverMinorResult = m_capabilityParameters->Get(
            NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
            &minDriverVersionMinor);

        if (NVSDK_NGX_FAILED(availabilityResult))
        {
            LogResult("DLSS availability query", availabilityResult);
            return false;
        }

        if (NVSDK_NGX_SUCCEED(driverUpdateResult))
        {
            m_needsUpdatedDriver = needsUpdatedDriver != 0;
        }

        if (NVSDK_NGX_SUCCEED(minDriverMajorResult))
        {
            m_minDriverVersionMajor = minDriverVersionMajor;
        }

        if (NVSDK_NGX_SUCCEED(minDriverMinorResult))
        {
            m_minDriverVersionMinor = minDriverVersionMinor;
        }

        m_dlssAvailable = dlssSupported != 0 && !m_needsUpdatedDriver;

        if (m_dlssAvailable)
        {
            OutputDebugStringA("DLSS: capability query succeeded; DLSS Super Resolution is available.\n");
        }
        else if (m_needsUpdatedDriver)
        {
            char message[192] = {};
            sprintf_s(
                message,
                "DLSS: a driver update is required; minimum reported version is %u.%u.\n",
                m_minDriverVersionMajor,
                m_minDriverVersionMinor);
            OutputDebugStringA(message);
        }
        else
        {
            OutputDebugStringA("DLSS: capability query succeeded; DLSS Super Resolution is unavailable.\n");
        }

        return m_dlssAvailable;
    }

    bool ConfigureFeature(
        unsigned int outputWidth,
        unsigned int outputHeight,
        NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality)
    {
        ResetFeatureConfiguration();

        if (!m_dlssAvailable || m_capabilityParameters == nullptr)
        {
            OutputDebugStringA("DLSS: cannot configure the feature before capability validation.\n");
            return false;
        }

        if (outputWidth == 0 || outputHeight == 0)
        {
            OutputDebugStringA("DLSS: cannot configure the feature with a zero output dimension.\n");
            return false;
        }

        m_lastResult = NGX_DLSS_GET_OPTIMAL_SETTINGS(
            m_capabilityParameters,
            outputWidth,
            outputHeight,
            perfQuality,
            &m_renderWidth,
            &m_renderHeight,
            &m_maxRenderWidth,
            &m_maxRenderHeight,
            &m_minRenderWidth,
            &m_minRenderHeight,
            &m_recommendedSharpness);

        if (NVSDK_NGX_FAILED(m_lastResult) || m_renderWidth == 0 || m_renderHeight == 0)
        {
            LogResult("NGX_DLSS_GET_OPTIMAL_SETTINGS", m_lastResult);
            ResetFeatureConfiguration();
            return false;
        }

        m_outputWidth = outputWidth;
        m_outputHeight = outputHeight;
        m_perfQuality = perfQuality;
        m_featureConfigured = true;

        char message[256] = {};
        sprintf_s(
            message,
            "DLSS: configured Quality mode at %ux%u -> %ux%u (dynamic range %ux%u to %ux%u).\n",
            m_renderWidth,
            m_renderHeight,
            m_outputWidth,
            m_outputHeight,
            m_minRenderWidth,
            m_minRenderHeight,
            m_maxRenderWidth,
            m_maxRenderHeight);
        OutputDebugStringA(message);
        return true;
    }

    bool CreateFeature(ID3D12GraphicsCommandList* commandList)
    {
        if (m_featureHandle != nullptr)
        {
            return true;
        }

        if (m_featureCreationAttempted)
        {
            return false;
        }

        m_featureCreationAttempted = true;

        if (!m_initialized || !m_dlssAvailable || !m_featureConfigured ||
            m_capabilityParameters == nullptr || commandList == nullptr)
        {
            OutputDebugStringA("DLSS: cannot create the feature before configuration or without a recording command list.\n");
            return false;
        }

        NVSDK_NGX_DLSS_Create_Params createParams = {};
        createParams.Feature.InWidth = m_renderWidth;
        createParams.Feature.InHeight = m_renderHeight;
        createParams.Feature.InTargetWidth = m_outputWidth;
        createParams.Feature.InTargetHeight = m_outputHeight;
        createParams.Feature.InPerfQualityValue = m_perfQuality;
        createParams.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        createParams.InEnableOutputSubrects = false;

        m_lastResult = NGX_D3D12_CREATE_DLSS_EXT(
            commandList,
            1,
            1,
            &m_featureHandle,
            m_capabilityParameters,
            &createParams);

        if (NVSDK_NGX_FAILED(m_lastResult) || m_featureHandle == nullptr)
        {
            LogResult("NGX_D3D12_CREATE_DLSS_EXT", m_lastResult);
            m_featureHandle = nullptr;
            return false;
        }

        OutputDebugStringA("DLSS: feature instance created and retained for evaluation.\n");
        return true;
    }

    bool BuildEvaluationParams(
        const EvaluationInput& input,
        NVSDK_NGX_D3D12_DLSS_Eval_Params* outputParams) const noexcept
    {
        if (outputParams == nullptr)
        {
            return false;
        }

        *outputParams = {};

        if (m_featureHandle == nullptr || !m_featureConfigured ||
            input.color == nullptr || input.depth == nullptr ||
            input.motionVectors == nullptr || input.output == nullptr ||
            !std::isfinite(input.jitterOffsetX) ||
            !std::isfinite(input.jitterOffsetY) ||
            !std::isfinite(input.frameTimeDeltaInMsec) ||
            std::abs(input.jitterOffsetX) > 0.5f ||
            std::abs(input.jitterOffsetY) > 0.5f ||
            input.frameTimeDeltaInMsec < 0.0f)
        {
            return false;
        }

        const D3D12_RESOURCE_DESC colorDesc = input.color->GetDesc();
        const D3D12_RESOURCE_DESC depthDesc = input.depth->GetDesc();
        const D3D12_RESOURCE_DESC motionDesc = input.motionVectors->GetDesc();
        const D3D12_RESOURCE_DESC outputDesc = input.output->GetDesc();

        if (!IsTexture2DAtSize(colorDesc, m_renderWidth, m_renderHeight) ||
            !IsTexture2DAtSize(depthDesc, m_renderWidth, m_renderHeight) ||
            !IsTexture2DAtSize(motionDesc, m_renderWidth, m_renderHeight) ||
            !IsTexture2DAtSize(outputDesc, m_outputWidth, m_outputHeight) ||
            colorDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            depthDesc.Format != DXGI_FORMAT_R32_TYPELESS ||
            motionDesc.Format != DXGI_FORMAT_R16G16_FLOAT ||
            outputDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            (outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        {
            return false;
        }

        outputParams->Feature.pInColor = input.color;
        outputParams->Feature.pInOutput = input.output;
        outputParams->Feature.InSharpness = 0.0f;
        outputParams->pInDepth = input.depth;
        outputParams->pInMotionVectors = input.motionVectors;
        outputParams->InJitterOffsetX = input.jitterOffsetX;
        outputParams->InJitterOffsetY = input.jitterOffsetY;
        outputParams->InRenderSubrectDimensions.Width = m_renderWidth;
        outputParams->InRenderSubrectDimensions.Height = m_renderHeight;
        outputParams->InReset = input.reset ? 1 : 0;

        // MotionUV stores currentUV - previousUV. DLSS expects a vector from
        // the current pixel to its previous-frame position in pixel units.
        outputParams->InMVScaleX = -static_cast<float>(m_renderWidth);
        outputParams->InMVScaleY = -static_cast<float>(m_renderHeight);

        outputParams->InPreExposure = 1.0f;
        outputParams->InExposureScale = 1.0f;
        outputParams->InFrameTimeDeltaInMsec = input.frameTimeDeltaInMsec;
        return true;
    }

    bool EvaluateFeature(
        ID3D12GraphicsCommandList* commandList,
        const EvaluationInput& input) noexcept
    {
        if (!CanEvaluate() || commandList == nullptr)
        {
            return false;
        }

        m_evaluationAttempted = true;
        m_lastEvaluationSucceeded = false;

        NVSDK_NGX_D3D12_DLSS_Eval_Params evalParams = {};
        if (!BuildEvaluationParams(input, &evalParams))
        {
            m_evaluationDisabled = true;
            OutputDebugStringA("DLSS: evaluation parameters are invalid; disabling DLSS evaluation.\n");
            return false;
        }

        m_lastResult = NGX_D3D12_EVALUATE_DLSS_EXT(
            commandList,
            m_featureHandle,
            m_capabilityParameters,
            &evalParams);
        if (NVSDK_NGX_FAILED(m_lastResult))
        {
            m_evaluationDisabled = true;
            LogResult("NGX_D3D12_EVALUATE_DLSS_EXT", m_lastResult);
            OutputDebugStringA("DLSS: evaluation failed; disabling DLSS evaluation.\n");
            return false;
        }

        m_lastEvaluationSucceeded = true;
        if (!m_hasSuccessfulEvaluation)
        {
            m_hasSuccessfulEvaluation = true;
            OutputDebugStringA("DLSS: first evaluation succeeded; the DLSS output will be presented next frame.\n");
        }

        return true;
    }

    bool CanEvaluate() const noexcept
    {
        return m_featureHandle != nullptr &&
            m_capabilityParameters != nullptr &&
            m_featureConfigured &&
            !m_evaluationDisabled;
    }

    bool WasLastEvaluationSuccessful() const noexcept
    {
        return m_lastEvaluationSucceeded;
    }

    bool HasSuccessfulEvaluation() const noexcept
    {
        return m_hasSuccessfulEvaluation;
    }

    void Shutdown() noexcept
    {
        ResetFeatureConfiguration();
        DestroyCapabilityParameters();

        if (!m_initialized)
        {
            return;
        }

        m_lastResult = NVSDK_NGX_D3D12_Shutdown1(m_device);
        if (NVSDK_NGX_FAILED(m_lastResult))
        {
            LogResult("NVSDK_NGX_D3D12_Shutdown1", m_lastResult);
        }

        m_device = nullptr;
        m_initialized = false;
        m_dlssAvailable = false;
        m_needsUpdatedDriver = false;
        m_minDriverVersionMajor = 0;
        m_minDriverVersionMinor = 0;
    }

    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

    bool IsDLSSAvailable() const noexcept
    {
        return m_dlssAvailable;
    }

    bool IsFeatureConfigured() const noexcept
    {
        return m_featureConfigured;
    }

    bool IsFeatureCreated() const noexcept
    {
        return m_featureHandle != nullptr;
    }

    bool WasFeatureCreationAttempted() const noexcept
    {
        return m_featureCreationAttempted;
    }

    unsigned int GetRenderWidth() const noexcept
    {
        return m_renderWidth;
    }

    unsigned int GetRenderHeight() const noexcept
    {
        return m_renderHeight;
    }

    bool NeedsUpdatedDriver() const noexcept
    {
        return m_needsUpdatedDriver;
    }

    unsigned int GetMinDriverVersionMajor() const noexcept
    {
        return m_minDriverVersionMajor;
    }

    unsigned int GetMinDriverVersionMinor() const noexcept
    {
        return m_minDriverVersionMinor;
    }

private:
    static bool IsTexture2DAtSize(
        const D3D12_RESOURCE_DESC& desc,
        unsigned int width,
        unsigned int height) noexcept
    {
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.Width == static_cast<UINT64>(width) &&
            desc.Height == height &&
            desc.DepthOrArraySize == 1 &&
            desc.MipLevels == 1 &&
            desc.SampleDesc.Count == 1;
    }

    void DestroyFeature() noexcept
    {
        if (m_featureHandle == nullptr)
        {
            return;
        }

        const NVSDK_NGX_Result releaseResult = NVSDK_NGX_D3D12_ReleaseFeature(m_featureHandle);
        if (NVSDK_NGX_FAILED(releaseResult))
        {
            LogResult("NVSDK_NGX_D3D12_ReleaseFeature", releaseResult);
        }

        m_featureHandle = nullptr;
    }

    void DestroyCapabilityParameters() noexcept
    {
        if (m_capabilityParameters == nullptr)
        {
            return;
        }

        const NVSDK_NGX_Result destroyResult = NVSDK_NGX_D3D12_DestroyParameters(m_capabilityParameters);
        if (NVSDK_NGX_FAILED(destroyResult))
        {
            LogResult("NVSDK_NGX_D3D12_DestroyParameters", destroyResult);
        }

        m_capabilityParameters = nullptr;
    }

    void ResetFeatureConfiguration() noexcept
    {
        DestroyFeature();
        m_featureConfigured = false;
        m_featureCreationAttempted = false;
        m_renderWidth = 0;
        m_renderHeight = 0;
        m_maxRenderWidth = 0;
        m_maxRenderHeight = 0;
        m_minRenderWidth = 0;
        m_minRenderHeight = 0;
        m_outputWidth = 0;
        m_outputHeight = 0;
        m_recommendedSharpness = 0.0f;
        m_evaluationAttempted = false;
        m_lastEvaluationSucceeded = false;
        m_hasSuccessfulEvaluation = false;
        m_evaluationDisabled = false;
    }

    static std::wstring BuildApplicationDataPath()
    {
        std::filesystem::path basePath;
        wchar_t localAppData[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);

        if (length > 0 && length < MAX_PATH)
        {
            basePath = localAppData;
        }
        else
        {
            std::error_code tempPathError;
            basePath = std::filesystem::temp_directory_path(tempPathError);
            if (tempPathError)
            {
                return {};
            }
        }

        const std::filesystem::path ngxDataPath = basePath / L"LearnDirectX" / L"NGX";
        std::error_code createDirectoryError;
        std::filesystem::create_directories(ngxDataPath, createDirectoryError);
        if (createDirectoryError)
        {
            return {};
        }

        return ngxDataPath.wstring();
    }

    static void LogResult(const char* operation, NVSDK_NGX_Result result) noexcept
    {
        char message[192] = {};
        sprintf_s(
            message,
            "%s failed with NVSDK_NGX_Result 0x%08X. Check the NGX log for details.\n",
            operation,
            static_cast<unsigned int>(result));
        OutputDebugStringA(message);
    }

    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_capabilityParameters = nullptr;
    NVSDK_NGX_Handle* m_featureHandle = nullptr;
    NVSDK_NGX_Result m_lastResult = NVSDK_NGX_Result_Fail;
    std::wstring m_applicationDataPath;
    NVSDK_NGX_PerfQuality_Value m_perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    unsigned int m_renderWidth = 0;
    unsigned int m_renderHeight = 0;
    unsigned int m_maxRenderWidth = 0;
    unsigned int m_maxRenderHeight = 0;
    unsigned int m_minRenderWidth = 0;
    unsigned int m_minRenderHeight = 0;
    unsigned int m_outputWidth = 0;
    unsigned int m_outputHeight = 0;
    float m_recommendedSharpness = 0.0f;
    bool m_initialized = false;
    bool m_dlssAvailable = false;
    bool m_featureConfigured = false;
    bool m_featureCreationAttempted = false;
    bool m_evaluationAttempted = false;
    bool m_lastEvaluationSucceeded = false;
    bool m_hasSuccessfulEvaluation = false;
    bool m_evaluationDisabled = false;
    bool m_needsUpdatedDriver = false;
    unsigned int m_minDriverVersionMajor = 0;
    unsigned int m_minDriverVersionMinor = 0;
};

#else

class DLSSManager
{
public:
    struct EvaluationInput
    {
        ID3D12Resource* color = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* motionVectors = nullptr;
        ID3D12Resource* output = nullptr;
        float jitterOffsetX = 0.0f;
        float jitterOffsetY = 0.0f;
        float frameTimeDeltaInMsec = 0.0f;
        bool reset = false;
    };

    bool Initialize(ID3D12Device*)
    {
        OutputDebugStringA("DLSS: Direct NGX integration requires an x64 build.\n");
        return false;
    }

    bool QueryDLSSCapability()
    {
        return false;
    }

    bool ConfigureFeature(unsigned int, unsigned int)
    {
        return false;
    }

    bool CreateFeature(ID3D12GraphicsCommandList*)
    {
        return false;
    }

    bool EvaluateFeature(ID3D12GraphicsCommandList*, const EvaluationInput&)
    {
        return false;
    }

    bool CanEvaluate() const noexcept
    {
        return false;
    }

    bool WasLastEvaluationSuccessful() const noexcept
    {
        return false;
    }

    bool HasSuccessfulEvaluation() const noexcept
    {
        return false;
    }

    void Shutdown() noexcept
    {
    }

    bool IsInitialized() const noexcept
    {
        return false;
    }

    bool IsDLSSAvailable() const noexcept
    {
        return false;
    }

    bool IsFeatureConfigured() const noexcept
    {
        return false;
    }

    bool IsFeatureCreated() const noexcept
    {
        return false;
    }

    bool WasFeatureCreationAttempted() const noexcept
    {
        return false;
    }

    unsigned int GetRenderWidth() const noexcept
    {
        return 0;
    }

    unsigned int GetRenderHeight() const noexcept
    {
        return 0;
    }

    bool NeedsUpdatedDriver() const noexcept
    {
        return false;
    }

    unsigned int GetMinDriverVersionMajor() const noexcept
    {
        return 0;
    }

    unsigned int GetMinDriverVersionMinor() const noexcept
    {
        return 0;
    }
};

#endif

#endif
