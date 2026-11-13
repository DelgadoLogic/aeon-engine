// AeonBrowser -- test_browser_chrome_m2.cpp
// DelgadoLogic | Quality Assurance & Test Engineering
//
// Comprehensive Unit & Integration Test Suite for Milestone 2:
// - Visual Tab Groups (TabGroupManager)
// - In-Viewport Split View (SplitViewController)
// - Tab Sleep Manager / Memory Saver (TabSleepManager)
// - Chrome M2 Layout Metrics & Hit-Testing

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <algorithm>

#include "../ui/TabGroupManager.h"
#include "../ui/SplitViewController.h"
#include "../ui/BrowserChrome.h"
#include "../memory/TabSleepManager.h"
#include "../session/SessionManager.h"
#include <fstream>
#include <map>

static int g_passCount = 0;
static int g_failCount = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            std::cout << "  [PASS] " << msg << std::endl; \
            g_passCount++; \
        } else { \
            std::cerr << "  [FAIL] " << msg << " (" << #cond << " at line " << __LINE__ << ")" << std::endl; \
            g_failCount++; \
        } \
    } while (0)

void RunTabGroupManagerTests() {
    std::cout << "\n==========================================" << std::endl;
    std::cout << "Running Suite 1: TabGroupManager Tests..." << std::endl;
    std::cout << "==========================================" << std::endl;

    auto& gm = BrowserChrome::TabGroupManager::GetInstance();
    gm.Clear();
    TEST_ASSERT(gm.GetGroupCount() == 0, "Initial TabGroupManager is empty");

    uint32_t gWork = gm.CreateGroup("Work", BrowserChrome::GROUP_COLOR_PURPLE);
    TEST_ASSERT(gWork > 0, "Created Work tab group with non-zero ID");
    TEST_ASSERT(gm.GetGroupCount() == 1, "Group count is 1");

    const BrowserChrome::TabGroup* pWorkGroup = gm.GetGroup(gWork);
    TEST_ASSERT(pWorkGroup != nullptr, "Work group found");
    if (pWorkGroup) {
        TEST_ASSERT(pWorkGroup->name == "Work", "Group name matches 'Work'");
        TEST_ASSERT(pWorkGroup->color == BrowserChrome::GROUP_COLOR_PURPLE, "Group color is Purple");
        TEST_ASSERT(!pWorkGroup->collapsed, "Group is initially expanded");
    }

    uint32_t gResearch = gm.CreateGroup("Research", BrowserChrome::GROUP_COLOR_CYAN);
    TEST_ASSERT(gResearch > 0 && gResearch != gWork, "Created distinct Research group ID");
    TEST_ASSERT(gm.GetGroupCount() == 2, "Group count is 2");

    gm.AddTabToGroup(101, gWork);
    gm.AddTabToGroup(102, gWork);
    gm.AddTabToGroup(201, gResearch);

    TEST_ASSERT(gm.GetTabGroupId(101) == gWork, "Tab 101 belongs to Work group");
    TEST_ASSERT(gm.GetTabGroupId(102) == gWork, "Tab 102 belongs to Work group");
    TEST_ASSERT(gm.GetTabGroupId(201) == gResearch, "Tab 201 belongs to Research group");
    TEST_ASSERT(gm.GetTabGroupId(999) == 0, "Ungrouped tab 999 returns 0");

    auto workTabs = gm.GetTabsInGroup(gWork);
    TEST_ASSERT(workTabs.size() == 2, "Work group contains exactly 2 tabs");
    TEST_ASSERT(std::find(workTabs.begin(), workTabs.end(), 101) != workTabs.end(), "Work contains Tab 101");
    TEST_ASSERT(std::find(workTabs.begin(), workTabs.end(), 102) != workTabs.end(), "Work contains Tab 102");

    TEST_ASSERT(!gm.IsGroupCollapsed(gWork), "Work group is initially not collapsed");
    bool toggled = gm.ToggleGroupCollapsed(gWork);
    TEST_ASSERT(toggled == true, "Work group toggled to collapsed");
    TEST_ASSERT(gm.IsGroupCollapsed(gWork), "Work group is now collapsed");

    gm.SetGroupCollapsed(gWork, false);
    TEST_ASSERT(!gm.IsGroupCollapsed(gWork), "Work group explicitly set to expanded");

    TEST_ASSERT(gm.RenameGroup(gWork, "Delgado Enterprise"), "Renamed Work group to Delgado Enterprise");
    const auto* pRenamedGroup = gm.GetGroup(gWork);
    TEST_ASSERT(pRenamedGroup != nullptr && pRenamedGroup->name == "Delgado Enterprise", "Group name updated");

    TEST_ASSERT(gm.SetGroupColor(gWork, BrowserChrome::GROUP_COLOR_GREEN), "Updated color to Green");
    const auto* pColorGroup = gm.GetGroup(gWork);
    TEST_ASSERT(pColorGroup != nullptr && pColorGroup->color == BrowserChrome::GROUP_COLOR_GREEN, "Group color updated");

    gm.RemoveTabFromGroup(101);
    TEST_ASSERT(gm.GetTabGroupId(101) == 0, "Tab 101 has groupId 0 after removal");
    workTabs = gm.GetTabsInGroup(gWork);
    TEST_ASSERT(workTabs.size() == 1 && workTabs[0] == 102, "Work group now only contains Tab 102");

    TEST_ASSERT(gm.DeleteGroup(gWork), "Deleted Work group");
    TEST_ASSERT(gm.GetGroup(gWork) == nullptr, "Deleted group lookup returns nullptr");
    TEST_ASSERT(gm.GetTabGroupId(102) == 0, "Tab 102 mapping cleaned up after group deletion");
    TEST_ASSERT(gm.GetGroupCount() == 1, "Remaining group count is 1");

    gm.Clear();
    TEST_ASSERT(gm.GetGroupCount() == 0, "Clear empties TabGroupManager");
}

