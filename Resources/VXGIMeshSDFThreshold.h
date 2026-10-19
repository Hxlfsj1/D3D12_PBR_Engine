#pragma once

#include "VXGIMeshSDFScene.h"
#include "VXGISDFThreshold.h"
#include <limits>

// CPU reference for the lightweight SSGI consumer. The instance BVH chooses
// only two tight model bounds; this query then reads their scalar nodes only.
// Probe capture/classification still uses its independent geometry path.
class VXGIMeshSDFThreshold
{
public:
    using Status = VXGISDFThreshold::Status;
    using Parameters = VXGISDFThreshold::Parameters;

    struct Stats
    {
        VXGIInstanceBVH::QueryStats broadPhase;
        uint32_t candidates = 0, localQueries = 0, samples = 0;
    };

    struct Result
    {
        Status status = Status::Invalid;
        DirectX::XMFLOAT3 position = {};
        float distance = 0, fieldDistance = 0;
        uint32_t instanceIndex = UINT32_MAX, instanceId = 0;
    };

    static Result Trace(const VXGIMeshSDFScene& scene,
        const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
        float rayMin, float rayMax, const Parameters& parameters, Stats* stats = nullptr)
    {
        if (stats) *stats = {};
        Result result;
        DirectX::XMFLOAT3 unit;
        if (!VXGISDFThreshold::ValidParameters(parameters) ||
            !VXGISDFThreshold::NormalizeDirection(direction, unit)) return result;
        // The existing double-precision CPU broad phase is a reference only.
        // The local field query and all transforms below use FP32, like HLSL.
        const auto candidates = scene.QueryClosestTwoInstances(origin, unit, rayMin, rayMax,
            stats ? &stats->broadPhase : nullptr);
        if (!candidates.valid) return result;
        result.status = Status::Miss;
        if (stats) stats->candidates = candidates.count;
        Status incompleteStatus = Status::Miss;
        float incompleteEntry = (std::numeric_limits<float>::max)();
        for (uint32_t i = 0; i < candidates.count; ++i)
        {
            const auto& candidate = candidates.candidates[i];
            const float entry = (std::max)(rayMin, (std::min)(rayMax, float(candidate.entryDistance)));
            const float exit = (std::max)(entry, (std::min)(rayMax, float(candidate.exitDistance)));
            if (result.status == Status::Hit && entry > result.distance) continue;
            const auto& instance = scene.GetInstances()[candidate.instanceIndex];
            const auto inverse = DirectX::XMLoadFloat4x4(&instance.localFromWorld);
            DirectX::XMFLOAT3 localOrigin, localVector;
            DirectX::XMStoreFloat3(&localOrigin, DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(&origin), inverse));
            DirectX::XMStoreFloat3(&localVector, DirectX::XMVector3TransformNormal(
                DirectX::XMLoadFloat3(&unit), inverse));
            const float largest = (std::max)({std::abs(localVector.x), std::abs(localVector.y), std::abs(localVector.z)});
            if (!std::isfinite(largest) || largest <= 0) return Result{};
            const DirectX::XMFLOAT3 scaled = {localVector.x / largest, localVector.y / largest, localVector.z / largest};
            const float distanceScale = largest * std::sqrt(scaled.x * scaled.x + scaled.y * scaled.y + scaled.z * scaled.z);
            if (!std::isfinite(distanceScale) || distanceScale <= 0) return Result{};

            const auto& sdf = instance.asset->GetSDF();
            const auto& sourceGrid = sdf.GetGrid();
            const VXGISDFThreshold::Grid grid = {sourceGrid.origin, sourceGrid.cellSize,
                sourceGrid.cellDimensions, 0, sourceGrid.boundsMaximum, 0};
            if (stats) ++stats->localQueries;
            const float maximum = result.status == Status::Hit ? (std::min)(exit, result.distance) : exit;
            const auto local = VXGISDFThreshold::TraceLocal(grid, sdf.GetNodes(), localOrigin,
                localVector, entry * distanceScale, maximum * distanceScale, parameters);
            if (stats) stats->samples += local.sampleCount;
            if (local.status == Status::Invalid) return Result{};
            if (local.status == Status::Exhausted || local.status == Status::Stalled)
            {
                if (entry < incompleteEntry)
                {
                    incompleteEntry = entry;
                    incompleteStatus = local.status;
                }
                continue;
            }
            if (local.status != Status::Hit) continue;
            // Convert local unit distance once. Preserve the original finite
            // world ray even when the final FP32 division rounds an endpoint.
            const float distance = (std::max)(entry, (std::min)(maximum, local.distance / distanceScale));
            if (result.status == Status::Hit && (distance > result.distance ||
                (distance == result.distance && candidate.instanceId >= result.instanceId))) continue;
            result.status = Status::Hit;
            result.distance = distance;
            result.position = {origin.x + unit.x * distance, origin.y + unit.y * distance, origin.z + unit.z * distance};
            result.fieldDistance = local.fieldDistance;
            result.instanceIndex = candidate.instanceIndex;
            result.instanceId = candidate.instanceId;
        }
        // A capped/stalled nearer candidate must not masquerade as a proven
        // miss, or allow an unproven farther hit to be called the closest one.
        if (incompleteStatus != Status::Miss &&
            (result.status != Status::Hit || incompleteEntry <= result.distance))
        {
            result = {};
            result.status = incompleteStatus;
        }
        return result;
    }

    static Result Trace(const VXGIMeshSDFScene& scene,
        const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
        float rayMin, float rayMax, Stats* stats = nullptr)
    {
        return Trace(scene, origin, direction, rayMin, rayMax, Parameters{}, stats);
    }
};
