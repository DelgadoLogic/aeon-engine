// AeonBrowser -- TabGroupManager.h
// DelgadoLogic | Lead UI Engineer
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace BrowserChrome {

// Preset Theme Colors for Tab Groups (matching DelgadoLogic Obsidian Design System)
constexpr COLORREF GROUP_COLOR_PURPLE = RGB(147,  51, 234); // #9333ea (Electric Purple)
constexpr COLORREF GROUP_COLOR_CYAN   = RGB( 56, 189, 248); // #38bdf8 (Neon Cyan)
constexpr COLORREF GROUP_COLOR_GREEN  = RGB( 34, 197,  94); // #22c55e (Emerald Green)
constexpr COLORREF GROUP_COLOR_AMBER  = RGB(245, 158,  11); // #f59e0b (Amber Gold)
constexpr COLORREF GROUP_COLOR_PINK   = RGB(236,  72, 153); // #ec4899 (Neon Pink)
constexpr COLORREF GROUP_COLOR_RED    = RGB(239,  68,  68); // #ef4444 (Ruby Red)

struct TabGroup {
    uint32_t    id;              // Unique group identifier (>0)
    std::string name;            // Group display label (e.g. "Work", "Research", "Dev")
    COLORREF    color;           // Accent color code
    bool        collapsed;       // True if group tabs are folded/hidden
    RECT        headerRect;      // Hit-test and render bounds in tab strip
    RECT        chevronRect;     // Hit-test bounds for collapse/expand toggle
};

class TabGroupManager {
public:
    static TabGroupManager& GetInstance();

    // Group CRUD operations
    uint32_t CreateGroup(const std::string& name, COLORREF color = GROUP_COLOR_PURPLE);
    void RestoreGroup(uint32_t id, const std::string& name, COLORREF color, bool collapsed);
    bool DeleteGroup(uint32_t groupId);
    bool RenameGroup(uint32_t groupId, const std::string& name);
    bool SetGroupColor(uint32_t groupId, COLORREF color);
    bool SetGroupCollapsed(uint32_t groupId, bool collapsed);
    bool ToggleGroupCollapsed(uint32_t groupId);

    // Queries
    bool IsGroupCollapsed(uint32_t groupId) const;
    const TabGroup* GetGroup(uint32_t groupId) const;
    TabGroup* GetGroupMut(uint32_t groupId);
    std::vector<TabGroup> GetAllGroups() const;
    size_t GetGroupCount() const;

    // Tab-to-Group Associations
    void AddTabToGroup(unsigned int tabId, uint32_t groupId);
    void RemoveTabFromGroup(unsigned int tabId);
    uint32_t GetTabGroupId(unsigned int tabId) const;
    std::vector<unsigned int> GetTabsInGroup(uint32_t groupId) const;
    size_t GetTabCountInGroup(uint32_t groupId) const;

    // Reset all group state
    void Clear();

private:
    TabGroupManager();
    ~TabGroupManager() = default;
    TabGroupManager(const TabGroupManager&) = delete;
    TabGroupManager& operator=(const TabGroupManager&) = delete;

    uint32_t m_nextGroupId;
    std::vector<TabGroup> m_groups;
    std::map<unsigned int, uint32_t> m_tabToGroup; // tabId -> groupId
};

} // namespace BrowserChrome