void RunSplitViewControllerTests() {
    std::cout << "\n==========================================" << std::endl;
    std::cout << "Running Suite 2: SplitViewController Tests..." << std::endl;
    std::cout << "==========================================" << std::endl;

    BrowserChrome::SplitViewController svc;
    TEST_ASSERT(!svc.IsEnabled(), "SplitViewController initially disabled");
    TEST_ASSERT(svc.GetSplitRatio() == 0.50f, "Default split ratio is 50%");
    TEST_ASSERT(svc.GetActivePane() == 0, "Default active pane is 0 (Primary Left)");

    svc.Enable(0, 1, 0.50f);
    TEST_ASSERT(svc.IsEnabled(), "SplitViewController enabled");
    TEST_ASSERT(svc.GetPrimaryTabIndex() == 0, "Primary tab index is 0");
    TEST_ASSERT(svc.GetSecondaryTabIndex() == 1, "Secondary tab index is 1");
    TEST_ASSERT(svc.GetActiveTabIndex() == 0, "Active tab is primary index 0");

    svc.SetActivePane(1);
    TEST_ASSERT(svc.GetActivePane() == 1, "Active pane set to 1 (Secondary Right)");
    TEST_ASSERT(svc.GetActiveTabIndex() == 1, "Active tab is secondary index 1");

    int clientW = 1200;
    int clientH = 800;
    int topH = 74;
    RECT priRect = {}, secRect = {}, splitRect = {};

    svc.CalculateLayout(clientW, clientH, topH, priRect, secRect, splitRect);

    TEST_ASSERT(priRect.left == 0 && priRect.top == 74, "Primary rect top-left is (0, 74)");
    TEST_ASSERT(priRect.bottom == 800, "Primary rect bottom is 800");

    int expectedSplitX = (int)(1200 * 0.50f); // 600
    TEST_ASSERT(splitRect.left == expectedSplitX - 3 && splitRect.right == expectedSplitX + 3,
                "Splitter rect is 6px wide centered at 600px");
    TEST_ASSERT(splitRect.top == 74 && splitRect.bottom == 800, "Splitter vertical bounds span content area");

    TEST_ASSERT(priRect.right == splitRect.left, "Primary right edge equals splitter left edge");
    TEST_ASSERT(secRect.left == splitRect.right, "Secondary left edge equals splitter right edge");
    TEST_ASSERT(secRect.right == 1200 && secRect.bottom == 800, "Secondary right-bottom is (1200, 800)");

    svc.SetSplitRatio(0.10f);
    TEST_ASSERT(svc.GetSplitRatio() == 0.20f, "Split ratio clamped to minimum 0.20f (20%)");

    svc.SetSplitRatio(0.95f);
    TEST_ASSERT(svc.GetSplitRatio() == 0.80f, "Split ratio clamped to maximum 0.80f (80%)");

    svc.SetSplitRatio(0.60f);
    TEST_ASSERT(svc.GetSplitRatio() == 0.60f, "Split ratio set to 0.60f");

    svc.CalculateLayout(clientW, clientH, topH, priRect, secRect, splitRect);
    TEST_ASSERT(svc.HitTestSplitter(splitRect.left + 1, 400), "HitTestSplitter positive inside splitter bounds");
    TEST_ASSERT(!svc.HitTestSplitter(100, 400), "HitTestSplitter negative inside left pane");

    TEST_ASSERT(svc.HitTestPane(100, 400, topH, clientW, clientH) == 0, "HitTestPane returns 0 for left coordinate");
    TEST_ASSERT(svc.HitTestPane(900, 400, topH, clientW, clientH) == 1, "HitTestPane returns 1 for right coordinate");
    TEST_ASSERT(svc.HitTestPane(100, 30, topH, clientW, clientH) == -1, "HitTestPane returns -1 in top chrome area");

    svc.StartDrag(720);
    TEST_ASSERT(svc.IsDragging(), "Dragging state activated");
    bool moved = svc.UpdateDrag(600, clientW);
    TEST_ASSERT(moved, "UpdateDrag computed new ratio");
    TEST_ASSERT(svc.GetSplitRatio() == 0.50f, "Split ratio updated to 0.50f via drag");
    svc.EndDrag();
    TEST_ASSERT(!svc.IsDragging(), "Dragging state ended");

    svc.Enable(1, 3, 0.50f);
    svc.OnTabClosed(0, 3);
    TEST_ASSERT(svc.GetPrimaryTabIndex() == 0, "Primary index shifted from 1 to 0 after tab 0 closed");
    TEST_ASSERT(svc.GetSecondaryTabIndex() == 2, "Secondary index shifted from 3 to 2 after tab 0 closed");

    svc.OnTabClosed(2, 2);
    TEST_ASSERT(!svc.IsEnabled(), "Split view automatically disabled when member tab is closed");

    svc.Enable(1, 2, 0.50f);
    svc.OnTabInserted(0);
    TEST_ASSERT(svc.GetPrimaryTabIndex() == 2, "Primary index shifted from 1 to 2 after insertion at 0");
    TEST_ASSERT(svc.GetSecondaryTabIndex() == 3, "Secondary index shifted from 2 to 3 after insertion at 0");
}

