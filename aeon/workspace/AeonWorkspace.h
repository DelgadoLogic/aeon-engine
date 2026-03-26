// AeonWorkspace.h — Split-screen tab layout engine
// Implements the Chrome Canary "side-by-side tabs" feature for Aeon,
// extended with AeonDNA session isolation (each pane is a separate profile).

#pragma once
#include <memory>
#include "chrome/browser/ui/browser.h"
#include "content/public/browser/web_contents.h"

namespace aeon {

enum class SplitOrientation { Horizontal, Vertical };
enum class SplitRatio { Half, ThirdLeft, ThirdRight };

struct SplitPane {
    std::unique_ptr<content::WebContents> contents;
    std::string label;
    bool is_isolated;  // true = separate profile (AeonDNA privacy guarantee)
};

class AeonWorkspace {
public:
    explicit AeonWorkspace(Browser* browser);
    ~AeonWorkspace();

    // ── Split actions ─────────────────────────────────────────────────────────
    // OpenSplit: creates a side-by-side view with optional profile isolation
    void OpenSplit(const GURL& url,
                   SplitOrientation orient = SplitOrientation::Horizontal,
                   SplitRatio ratio = SplitRatio::Half,
                   bool isolated_profile = false);

    // SwapPanes: flip left/right or top/bottom without reload
    void SwapPanes();

    // CollapseSplit: merge both panes back to single tab (keeps active pane)
    void CollapseSplit(bool keep_left = true);

    // ── State queries ─────────────────────────────────────────────────────────
    bool IsSplitActive() const { return split_active_; }
    SplitOrientation GetOrientation() const { return orientation_; }
    const SplitPane& GetLeftPane()  const { return *left_pane_; }
    const SplitPane& GetRightPane() const { return *right_pane_; }

    // ── Keyboard shortcut handlers ────────────────────────────────────────────
    // Ctrl+Shift+\ — toggle split
    // Ctrl+Shift+[ — focus left pane
    // Ctrl+Shift+] — focus right pane
    void HandleShortcut(int accelerator_id);

private:
    Browser* browser_;
    bool split_active_ = false;
    SplitOrientation orientation_ = SplitOrientation::Horizontal;
    SplitRatio ratio_ = SplitRatio::Half;
    std::unique_ptr<SplitPane> left_pane_;
    std::unique_ptr<SplitPane> right_pane_;

    void ApplyLayoutToView();
    void EnsureIsolation(SplitPane& pane);
};

} // namespace aeon
