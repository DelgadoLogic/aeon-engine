// AeonBrowser — AeonVersion.h
// DelgadoLogic | Build Infrastructure
//
// PURPOSE: Single source of truth for browser version strings.
// Included by any module that needs to report the version (agent pipe,
// telemetry, updater, crash handler, about:aeon page).
//
// BUMPING RULES:
//   MAJOR — breaking ABI change (engine interface version bump)
//   MINOR — session number (tracks development progress)
//   PATCH — hotfix within a session

#pragma once

#define AEON_VERSION       "0.19.0"
#define AEON_VERSION_MAJOR 0
#define AEON_VERSION_MINOR 19
#define AEON_VERSION_PATCH 0
