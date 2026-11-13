// AeonBrowser -- TabGroupManager.cpp
// DelgadoLogic | Lead UI Engineer

#include "TabGroupManager.h"
#include <algorithm>

namespace BrowserChrome {

TabGroupManager::TabGroupManager()
    : m_nextGroupId(1)
{
}

TabGroupManager& TabGroupManager::GetInstance() {
    static TabGroupManager s_instance;
    return s_instance;
}

uint32_t TabGroupManager::CreateGroup(const std::string& name, COLORREF color) {
    uint32_t gid = m_nextGroupId++;
    TabGroup group;
    group.id = gid;
    group.name = name.empty() ? ("Group " + std::to_string(gid)) : name;
    group.color = color;
    group.collapsed = false;
    group.headerRect = { 0, 0, 0, 0 };
    group.chevronRect = { 0, 0, 0, 0 };
    m_groups.push_back(group);
    return gid;
}

void TabGroupManager::RestoreGroup(uint32_t id, const std::string& name, COLORREF color, bool collapsed) {
    if (id == 0) return;
    TabGroup* existing = GetGroupMut(id);
    if (existing) {
        existing->name = name.empty() ? ("Group " + std::to_string(id)) : name;
        existing->color = color;
        existing->collapsed = collapsed;
    } else {
        TabGroup group;
        group.id = id;
        group.name = name.empty() ? ("Group " + std::to_string(id)) : name;
        group.color = color;
        group.collapsed = collapsed;
        group.headerRect = { 0, 0, 0, 0 };
        group.chevronRect = { 0, 0, 0, 0 };
        m_groups.push_back(group);
    }
    if (id >= m_nextGroupId) {
        m_nextGroupId = id + 1;
    }
}

bool TabGroupManager::DeleteGroup(uint32_t groupId) {
    if (groupId == 0) return false;

    // Remove tab mappings
    auto itTab = m_tabToGroup.begin();
    while (itTab != m_tabToGroup.end()) {
        if (itTab->second == groupId) {
            itTab = m_tabToGroup.erase(itTab);
        } else {
            ++itTab;
        }
    }

    // Remove group record
    auto it = std::find_if(m_groups.begin(), m_groups.end(),
        [groupId](const TabGroup& g) { return g.id == groupId; });
    if (it != m_groups.end()) {
        m_groups.erase(it);
        return true;
    }
    return false;
}

bool TabGroupManager::RenameGroup(uint32_t groupId, const std::string& name) {
    TabGroup* g = GetGroupMut(groupId);
    if (!g) return false;
    g->name = name.empty() ? ("Group " + std::to_string(groupId)) : name;
    return true;
}

bool TabGroupManager::SetGroupColor(uint32_t groupId, COLORREF color) {
    TabGroup* g = GetGroupMut(groupId);
    if (!g) return false;
    g->color = color;
    return true;
}

bool TabGroupManager::SetGroupCollapsed(uint32_t groupId, bool collapsed) {
    TabGroup* g = GetGroupMut(groupId);
    if (!g) return false;
    g->collapsed = collapsed;
    return true;
}

bool TabGroupManager::ToggleGroupCollapsed(uint32_t groupId) {
    TabGroup* g = GetGroupMut(groupId);
    if (!g) return false;
    g->collapsed = !g->collapsed;
    return true;
}

bool TabGroupManager::IsGroupCollapsed(uint32_t groupId) const {
    const TabGroup* g = GetGroup(groupId);
    return g ? g->collapsed : false;
}

const TabGroup* TabGroupManager::GetGroup(uint32_t groupId) const {
    for (const auto& g : m_groups) {
        if (g.id == groupId) return &g;
    }
    return nullptr;
}

TabGroup* TabGroupManager::GetGroupMut(uint32_t groupId) {
    for (auto& g : m_groups) {
        if (g.id == groupId) return &g;
    }
    return nullptr;
}

std::vector<TabGroup> TabGroupManager::GetAllGroups() const {
    return m_groups;
}

size_t TabGroupManager::GetGroupCount() const {
    return m_groups.size();
}

void TabGroupManager::AddTabToGroup(unsigned int tabId, uint32_t groupId) {
    if (tabId == 0) return;
    if (groupId == 0) {
        RemoveTabFromGroup(tabId);
        return;
    }
    if (GetGroup(groupId) != nullptr) {
        m_tabToGroup[tabId] = groupId;
    }
}

void TabGroupManager::RemoveTabFromGroup(unsigned int tabId) {
    m_tabToGroup.erase(tabId);
}

uint32_t TabGroupManager::GetTabGroupId(unsigned int tabId) const {
    auto it = m_tabToGroup.find(tabId);
    if (it != m_tabToGroup.end()) {
        return it->second;
    }
    return 0;
}

std::vector<unsigned int> TabGroupManager::GetTabsInGroup(uint32_t groupId) const {
    std::vector<unsigned int> tabs;
    for (const auto& kv : m_tabToGroup) {
        if (kv.second == groupId) {
            tabs.push_back(kv.first);
        }
    }
    return tabs;
}

size_t TabGroupManager::GetTabCountInGroup(uint32_t groupId) const {
    size_t count = 0;
    for (const auto& kv : m_tabToGroup) {
        if (kv.second == groupId) {
            count++;
        }
    }
    return count;
}

void TabGroupManager::Clear() {
    m_groups.clear();
    m_tabToGroup.clear();
    m_nextGroupId = 1;
}

} // namespace BrowserChrome