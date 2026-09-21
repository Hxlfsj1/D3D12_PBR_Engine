#pragma once

#include <cstdint>
#include <optional>

// Shared by the outliner, inspector and GPU picker. Zero means background/no selection.
class EditorSelection
{
public:
    struct PickRequest
    {
        float u, v; // Normalized FULL client-area coordinates, not the exposed centre pane.
        uint64_t revision;
    };

    uint32_t SelectedId() const { return m_selectedId; }
    bool HasPickRequest() const { return m_request.has_value(); }

    void Select(uint32_t id)
    {
        m_selectedId = id;
        ++m_revision; // Also invalidate an older GPU click when reselecting the same row.
        m_request.reset();
    }

    void RequestPick(float u, float v)
    {
        m_request = PickRequest{ u, v, ++m_revision };
    }

    PickRequest TakePickRequest()
    {
        PickRequest request = m_request.value();
        m_request.reset();
        return request;
    }

    void ApplyPick(uint64_t revision, uint32_t id)
    {
        if (revision == m_revision)
            m_selectedId = id;
    }

private:
    uint32_t m_selectedId = 0;
    uint64_t m_revision = 0;
    std::optional<PickRequest> m_request;
};
