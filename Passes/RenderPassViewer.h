#pragma once

#include "RenderPassView.h"
#include "RenderPassPreview.h"

// One selected logical result per frame. Copies are inserted at the producer's
// boundary, before later in-place writes. Normal rendering/history continues.
class RenderPassViewer
{
public:
    using Mapping = RenderPassPreview::Mapping;
    RenderPassViewer(const RenderPassView& view, RenderPassPreview& preview,
        ID3D12DescriptorHeap* heap, UINT width, UINT height, bool captureDefault = false)
        : m_view(view), m_preview(preview), m_heap(heap), m_width(width), m_height(height),
          m_captureDefault(captureDefault) {}

    bool Active() const { return m_view.pass != RenderPassId::Default; }
    bool NeedsResult() const { return Active() || m_captureDefault; }
    bool Wants(RenderPassId pass) const { return Active() && m_view.pass == pass; }

    void Output(RDGBuilder& graph, RenderPassId pass, RDGTextureHandle texture,
        Mapping mapping = Mapping::HDR, UINT channel = 0, UINT slice = UINT_MAX)
    {
        if (Wants(pass) && m_view.result == RenderPassResult::PassOutput)
            Capture(graph, texture, mapping, channel, slice);
    }

    void Accumulated(RDGBuilder& graph, RenderPassId pass, RDGTextureHandle texture,
        Mapping mapping = Mapping::HDR)
    {
        if (Wants(pass) && m_view.result == RenderPassResult::Accumulated)
            Capture(graph, texture, mapping);
    }

    void Color(RDGBuilder& graph, RenderPassId pass, RDGTextureHandle texture,
        Mapping mapping = Mapping::HDR)
    {
        if (Wants(pass)) Capture(graph, texture, mapping);
    }

    void Before(RDGBuilder& graph, RenderPassId pass, RDGTextureHandle texture)
    {
        if (Wants(pass) && m_view.result == RenderPassResult::PassOutput)
            m_before = RenderPassPreview::Snapshot(graph, texture, "PassView.Before").texture;
    }

    void After(RDGBuilder& graph, RenderPassId pass, RDGTextureHandle texture)
    {
        if (!Wants(pass)) return;
        if (m_view.result == RenderPassResult::Accumulated)
            Capture(graph, texture, Mapping::HDR);
        else if (m_before.IsValid())
        {
            m_source.texture = m_preview.Difference(graph, m_heap, texture, m_before, "PassView.Change").texture;
            m_source.mapping = Mapping::DeltaHDR;
        }
    }

    bool Finish(RDGBuilder& graph, RDGTextureHandle backBuffer)
    {
        if (!NeedsResult()) return true;
        if (!Active()) Capture(graph, backBuffer, Mapping::LDR);
        if (!m_source.texture.IsValid())
        {
            ErrorLog::Write("Pass viewer: the selected stage did not register a result.");
            return false;
        }
        m_output = m_preview.AddToGraph(graph, m_heap, m_source, m_width, m_height);
        return m_output.texture.IsValid() && (!Active() ||
            m_preview.PresentToGraph(graph, m_heap, m_output.texture, backBuffer, m_width, m_height).IsValid());
    }

    const RenderPassPreview::Source& Source() const { return m_source; }
    RDGTextureHandle PreviewTexture() const { return m_output.texture; }

private:
    void Capture(RDGBuilder& graph, RDGTextureHandle texture, Mapping mapping,
        UINT channel = 0, UINT slice = UINT_MAX)
    {
        m_source.texture = RenderPassPreview::Snapshot(graph, texture, "PassView.Result").texture;
        m_source.mapping = mapping;
        m_source.channel = channel;
        m_source.slice = slice;
    }

    const RenderPassView& m_view;
    RenderPassPreview& m_preview;
    ID3D12DescriptorHeap* m_heap;
    UINT m_width, m_height;
    bool m_captureDefault;
    RDGTextureHandle m_before;
    RenderPassPreview::Source m_source;
    RenderPassPreview::Output m_output;
};