void RunTabSleepManagerTests() {
    std::cout << "\n==========================================" << std::endl;
    std::cout << "Running Suite 3: TabSleepManager Tests..." << std::endl;
    std::cout << "==========================================" << std::endl;

    TabSleepManager::Initialize(0);

    TabSleepManager::RegisterTab(501, "https://example.com/docs");
    TEST_ASSERT(!TabSleepManager::IsTabSleeping(501), "Tab 501 is initially awake");

    // Pinned Tab Exemption
    TabSleepManager::SetTabPinned(501, true);
    // Audio State Exemption
    TabSleepManager::UpdateTabAudioState(501, true);
    TabSleepManager::UpdateTabAudioState(501, false);

    // Download State Exemption
    TabSleepManager::UpdateTabDownloadState(501, true);
    TabSleepManager::UpdateTabDownloadState(501, false);

    TabSleepManager::SetTabPinned(501, false);

    // State Transitions (Suspend & Wake)
    bool susp = TabSleepManager::SuspendTab(501);
    TEST_ASSERT(susp, "Tab 501 SuspendTab returned true");
    TEST_ASSERT(TabSleepManager::IsTabSleeping(501), "Tab 501 is now sleeping");

    uint64_t savedBytesWhileSleeping = TabSleepManager::GetTotalMemorySavedBytes();
    TEST_ASSERT(savedBytesWhileSleeping > 0, "Total memory saved bytes is non-zero while tab is sleeping");

    bool wake = TabSleepManager::WakeTab(501);
    TEST_ASSERT(wake, "Tab 501 WakeTab returned true");
    TEST_ASSERT(!TabSleepManager::IsTabSleeping(501), "Tab 501 woke up successfully");

    uint64_t savedBytesAwake = TabSleepManager::GetTotalMemorySavedBytes();
    TEST_ASSERT(savedBytesAwake == 0, "Total memory saved bytes is 0 after waking all tabs");

    // Wake on Focus
    TabSleepManager::SuspendTab(501);
    TEST_ASSERT(TabSleepManager::IsTabSleeping(501), "Tab 501 suspended again");
    TabSleepManager::OnTabFocused(501);
    TEST_ASSERT(!TabSleepManager::IsTabSleeping(501), "Tab 501 instantly woke upon focus event");

    TabSleepManager::UnregisterTab(501);
    TabSleepManager::Shutdown();
}

