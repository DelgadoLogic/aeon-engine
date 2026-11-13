// AeonBrowser -- SplitViewController.h
// DelgadoLogic | Lead UI Engineer
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace BrowserChrome {

class SplitViewController {
public:
    static constexpr float MIN_SPLIT_RATIO = 0.20f;
    static constexpr float MAX_SPLIT_RATIO = 0.80f;
    static constexpr float DEFAULT_SPLIT_RATIO = 0.50f;
    static constexpr int   SPLITTER_WIDTH = 6; // 6px draggable divider bar

    SplitViewController();
    ~SplitViewController() = default;

    // Split view activation / deactivation
    bool Enable(int primaryTabIndex, int secondaryTabIndex, float ratio = DEFAULT_SPLIT_RATIO);
    void Disable();
    bool Toggle(int currentTabIndex, int adjacentTabIndex);

    bool IsEnabled() const { return m_enabled; }
    int  GetPrimaryTabIndex() const { return m_primaryTabIndex; }
    int  GetSecondaryTabIndex() const { return m_secondaryTabIndex; }
    void SetPrimaryTabIndex(int index) { m_primaryTabIndex = index; }
    void SetSecondaryTabIndex(int index) { m_secondaryTabIndex = index; }

    // Active pane: 0 = Primary / Left, 1 = Secondary / Right
    int  GetActivePane() const { return m_activePane; }
    void SetActivePane(int pane) { m_activePane = (pane == 1) ? 1 : 0; }
    int  GetActiveTabIndex() const {
        return (m_activePane == 0) ? m_primaryTabIndex : m_secondaryTabIndex;
    }

    float GetSplitRatio() const { return m_splitRatio; }
    void  SetSplitRatio(float ratio);
    void  ResetRatio() { m_splitRatio = DEFAULT_SPLIT_RATIO; }

    // Crisp layout presets (50/50, 60/40, 70/30, 40/60, 30/70)
    void  SetPreset(float ratio) { SetSplitRatio(ratio); }
    void  SetPreset50_50() { SetSplitRatio(0.50f); }
    void  SetPreset60_40() { SetSplitRatio(0.60f); }
    void  SetPreset70_30() { SetSplitRatio(0.70f); }

    // Pane management & swapping
    void  SwapPanes();
    void  SetPaneTab(int pane, int tabIndex);

    // Calculate layout bounds for left pane, right pane, and splitter divider
    void CalculateLayout(int clientW, int clientH, int topH,
                         RECT& outPrimaryRect, RECT& outSecondaryRect, RECT& outSplitterRect,
                         int leftOffset = 0, float dpiScale = 1.0f);

    const RECT& GetSplitterRect() const { return m_splitterRect; }
    void SetSplitterRect(const RECT& r) { m_splitterRect = r; }

    // Hit-testing & hover feedback
    bool HitTestSplitter(int x, int y) const;
    int  HitTestPane(int x, int y, int topH, int clientW, int clientH) const;
    bool IsHovered() const { return m_isHovered; }
    bool SetHovered(bool hovered);

    // Drag interaction (with magnetic snap to 30/70, 40/60, 50/50, 60/40, 70/30)
    void StartDrag(int mouseX);
    bool UpdateDrag(int mouseX, int clientW, int leftOffset = 0);
    void EndDrag();
    bool IsDragging() const { return m_isDragging; }

    // Tab list mutation notifications
    void OnTabClosed(int closedIndex, int remainingTabCount);
    void OnTabInserted(int insertedIndex);

private:
    bool  m_enabled;
    int   m_primaryTabIndex;
    int   m_secondaryTabIndex;
    float m_splitRatio;
    int   m_activePane;
    bool  m_isDragging;
    bool  m_isHovered;
    int   m_dragStartX;
    float m_dragStartRatio;
    float m_dpiScale;
    RECT  m_splitterRect;
};

} // namespace BrowserChrome
