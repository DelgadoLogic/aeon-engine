# SQLite3 Amalgamation — Vendor Note
# AeonBrowser / history/
# DelgadoLogic

## Required Files

This directory expects two vendored files that are NOT included in the git repository
(see .gitignore — sqlite3.c and sqlite3.h are excluded due to their large size):

    history/sqlite3.c   ~230 KB
    history/sqlite3.h   ~12 KB

## How to vendor SQLite3

Download the official SQLite3 amalgamation from:
    https://sqlite.org/download.html
    → "sqlite-amalgamation-XXXXXXX.zip"

Extract sqlite3.c and sqlite3.h into this directory (history/).

Version used during development: SQLite 3.45.x (public domain)

## Why we vendor SQLite3

- SQLite3 is public domain — no license constraints.
- Vendoring the amalgamation means zero system-level dependency.
  Aeon runs on XP/Vista/7 without needing an installed SQLite.
- Same pattern used by Firefox (places.sqlite uses vendored SQLite).
- The .gitignore excludes it so the compiler script will download it
  automatically at build time.

## Build Script (Automated)

The CMakeLists.txt includes a FetchContent step to download sqlite3.c
automatically during cmake configuration if not already present:

    find_path(SQLITE3_FOUND sqlite3.h PATHS ${CMAKE_SOURCE_DIR}/history)
    if(NOT SQLITE3_FOUND)
        message(STATUS "Downloading SQLite3 amalgamation...")
        file(DOWNLOAD
            https://sqlite.org/2024/sqlite-amalgamation-3450300.zip
            ${CMAKE_BINARY_DIR}/sqlite-amalgamation.zip
        )
        # Extract to history/
    endif()