void RunChromeConstantsTests() {
    std::cout << "\n==========================================" << std::endl;
    std::cout << "Running Suite 4: Chrome Hit-Test & Menu Constants..." << std::endl;
    std::cout << "==========================================" << std::endl;

    TEST_ASSERT(BrowserChrome::HIT_SPLITVIEW == 24, "HIT_SPLITVIEW hit code is 24");
    TEST_ASSERT(BrowserChrome::HIT_GROUP_HEADER_BASE == 300, "HIT_GROUP_HEADER_BASE is 300");
    TEST_ASSERT(BrowserChrome::HIT_GROUP_CHEVRON_BASE == 400, "HIT_GROUP_CHEVRON_BASE is 400");
    TEST_ASSERT(BrowserChrome::HIT_SPLITTER == 500, "HIT_SPLITTER is 500");

    TEST_ASSERT(IDM_GROUP_NEW_TAB == 41008, "IDM_GROUP_NEW_TAB is 41008");
    TEST_ASSERT(IDM_GROUP_RENAME == 41001, "IDM_GROUP_RENAME is 41001");
    TEST_ASSERT(IDM_GROUP_UNGROUP == 41009, "IDM_GROUP_UNGROUP is 41009");
    TEST_ASSERT(IDM_GROUP_CLOSE == 41010, "IDM_GROUP_CLOSE is 41010");

    TEST_ASSERT(IDM_TAB_SPLITVIEW == 42008, "IDM_TAB_SPLITVIEW is 42008");
    TEST_ASSERT(IDM_TAB_SLEEP_NOW == 42009, "IDM_TAB_SLEEP_NOW is 42009");
}


// ===========================================================================
// Suite 5: TabGroup Session Persistence & Restoration Tests
// ===========================================================================
struct MockTab {
    unsigned int id;
    std::string url;
    std::string title;
    uint32_t groupId;
};

static std::vector<MockTab> s_mockTabs;
static int s_mockActiveIndex = 0;
static std::vector<MockTab> s_restoredTabs;
static std::map<unsigned int, uint32_t> s_restoredGroupMappings;

static int MockGetTabCount(HWND) {
    return (int)s_mockTabs.size();
}

static int MockGetActiveTabIndex(HWND) {
    return s_mockActiveIndex;
}

static bool MockGetTabInfo(HWND, int index, unsigned int* outId, char* outUrl, int urlLen, char* outTitle, int titleLen, bool* outActive) {
    if (index < 0 || index >= (int)s_mockTabs.size()) return false;
    const auto& t = s_mockTabs[index];
    if (outId) *outId = t.id;
    if (outUrl) strncpy_s(outUrl, urlLen, t.url.c_str(), _TRUNCATE);
    if (outTitle) strncpy_s(outTitle, titleLen, t.title.c_str(), _TRUNCATE);
    if (outActive) *outActive = (index == s_mockActiveIndex);
    return true;
}

static unsigned int MockCreateTab(HWND, const char* url) {
    static unsigned int s_nextMockId = 1000;
    MockTab t;
    t.id = s_nextMockId++;
    t.url = url ? url : "";
    t.title = "Restored Tab";
    t.groupId = 0;
    s_restoredTabs.push_back(t);
    return t.id;
}

static bool MockAddTabToGroup(HWND, int tabIndex, uint32_t groupId) {
    if (tabIndex >= 0 && tabIndex < (int)s_restoredTabs.size()) {
        s_restoredTabs[tabIndex].groupId = groupId;
        s_restoredGroupMappings[s_restoredTabs[tabIndex].id] = groupId;
        return true;
    }
    return false;
}

