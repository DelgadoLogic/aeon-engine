# cmake/build_router.cmake
# DelgadoLogic | Aeon Browser
# Ensures aeon_router.dll is built without cargo file lock conflicts during MSBuild custom target execution.

if(NOT CMAKE_SOURCE_DIR)
    get_filename_component(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(ROUTER_DLL "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll")
set(ROUTER_IMPLIB "${CMAKE_SOURCE_DIR}/router/target/release/aeon_router.dll.lib")

if(EXISTS "${ROUTER_DLL}")
    message(STATUS "Rust protocol router aeon_router.dll already exists.")
else()
    message(STATUS "Building Rust protocol router with cargo...")
    find_program(CARGO_BIN cargo)
    if(NOT CARGO_BIN OR CARGO_BIN MATCHES "NOTFOUND")
        set(CARGO_BIN "cargo.exe")
    endif()
    execute_process(
        COMMAND "${CARGO_BIN}" build --release
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/router"
        RESULT_VARIABLE CARGO_RES
    )
    if(NOT CARGO_RES EQUAL 0)
        message(FATAL_ERROR "cargo build --release failed with exit code ${CARGO_RES}")
    endif()
endif()
