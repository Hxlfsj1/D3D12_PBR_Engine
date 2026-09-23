#pragma once

#include "SceneObject.h"
#include "EditorSelection.h"
#include <optional>
#include <utility>

// Runtime editor history. IDs survive instance sorting; no pointers into scene vectors.
class EditorHistory
{
public:
    static constexpr size_t Capacity = 100;
    struct Transform
    {
        DirectX::XMFLOAT3 translation, rotation, scale;
        static Transform Capture(const ModelInstance& object) { return { object.translation, object.rotation, object.scale }; }
        void Apply(ModelInstance& object) const
        {
            object.SetTranslation(translation.x, translation.y, translation.z);
            object.SetRotation(rotation.x, rotation.y, rotation.z);
            object.SetScale(scale.x, scale.y, scale.z);
        }
        bool operator==(const Transform& other) const
        {
            auto same = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
            { return a.x == b.x && a.y == b.y && a.z == b.z; };
            return same(translation, other.translation) && same(rotation, other.rotation) && same(scale, other.scale);
        }
    };

    size_t Size() const { return m_entries.size(); }
    size_t UndoCount() const { return m_cursor; }
    size_t RedoCount() const { return m_entries.size() - m_cursor; }

    void RecordTransform(uint32_t id, const Transform& before, const Transform& after)
    {
        if (before == after) return;
        Entry entry{}; entry.kind = Kind::Transform; entry.id = id;
        entry.before = before; entry.after = after;
        Push(std::move(entry));
    }
    void BeginTransform(std::vector<ModelInstance>& instances, const ModelInstance& object, uint32_t widget)
    {
        if (m_pending && m_pending->widget == widget && m_pending->id == object.editorId) return;
        CommitTransform(instances);
        m_pending = Pending{ object.editorId, widget, Transform::Capture(object) };
    }
    void CommitTransform(std::vector<ModelInstance>& instances)
    {
        if (!m_pending) return;
        if (const auto* object = Find(instances, m_pending->id))
            RecordTransform(object->editorId, m_pending->before, Transform::Capture(*object));
        m_pending.reset();
    }
    void CommitTransform(std::vector<ModelInstance>& instances, uint32_t widget)
    {
        if (m_pending && m_pending->widget == widget) CommitTransform(instances);
    }
    void CommitInactiveTransform(std::vector<ModelInstance>& instances, uint32_t activeWidget)
    {
        if (m_pending && m_pending->widget != activeWidget) CommitTransform(instances);
    }
    void Rename(std::vector<ModelInstance>& instances, ModelInstance& object, const std::string& name)
    {
        if (object.name == name) return;
        CommitTransform(instances);
        Entry entry{}; entry.kind = Kind::Rename; entry.id = object.editorId;
        entry.oldName = object.name; entry.newName = name;
        object.name = name;
        Push(std::move(entry));
    }
    void RecordSelection(std::vector<ModelInstance>& instances, uint32_t before, uint32_t after)
    {
        if (before == after) return;
        CommitTransform(instances);
        Entry entry{}; entry.kind = Kind::Selection;
        entry.oldSelection = before; entry.newSelection = after;
        Push(std::move(entry));
    }
    void Select(std::vector<ModelInstance>& instances, EditorSelection& selection, uint32_t id)
    {
        RecordSelection(instances, selection.SelectedId(), id);
        selection.Select(id); // Also invalidate an outstanding pick on a no-op selection.
    }
    bool Undo(std::vector<ModelInstance>& instances, EditorSelection& selection)
    {
        CommitTransform(instances);
        selection.Select(selection.SelectedId()); // Older GPU readbacks must not undo an undo.
        while (m_cursor > 0)
            if (Apply(m_entries[--m_cursor], false, instances, selection)) return true;
        return false;
    }
    bool Redo(std::vector<ModelInstance>& instances, EditorSelection& selection)
    {
        CommitTransform(instances);
        selection.Select(selection.SelectedId());
        while (m_cursor < m_entries.size())
            if (Apply(m_entries[m_cursor++], true, instances, selection)) return true;
        return false;
    }

private:
    enum class Kind { Transform, Rename, Selection };
    struct Entry
    {
        Kind kind;
        uint32_t id = 0, oldSelection = 0, newSelection = 0;
        Transform before{}, after{};
        std::string oldName, newName;
    };
    struct Pending { uint32_t id, widget; Transform before; };
    static ModelInstance* Find(std::vector<ModelInstance>& instances, uint32_t id)
    {
        for (auto& object : instances) if (id != 0 && object.editorId == id) return &object;
        return nullptr;
    }
    void Push(Entry entry)
    {
        m_entries.resize(m_cursor);
        if (m_entries.size() == Capacity) m_entries.erase(m_entries.begin());
        m_entries.push_back(std::move(entry));
        m_cursor = m_entries.size();
    }
    static bool Apply(const Entry& entry, bool forward, std::vector<ModelInstance>& instances, EditorSelection& selection)
    {
        if (entry.kind == Kind::Selection)
        {
            const auto id = forward ? entry.newSelection : entry.oldSelection;
            selection.Select(Find(instances, id) ? id : 0);
            return true;
        }
        auto* object = Find(instances, entry.id);
        if (!object) return false;
        if (entry.kind == Kind::Transform) (forward ? entry.after : entry.before).Apply(*object);
        else object->name = forward ? entry.newName : entry.oldName;
        return true;
    }
    std::vector<Entry> m_entries;
    size_t m_cursor = 0;
    std::optional<Pending> m_pending;
};
