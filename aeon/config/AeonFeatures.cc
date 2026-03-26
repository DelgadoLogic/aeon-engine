// AeonFeatures.cc — Feature registration + force-override implementation
// Chromium's FeatureList evaluates these at startup before chrome://flags.

#include "chrome/browser/aeon/AeonFeatures.h"
#include "base/feature_list.h"

namespace aeon {

// ─── ADOPTED FEATURES (enabled by default) ───────────────────────────────────

// DBSC — Device Bound Session Credentials
// Requires: Windows CNG/TPM. Falls back gracefully on unsupported hardware.
BASE_FEATURE(kAeonDeviceBoundSessionCredentials,
             "AeonDeviceBoundSessionCredentials",
             base::FEATURE_ENABLED_BY_DEFAULT);

// Sanitizer API — native XSS prevention
BASE_FEATURE(kAeonSanitizerAPI,
             "AeonSanitizerAPI",
             base::FEATURE_ENABLED_BY_DEFAULT);

// Scroll-driven animations
BASE_FEATURE(kAeonScrollDrivenAnimations,
             "AeonScrollDrivenAnimations",
             base::FEATURE_ENABLED_BY_DEFAULT);

// Scoped Custom Element Registries
BASE_FEATURE(kAeonScopedCustomElementRegistry,
             "AeonScopedCustomElementRegistry",
             base::FEATURE_ENABLED_BY_DEFAULT);

// PDF OCR — Tesseract local inference only
BASE_FEATURE(kAeonPdfOcr,
             "AeonPdfOcr",
             base::FEATURE_ENABLED_BY_DEFAULT);

// Side-by-side tab split (AeonWorkspace)
BASE_FEATURE(kAeonSideBySideTabs,
             "AeonSideBySideTabs",
             base::FEATURE_ENABLED_BY_DEFAULT);

// WebMCP — local MCP servers only; remote MCP is blocked at AeonShield layer
BASE_FEATURE(kAeonWebMCP,
             "AeonWebMCP",
             base::FEATURE_ENABLED_BY_DEFAULT);


// ─── STRIPPED FEATURES (disabled at compile-time — not user-toggleable) ──────

BASE_FEATURE(kAeonGeminiIntegration,
             "AeonGeminiIntegration",
             base::FEATURE_DISABLED_BY_DEFAULT);

BASE_FEATURE(kAeonAutoBrowse,
             "AeonAutoBrowse",
             base::FEATURE_DISABLED_BY_DEFAULT);

BASE_FEATURE(kAeonCloudImageGen,
             "AeonCloudImageGen",
             base::FEATURE_DISABLED_BY_DEFAULT);

BASE_FEATURE(kAeonServerSideSafeBrowsing,
             "AeonServerSideSafeBrowsing",
             base::FEATURE_DISABLED_BY_DEFAULT);

BASE_FEATURE(kAeonGoogleSync,
             "AeonGoogleSync",
             base::FEATURE_DISABLED_BY_DEFAULT);


// ─── Startup override: force-apply before FeatureList processes flags ─────────
void RegisterAeonFeatureOverrides(base::FeatureList* feature_list) {
  if (!feature_list) return;

  // Adopt: ensure these are on regardless of field trial configs
  const base::Feature* enable_list[] = {
      &kAeonDeviceBoundSessionCredentials,
      &kAeonSanitizerAPI,
      &kAeonScrollDrivenAnimations,
      &kAeonScopedCustomElementRegistry,
      &kAeonPdfOcr,
      &kAeonSideBySideTabs,
      &kAeonWebMCP,
  };
  for (const auto* f : enable_list)
    feature_list->RegisterFieldTrialOverride(
        f->name, base::FeatureList::OVERRIDE_ENABLE_FEATURE, nullptr);

  // Strip: force-disable regardless of any Finch/field trial push from Google
  const base::Feature* disable_list[] = {
      &kAeonGeminiIntegration,
      &kAeonAutoBrowse,
      &kAeonCloudImageGen,
      &kAeonServerSideSafeBrowsing,
      &kAeonGoogleSync,
  };
  for (const auto* f : disable_list)
    feature_list->RegisterFieldTrialOverride(
        f->name, base::FeatureList::OVERRIDE_DISABLE_FEATURE, nullptr);
}

} // namespace aeon