void RunTabGroupSessionPersistenceTests() {
    std::cout << "\n==========================================" << std::endl;
    std::cout << "Running Suite 5: TabGroup Session Persistence Tests..." << std::endl;
    std::cout << "==========================================" << std::endl;

    auto& groupMgr = BrowserChrome::TabGroupManager::GetInstance();
    groupMgr.Clear();

    // 1. Setup session test file path
    const char* testSessionFile = "test_tab_group_session.json";
    SessionManager::SetSessionPath(testSessionFile);
    remove(testSessionFile);

    // 2. Setup mock groups in TabGroupManager
    uint32_t gid1 = groupMgr.CreateGroup("Work Project", BrowserChrome::GROUP_COLOR_PURPLE);
    uint32_t gid2 = groupMgr.CreateGroup("Research", BrowserChrome::GROUP_COLOR_CYAN);
    groupMgr.SetGroupCollapsed(gid2, true);

    TEST_ASSERT(gid1 == 1, "First created group has id 1");
    TEST_ASSERT(gid2 == 2, "Second created group has id 2");
    TEST_ASSERT(!groupMgr.IsGroupCollapsed(gid1), "Group 1 is expanded");
    TEST_ASSERT(groupMgr.IsGroupCollapsed(gid2), "Group 2 is collapsed");

    // 3. Setup mock tabs (including internal pages to test skip logic)
    s_mockTabs.clear();
    s_mockTabs.push_back({ 101, "https://delgadologic.com", "DelgadoLogic", gid1 });
    s_mockTabs.push_back({ 102, "https://github.com/Chronolapse411", "GitHub", gid1 });
    s_mockTabs.push_back({ 103, "aeon://settings", "Settings", 0 }); // INTERNAL PAGE - MUST BE SKIPPED
    s_mockTabs.push_back({ 104, "https://news.ycombinator.com", "Hacker News", gid2 });
    s_mockTabs.push_back({ 105, "aeon://newtab", "New Tab", 0 }); // aeon://newtab - MUST BE PRESERVED
    s_mockActiveIndex = 1;

    // Associate tabs with groups in TabGroupManager
    groupMgr.AddTabToGroup(101, gid1);
    groupMgr.AddTabToGroup(102, gid1);
    groupMgr.AddTabToGroup(104, gid2);

    TEST_ASSERT(groupMgr.GetTabGroupId(101) == gid1, "Tab 101 mapped to group 1");
    TEST_ASSERT(groupMgr.GetTabGroupId(102) == gid1, "Tab 102 mapped to group 1");
    TEST_ASSERT(groupMgr.GetTabGroupId(104) == gid2, "Tab 104 mapped to group 2");
    TEST_ASSERT(groupMgr.GetTabGroupId(105) == 0, "Tab 105 is not in any group");

    // 4. Wire UI Hooks and trigger SaveAndExit()
    SessionManager::SessionUiHooks hooks = {};
    hooks.GetTabCount = MockGetTabCount;
    hooks.GetActiveTabIndex = MockGetActiveTabIndex;
    hooks.GetTabInfo = MockGetTabInfo;
    hooks.CreateTab = MockCreateTab;
    hooks.AddTabToGroup = MockAddTabToGroup;

    SessionManager::SetUiHooks(hooks);
    SessionManager::SetMainWindow((HWND)(uintptr_t)0x5E55104); // Mock non-null HWND

    SessionManager::SaveAndExit();

    // 5. Verify the serialized JSON file on disk
    std::ifstream ifs(testSessionFile);
    TEST_ASSERT(ifs.is_open(), "Session file was successfully written to disk");

    std::string jsonContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    TEST_ASSERT(!jsonContent.empty(), "Session file content is non-empty");
    TEST_ASSERT(jsonContent.find("\"groups\":") != std::string::npos, "JSON contains 'groups' key");
    TEST_ASSERT(jsonContent.find("\"Work Project\"") != std::string::npos, "JSON contains 'Work Project' group name");
    TEST_ASSERT(jsonContent.find("\"Research\"") != std::string::npos, "JSON contains 'Research' group name");
    TEST_ASSERT(jsonContent.find("\"color_hex\":") != std::string::npos, "JSON contains 'color_hex' attribute");
    TEST_ASSERT(jsonContent.find("\"collapsed\":true") != std::string::npos, "JSON records collapsed:true for Group 2");
    TEST_ASSERT(jsonContent.find("delgadologic.com") != std::string::npos, "JSON contains delgadologic.com tab");
    TEST_ASSERT(jsonContent.find("\"aeon://newtab\"") != std::string::npos, "JSON preserves aeon://newtab");
    TEST_ASSERT(jsonContent.find("\"aeon://settings\"") == std::string::npos, "JSON skipped internal page aeon://settings");

    // 6. Test Restoration: Clear TabGroupManager and mock state
    groupMgr.Clear();
    TEST_ASSERT(groupMgr.GetGroupCount() == 0, "TabGroupManager cleared before restoration");
    TEST_ASSERT(groupMgr.GetTabGroupId(101) == 0, "Tab mappings cleared before restoration");

    s_restoredTabs.clear();
    s_restoredGroupMappings.clear();

    // Restore previous session
    bool restoreOk = SessionManager::RestorePreviousSession();
    TEST_ASSERT(restoreOk, "SessionManager::RestorePreviousSession succeeded");

    // 7. Verify deserialized Tab Groups in TabGroupManager
    TEST_ASSERT(groupMgr.GetGroupCount() == 2, "Both tab groups restored into TabGroupManager");
    const auto* restGrp1 = groupMgr.GetGroup(gid1);
    const auto* restGrp2 = groupMgr.GetGroup(gid2);
    TEST_ASSERT(restGrp1 != nullptr, "Restored Group 1 exists");
    TEST_ASSERT(restGrp2 != nullptr, "Restored Group 2 exists");
    if (restGrp1) {
        TEST_ASSERT(restGrp1->name == "Work Project", "Restored Group 1 name matches 'Work Project'");
        TEST_ASSERT(restGrp1->color == BrowserChrome::GROUP_COLOR_PURPLE, "Restored Group 1 color matches Purple");
        TEST_ASSERT(!restGrp1->collapsed, "Restored Group 1 is expanded");
    }
    if (restGrp2) {
        TEST_ASSERT(restGrp2->name == "Research", "Restored Group 2 name matches 'Research'");
        TEST_ASSERT(restGrp2->color == BrowserChrome::GROUP_COLOR_CYAN, "Restored Group 2 color matches Cyan");
        TEST_ASSERT(restGrp2->collapsed, "Restored Group 2 is collapsed");
    }

    // 8. Verify restored tabs and associations
    // We saved 4 tabs: delgadologic.com (group 1), github.com (group 1), news.ycombinator.com (group 2), aeon://newtab (group 0)
    TEST_ASSERT(s_restoredTabs.size() == 4, "Exactly 4 non-internal tabs restored");
    if (s_restoredTabs.size() == 4) {
        TEST_ASSERT(s_restoredTabs[0].url == "https://delgadologic.com", "Restored tab 0 is delgadologic.com");
        TEST_ASSERT(s_restoredTabs[0].groupId == gid1, "Restored tab 0 associated with group 1");
        TEST_ASSERT(groupMgr.GetTabGroupId(s_restoredTabs[0].id) == gid1, "TabGroupManager maps tab 0 to group 1");

        TEST_ASSERT(s_restoredTabs[1].url == "https://github.com/Chronolapse411", "Restored tab 1 is GitHub");
        TEST_ASSERT(s_restoredTabs[1].groupId == gid1, "Restored tab 1 associated with group 1");
        TEST_ASSERT(groupMgr.GetTabGroupId(s_restoredTabs[1].id) == gid1, "TabGroupManager maps tab 1 to group 1");

        TEST_ASSERT(s_restoredTabs[2].url == "https://news.ycombinator.com", "Restored tab 2 is Hacker News");
        TEST_ASSERT(s_restoredTabs[2].groupId == gid2, "Restored tab 2 associated with group 2");
        TEST_ASSERT(groupMgr.GetTabGroupId(s_restoredTabs[2].id) == gid2, "TabGroupManager maps tab 2 to group 2");

        TEST_ASSERT(s_restoredTabs[3].url == "aeon://newtab", "Restored tab 3 is aeon://newtab");
        TEST_ASSERT(s_restoredTabs[3].groupId == 0, "Restored tab 3 has no group association");
        TEST_ASSERT(groupMgr.GetTabGroupId(s_restoredTabs[3].id) == 0, "TabGroupManager maps tab 3 to 0");
    }

    // Clean up
    groupMgr.Clear();
    remove(testSessionFile);
    SessionManager::SetMainWindow(nullptr);
}
int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << " AeonBrowser v0.22.0 - Milestone 2 Test Suite Runner   " << std::endl;
    std::cout << " Tab Management Power Suite Verification               " << std::endl;
    std::cout << "========================================================" << std::endl;

    RunTabGroupManagerTests();
    RunSplitViewControllerTests();
    RunTabSleepManagerTests();
    RunChromeConstantsTests();
    RunTabGroupSessionPersistenceTests();

    std::cout << "\n========================================================" << std::endl;
    std::cout << "Test Summary: " << g_passCount << " passed, " << g_failCount << " failed." << std::endl;
    std::cout << "========================================================" << std::endl;

    return (g_failCount == 0) ? 0 : 1;
}
