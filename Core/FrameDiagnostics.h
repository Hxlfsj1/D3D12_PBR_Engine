#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "../ThirdParty/json.hpp"

// Optional single-threaded capture. Collection only inspects already completed
// fences: instrumentation never submits commands, signals a fence, or waits.
class FrameDiagnostics
{
public:
    using Json = nlohmann::json;
    static constexpr UINT TimestampCapacity = 128;
    static constexpr size_t MaximumFrames = 12000;

    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                    int slots, const std::string& outputPrefix)
    {
        if (m_initialized) return true;
        if (!device || !queue || slots <= 0 || outputPrefix.empty()) return false;

        UINT64 frequency = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency) return false;
        const UINT64 queryCount = UINT64(slots) * TimestampCapacity;
        if (queryCount > (std::numeric_limits<UINT>::max)()) return false;

        D3D12_QUERY_HEAP_DESC queryDesc{};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryDesc.Count = static_cast<UINT>(queryCount);
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queryHeap;
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queryHeap)))) return false;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = queryCount * sizeof(UINT64);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return false;

        m_queryHeap = std::move(queryHeap);
        m_readback = std::move(readback);
        m_slots.resize(static_cast<size_t>(slots));
        m_rows.reserve(MaximumFrames);
        m_frequency = frequency;
        m_outputPrefix = outputPrefix;
        m_initialized = true;
        return true;
    }

    bool IsEnabled() const { return m_initialized; }

    void BeginFrame(int slot, uint64_t frame, int mode, double wallMs)
    {
        m_activeSlot = -1;
        if (!m_initialized || !ValidSlot(slot) || m_startedFrames >= MaximumFrames) return;
        Slot& state = m_slots[static_cast<size_t>(slot)];
        // The owner calls Collect after its normal allocator/fence wait. If that
        // did not happen, skip the frame instead of overwriting in-flight queries.
        if (state.submitted)
        {
            ++m_skippedBusySlots;
            return;
        }
        if (state.hasFrame)
        {
            state.row["metadata"]["diagnostics_gpu_not_submitted"] = true;
            m_rows.push_back(std::move(state.row));
            ++m_abandonedFrames;
        }
        state = Slot{};
        state.hasFrame = true;
        state.row = {
            {"frame", frame}, {"mode", mode}, {"wall_ms", wallMs},
            {"utc_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
            {"cpu_ms", Json::object()}, {"gpu_ms", Json::object()},
            {"metadata", Json::object()}
        };
        m_activeSlot = slot;
        ++m_startedFrames;
    }

    void CPU(const char* name, double ms)
    {
        Slot* state = Active();
        if (!state || !name || !*name) return;
        Json& values = state->row["cpu_ms"];
        values[name] = values.value(name, 0.0) + ms;
    }

    void GPU(ID3D12GraphicsCommandList* commands, const char* name, bool begin)
    {
        Slot* state = Active();
        if (!state || !commands || !name || !*name || state->resolved) return;
        if (state->queryCount >= TimestampCapacity)
        {
            ++m_droppedTimestampMarks;
            return;
        }
        if (begin)
        {
            Mark mark;
            mark.name = name;
            mark.begin = state->queryCount++;
            commands->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                QueryBase(m_activeSlot) + mark.begin);
            state->marks.push_back(std::move(mark));
            return;
        }
        // Reverse search supports nested scopes and repeated scope names.
        for (auto it = state->marks.rbegin(); it != state->marks.rend(); ++it)
        {
            if (it->name != name || it->end != InvalidTimestamp) continue;
            it->end = state->queryCount++;
            commands->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                QueryBase(m_activeSlot) + it->end);
            return;
        }
        ++m_unmatchedTimestampEnds;
    }

    void Metadata(const char* name, Json value)
    {
        Slot* state = Active();
        if (state && name && *name) state->row["metadata"][name] = std::move(value);
    }

    void Resolve(ID3D12GraphicsCommandList* commands)
    {
        Slot* state = Active();
        if (!state || !commands || state->resolved) return;
        if (state->queryCount)
            commands->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                QueryBase(m_activeSlot), state->queryCount, m_readback.Get(),
                UINT64(QueryBase(m_activeSlot)) * sizeof(UINT64));
        state->resolved = true;
    }

    // Call only after the command list has been executed and this fence value
    // successfully signaled on the same queue. A value of zero is not submitted.
    void Submitted(int slot, uint64_t fenceValue)
    {
        if (!m_initialized || !ValidSlot(slot) || !fenceValue) return;
        Slot& state = m_slots[static_cast<size_t>(slot)];
        if (!state.hasFrame || !state.resolved || state.submitted) return;
        state.fenceValue = fenceValue;
        state.submitted = true;
    }

    void Collect(int slot, ID3D12Fence* fence)
    {
        if (!m_initialized || !ValidSlot(slot) || !fence) return;
        Slot& state = m_slots[static_cast<size_t>(slot)];
        if (!state.hasFrame || !state.submitted) return;
        const UINT64 completed = fence->GetCompletedValue();
        // UINT64_MAX denotes device removal, not successful completion.
        if (completed == (std::numeric_limits<UINT64>::max)() || completed < state.fenceValue) return;

        if (state.queryCount)
        {
            const SIZE_T byteBegin = SIZE_T(QueryBase(slot)) * sizeof(UINT64);
            D3D12_RANGE range{ byteBegin, byteBegin + SIZE_T(state.queryCount) * sizeof(UINT64) };
            void* data = nullptr;
            if (SUCCEEDED(m_readback->Map(0, &range, &data)))
            {
                const UINT64* timestamps = static_cast<const UINT64*>(data) + QueryBase(slot);
                Json& values = state.row["gpu_ms"];
                for (const Mark& mark : state.marks)
                {
                    if (mark.end == InvalidTimestamp || timestamps[mark.end] < timestamps[mark.begin]) continue;
                    const double ms = double(timestamps[mark.end] - timestamps[mark.begin]) * 1000.0 / double(m_frequency);
                    values[mark.name] = values.value(mark.name, 0.0) + ms;
                }
                D3D12_RANGE written{ 0, 0 };
                m_readback->Unmap(0, &written);
            }
            else state.row["metadata"]["diagnostics_readback_failed"] = true;
        }
        m_rows.push_back(std::move(state.row));
        ++m_collectedFrames;
        state = Slot{};
        if (m_activeSlot == slot) m_activeSlot = -1;
    }

    // No fence wait here either. Pending frames retain CPU data and are labelled;
    // the owner may call Collect after its existing shutdown wait, then Flush again.
    void Flush()
    {
        if (!m_initialized) return;
        Json rows = Json::array();
        for (const Json& row : m_rows) rows.push_back(row);
        for (const Slot& state : m_slots)
        {
            if (!state.hasFrame) continue;
            Json row = state.row;
            row["metadata"][state.submitted ? "diagnostics_gpu_pending" : "diagnostics_gpu_not_submitted"] = true;
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
            return a.at("frame").get<uint64_t>() < b.at("frame").get<uint64_t>();
        });
        Json report = {
            {"schema_version", 1}, {"timestamp_frequency_hz", m_frequency},
            {"max_frames", MaximumFrames}, {"frames", std::move(rows)},
            {"statistics", {
                {"started_frames", m_startedFrames}, {"collected_frames", m_collectedFrames},
                {"skipped_busy_slots", m_skippedBusySlots}, {"abandoned_frames", m_abandonedFrames},
                {"dropped_timestamp_marks", m_droppedTimestampMarks},
                {"unmatched_timestamp_ends", m_unmatchedTimestampEnds}
            }}
        };
        try
        {
            const std::filesystem::path path(m_outputPrefix + "-frames.json");
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (output) output << report.dump(2);
        }
        catch (const std::exception&) { /* Diagnostics must not prevent shutdown. */ }
    }

