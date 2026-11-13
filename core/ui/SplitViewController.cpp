// AeonBrowser -- SplitViewController.cpp
// DelgadoLogic | Lead UI Engineer

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "SplitViewController.h"
#include <algorithm>
#include <cmath>

namespace BrowserChrome {

SplitViewController::SplitViewController()
    : m_enabled(false)
    , m_primaryTabIndex(-1)
    , m_secondaryTabIndex(-1)
    , m_splitRatio(DEFAULT_SPLIT_RATIO)
    , m_activePane(0)
    , m_isDragging(false)
    , m_isHovered(false)
    , m_dragStartX(0)
    , m_dragStartRatio(DEFAULT_SPLIT_RATIO)
    , m_dpiScale(1.0f)
    , m_splitterRect({ 0, 0, 0, 0 })
{
}

bool SplitViewController::Enable(int primaryTabIndex, int secondaryTabIndex, float ratio) {
    if (primaryTabIndex < 0 || secondaryTabIndex < 0 || primaryTabIndex == secondaryTabIndex) {
        return false;
    }
    m_enabled = true;
    m_primaryTabIndex = primaryTabIndex;
    m_secondaryTabIndex = secondaryTabIndex;
    SetSplitRatio(ratio);
    m_activePane = 0;
    m_isDragging = false;
    m_isHovered = false;
    return true;
}

void SplitViewController::Disable() {
    m_enabled = false;
    m_isDragging = false;
    m_isHovered = false;
    m_splitterRect = { 0, 0, 0, 0 };
}

bool SplitViewController::Toggle(int currentTabIndex, int adjacentTabIndex) {
    if (m_enabled) {
        Disable();
        return false;
    } else {
        return Enable(currentTabIndex, adjacentTabIndex, m_splitRatio);
    }
}

void SplitViewController::SetSplitRatio(float ratio) {
    m_splitRatio = (std::max)(MIN_SPLIT_RATIO, (std::min)(MAX_SPLIT_RATIO, ratio));
}

void SplitViewController::SwapPanes() {
    if (!m_enabled) return;
    std::swap(m_primaryTabIndex, m_secondaryTabIndex);
}

void SplitViewController::SetPaneTab(int pane, int tabIndex) {
    if (!m_enabled || tabIndex < 0) return;
    if (pane == 0) {
        m_primaryTabIndex = tabIndex;
    } else {
        m_secondaryTabIndex = tabIndex;
    }
}

void SplitViewController::CalculateLayout(int clientW, int clientH, int topH,
                                          RECT& outPrimaryRect, RECT& outSecondaryRect, RECT& outSplitterRect,
                                          int leftOffset, float dpiScale) {
    m_dpiScale = (dpiScale > 0.5f) ? dpiScale : 1.0f;
    int clampedLeft = (std::max)(0, leftOffset);
    int availW = (std::max)(0, clientW - clampedLeft);
    int contentH = (std::max)(0, clientH - topH);
    if (!m_enabled || availW <= 0 || contentH <= 0) {
        outPrimaryRect = { clampedLeft, topH, clientW, clientH };
        outSecondaryRect = { 0, 0, 0, 0 };
        outSplitterRect = { 0, 0, 0, 0 };
        m_splitterRect = outSplitterRect;
        return;
    }

    int gutterW = (m_dpiScale > 1.01f)
        ? (std::max)(SPLITTER_WIDTH, static_cast<int>(std::lround(SPLITTER_WIDTH * m_dpiScale)))
        : SPLITTER_WIDTH;
    // Keep even gutter width so left/right halves stay symmetric around splitX
    if (gutterW % 2 != 0) gutterW += 1;
    int halfGutter = gutterW / 2;

    int splitX = clampedLeft + static_cast<int>(availW * m_splitRatio);

    int leftRight = (std::max)(clampedLeft, splitX - halfGutter);
    int rightLeft = (std::min)(clientW, splitX + halfGutter);

    outPrimaryRect   = { clampedLeft, topH, leftRight, clientH };
    outSecondaryRect = { rightLeft, topH, clientW, clientH };
    outSplitterRect  = { leftRight, topH, rightLeft, clientH };
    m_splitterRect   = outSplitterRect;
}

bool SplitViewController::HitTestSplitter(int x, int y) const {
    if (!m_enabled) return false;
    int pad = (std::max)(2, static_cast<int>(std::lround(3.0f * m_dpiScale)));
    return (x >= m_splitterRect.left - pad && x <= m_splitterRect.right + pad &&
            y >= m_splitterRect.top && y <= m_splitterRect.bottom);
}

int SplitViewController::HitTestPane(int x, int y, int topH, int clientW, int clientH) const {
    if (!m_enabled) return 0;
    if (y < topH || y > clientH || x < 0 || x > clientW) return -1;
    if (HitTestSplitter(x, y)) return -1;
    return (x < m_splitterRect.left) ? 0 : 1;
}

bool SplitViewController::SetHovered(bool hovered) {
    if (!m_enabled) hovered = false;
    if (m_isHovered == hovered) return false;
    m_isHovered = hovered;
    return true;
}

void SplitViewController::StartDrag(int mouseX) {
    if (!m_enabled) return;
    m_isDragging = true;
    m_dragStartX = mouseX;
    m_dragStartRatio = m_splitRatio;
}

bool SplitViewController::UpdateDrag(int mouseX, int clientW, int leftOffset) {
    int availW = clientW - (std::max)(0, leftOffset);
    if (!m_isDragging || availW <= 0) return false;
    float newRatio = static_cast<float>(mouseX - (std::max)(0, leftOffset)) / static_cast<float>(availW);

    // Magnetic snap to ergonomic presets (30/70, 40/60, 50/50, 60/40, 70/30)
    static constexpr float kPresets[] = { 0.30f, 0.40f, 0.50f, 0.60f, 0.70f };
    for (float p : kPresets) {
        if (std::fabs(newRatio - p) < 0.012f) {
            newRatio = p;
            break;
        }
    }

    float oldRatio = m_splitRatio;
    SetSplitRatio(newRatio);
    return (m_splitRatio != oldRatio);
}

void SplitViewController::EndDrag() {
    m_isDragging = false;
}

void SplitViewController::OnTabClosed(int closedIndex, int remainingTabCount) {
    if (!m_enabled) return;

    if (remainingTabCount < 2) {
        Disable();
        return;
    }

    if (closedIndex == m_primaryTabIndex || closedIndex == m_secondaryTabIndex) {
        Disable();
    } else {
        if (closedIndex < m_primaryTabIndex) {
            m_primaryTabIndex--;
        }
        if (closedIndex < m_secondaryTabIndex) {
            m_secondaryTabIndex--;
        }
    }
}

void SplitViewController::OnTabInserted(int insertedIndex) {
    if (!m_enabled) return;
    if (insertedIndex <= m_primaryTabIndex) {
        m_primaryTabIndex++;
    }
    if (insertedIndex <= m_secondaryTabIndex) {
        m_secondaryTabIndex++;
    }
}

} // namespace BrowserChrome
