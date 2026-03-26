// Copyright 2026 The Aeon Browser Authors
// AeonFingerprintSeed — Header

#ifndef CHROME_BROWSER_AEON_FINGERPRINT_SEED_H_
#define CHROME_BROWSER_AEON_FINGERPRINT_SEED_H_

#include <cstddef>
#include <cstdint>
#include <string>

class AeonFingerprintSeed {
 public:
  // Called once at browser process startup
  static void Initialize();
  // Called by window.aeon.privacy.refresh() or ghost mode timer
  static void RotateSeed();

  // Accessors
  static uint64_t GetSeed();
  static std::string GetCurrentHash();
  static std::string GetMode();   // "normal" | "stealth" | "ghost"
  static void SetMode(const std::string& mode);

  // Per-API noise — used by Blink patches
  static int8_t GetCanvasNoise(size_t pixel_index);
  static float GetAudioFreqOffset(size_t index);
  static std::string GetWebGLRenderer();  // Empty = use real
  static std::string GetWebGLVendor();
  static int GetHardwareConcurrency();    // -1 = use real
  static int GetDeviceMemory();           // -1 = use real
};

#endif  // CHROME_BROWSER_AEON_FINGERPRINT_SEED_H_