private:
    static constexpr UINT InvalidTimestamp = (std::numeric_limits<UINT>::max)();
    struct Mark
    {
        std::string name;
        UINT begin = InvalidTimestamp;
        UINT end = InvalidTimestamp;
    };
    struct Slot
    {
        bool hasFrame = false;
        bool resolved = false;
        bool submitted = false;
        UINT queryCount = 0;
        UINT64 fenceValue = 0;
        Json row;
        std::vector<Mark> marks;
    };
    bool ValidSlot(int slot) const { return slot >= 0 && static_cast<size_t>(slot) < m_slots.size(); }
    UINT QueryBase(int slot) const { return static_cast<UINT>(slot) * TimestampCapacity; }
    Slot* Active()
    {
        return m_initialized && ValidSlot(m_activeSlot) && m_slots[static_cast<size_t>(m_activeSlot)].hasFrame
            ? &m_slots[static_cast<size_t>(m_activeSlot)] : nullptr;
    }

    bool m_initialized = false;
    int m_activeSlot = -1;
    UINT64 m_frequency = 0;
    size_t m_startedFrames = 0;
    size_t m_collectedFrames = 0;
    size_t m_skippedBusySlots = 0;
    size_t m_abandonedFrames = 0;
    size_t m_droppedTimestampMarks = 0;
    size_t m_unmatchedTimestampEnds = 0;
    std::string m_outputPrefix;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
    std::vector<Slot> m_slots;
    std::vector<Json> m_rows;
};
