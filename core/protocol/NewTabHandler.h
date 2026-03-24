// AeonBrowser — NewTabHandler.h
#pragma once

namespace NewTabHandler {
    // Returns allocated HTML buffer for the given aeon:// URL, or nullptr.
    // Caller must free() the returned buffer.
    char* Serve(const char* aeonUrl);

    // Returns true if the URL is an aeon:// internal URL.
    bool IsAeonUrl(const char* url);
}
