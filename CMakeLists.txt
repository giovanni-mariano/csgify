cmake_minimum_required(VERSION 3.20)

project(csgify VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CSGIFY_LIBALEA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libalea" CACHE PATH "Path to libalea checkout")
set(CSGIFY_VENDOR_LUA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua-5.5.0" CACHE PATH "Path to vendored Lua source tree")

find_package(OpenCASCADE 8.0.0 EXACT REQUIRED)
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

set(CSGIFY_LIBALEA_INCLUDE_DIR "${CSGIFY_LIBALEA_ROOT}/include")
set(CSGIFY_LIBALEA_CORE_ARCHIVE "${CSGIFY_LIBALEA_ROOT}/bin/libalea.a")
set(CSGIFY_LIBALEA_MCNP_ARCHIVE "${CSGIFY_LIBALEA_ROOT}/bin/libalea_mcnp.a")
foreach(required_path IN ITEMS
        "${CSGIFY_LIBALEA_INCLUDE_DIR}/alea.h"
        "${CSGIFY_LIBALEA_CORE_ARCHIVE}"
        "${CSGIFY_LIBALEA_MCNP_ARCHIVE}")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Required Alea artifact not found: ${required_path}")
    endif()
endforeach()

add_library(Alea::Core STATIC IMPORTED GLOBAL)
set_target_properties(Alea::Core PROPERTIES
    IMPORTED_LOCATION "${CSGIFY_LIBALEA_CORE_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${CSGIFY_LIBALEA_INCLUDE_DIR}"
)
add_library(Alea::MCNP STATIC IMPORTED GLOBAL)
set_target_properties(Alea::MCNP PROPERTIES
    IMPORTED_LOCATION "${CSGIFY_LIBALEA_MCNP_ARCHIVE}"
    INTERFACE_LINK_LIBRARIES "Alea::Core"
)

# Compile and link one required post-processing API at configure time. This
# catches a new header paired with stale bin/*.a archives before the main build.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${CSGIFY_LIBALEA_INCLUDE_DIR}")
set(CMAKE_REQUIRED_LIBRARIES "${CSGIFY_LIBALEA_CORE_ARCHIVE};m")
unset(CSGIFY_LIBALEA_API_COMPATIBLE CACHE)
check_c_source_compiles(
    "#include <alea.h>\nint main(void) { alea_system_t *s = alea_create(); double tr[3] = {0,0,0}; if (!s) return 1; alea_simplify_and_prune_cells(s, 0); if (alea_add_transform(s, 1, tr, 3, 0) != 0) return 2; alea_destroy(s); return 0; }"
    CSGIFY_LIBALEA_API_COMPATIBLE)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT CSGIFY_LIBALEA_API_COMPATIBLE)
    message(FATAL_ERROR
        "The Alea headers and static archives under CSGIFY_LIBALEA_ROOT are not API-compatible. "
        "Rebuild ../libalea so bin/libalea.a matches include/alea.h.")
endif()
find_library(CSGIFY_OCCT_TKOPENGL
    NAMES TKOpenGl
    PATHS "${OpenCASCADE_LIBRARY_DIR}"
    NO_DEFAULT_PATH
)

option(CSGIFY_ENABLE_LUA "Build the batch Lua runner from vendored Lua sources" ON)
option(CSGIFY_ENABLE_QT_VIEWER
       "Build the Qt 6 decision-review viewer when Qt6 Widgets is available" ON)
if(CSGIFY_ENABLE_QT_VIEWER)
    find_package(Qt6 6.4 COMPONENTS Widgets QUIET)
    if(Qt6Widgets_FOUND)
        find_package(Qt6 6.4 COMPONENTS Network QUIET)
    endif()
endif()
if(CSGIFY_ENABLE_LUA)
    set(CSGIFY_VENDOR_LUA_SRC "${CSGIFY_VENDOR_LUA_ROOT}/src")
    if(NOT EXISTS "${CSGIFY_VENDOR_LUA_SRC}/lua.h")
        message(FATAL_ERROR
            "CSGIFY_ENABLE_LUA=ON but vendored Lua was not found at "
            "${CSGIFY_VENDOR_LUA_SRC}. Set CSGIFY_VENDOR_LUA_ROOT or disable Lua.")
    endif()

    add_library(csgify_lua STATIC
        "${CSGIFY_VENDOR_LUA_SRC}/lapi.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lauxlib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lbaselib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lcode.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lcorolib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lctype.c"
        "${CSGIFY_VENDOR_LUA_SRC}/ldblib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/ldebug.c"
        "${CSGIFY_VENDOR_LUA_SRC}/ldo.c"
        "${CSGIFY_VENDOR_LUA_SRC}/ldump.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lfunc.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lgc.c"
        "${CSGIFY_VENDOR_LUA_SRC}/linit.c"
        "${CSGIFY_VENDOR_LUA_SRC}/liolib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/llex.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lmathlib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lmem.c"
        "${CSGIFY_VENDOR_LUA_SRC}/loadlib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lobject.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lopcodes.c"
        "${CSGIFY_VENDOR_LUA_SRC}/loslib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lparser.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lstate.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lstring.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lstrlib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/ltable.c"
        "${CSGIFY_VENDOR_LUA_SRC}/ltablib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/ltm.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lundump.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lutf8lib.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lvm.c"
        "${CSGIFY_VENDOR_LUA_SRC}/lzio.c"
    )
    target_include_directories(csgify_lua PUBLIC "${CSGIFY_VENDOR_LUA_SRC}")
    target_compile_definitions(csgify_lua PRIVATE LUA_USE_LINUX)
    set_target_properties(csgify_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(csgify_engine
    src/agent_protocol.cpp
    src/alea_backend.cpp
    src/alea_bridge.cpp
    src/alea_void_bridge.cpp
    src/analytic_surface.cpp
    src/approximation.cpp
    src/autonomy.cpp
    src/analyze.cpp
    src/c_api.cpp
    src/cleanup.cpp
    src/coalesce.cpp
    src/decision_pack.cpp
    src/decision_log.cpp
    src/decomposition_ledger.cpp
    src/decompose.cpp
    src/emit.cpp
    src/engine.cpp
    src/engine_internal.cpp
    src/fixed_point.cpp
    src/fitted_candidates.cpp
    src/hole_fill.cpp
    src/import.cpp
    src/lua_runner.cpp
    src/local_partition.cpp
    src/local_overlap.cpp
    src/metadata.cpp
    src/medium.cpp
    src/operation_registry.cpp
    src/project_store.cpp
    src/project_job_progress.cpp
    src/progress.cpp
    src/recipe.cpp
    src/replay.cpp
    src/report.cpp
    src/session.cpp
    src/shatter.cpp
    src/spline_fit.cpp
    src/step_export.cpp
    src/topology_analysis.cpp
    src/viewer_controller.cpp
    src/worker.cpp
)

target_include_directories(csgify_engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        ${OpenCASCADE_INCLUDE_DIR}
)
target_compile_definitions(csgify_engine PRIVATE
    CSGIFY_VERSION="${PROJECT_VERSION}")

target_link_libraries(csgify_engine
    PUBLIC
        Alea::MCNP
        SQLite3::SQLite3
        ${OpenCASCADE_LIBRARIES}
        m
)
if(CSGIFY_ENABLE_LUA)
    target_compile_definitions(csgify_engine PRIVATE CSGIFY_ENABLE_LUA=1)
    target_link_libraries(csgify_engine PRIVATE csgify_lua dl)
else()
    target_compile_definitions(csgify_engine PRIVATE CSGIFY_ENABLE_LUA=0)
endif()

add_executable(csgify src/main.cpp)
target_link_libraries(csgify PRIVATE csgify_engine)
if(CSGIFY_OCCT_TKOPENGL)
    find_library(CSGIFY_OCCT_TKV3D
        NAMES TKV3d
        PATHS "${OpenCASCADE_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    find_library(CSGIFY_OCCT_TKSERVICE
        NAMES TKService
        PATHS "${OpenCASCADE_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    target_sources(csgify PRIVATE src/viewer.cpp)
    target_link_libraries(csgify PRIVATE
        "${CSGIFY_OCCT_TKOPENGL}"
        "${CSGIFY_OCCT_TKV3D}"
        "${CSGIFY_OCCT_TKSERVICE}"
        X11
    )
    target_compile_definitions(csgify PRIVATE CSGIFY_HAVE_TKOPENGL=1)
    if(CSGIFY_ENABLE_QT_VIEWER AND Qt6Widgets_FOUND)
        set_target_properties(csgify PROPERTIES AUTOMOC ON)
        target_sources(csgify PRIVATE
            src/viewer_qt.cpp
            src/qt/cad_job_controller.cpp
            src/qt/cad_job_controller.hpp
            src/qt/agent_conversation_dock.cpp
            src/qt/agent_conversation_dock.hpp
            src/qt/cad_job_worker.cpp
            src/qt/cad_job_worker.hpp
            src/qt/job_progress_dock.cpp
            src/qt/job_progress_dock.hpp
            src/qt/job_progress_model.cpp
            src/qt/job_progress_model.hpp
            src/qt/view_artifact_capture.cpp
            src/qt/view_artifact_capture.hpp)
        target_link_libraries(csgify PRIVATE Qt6::Widgets)
        if(Qt6Network_FOUND)
            target_sources(csgify PRIVATE
                src/qt/openai_agent_provider.cpp
                src/qt/openai_agent_provider.hpp)
            target_link_libraries(csgify PRIVATE Qt6::Network)
            target_compile_definitions(csgify PRIVATE
                CSGIFY_HAVE_OPENAI_PROVIDER=1)
        else()
            target_compile_definitions(csgify PRIVATE
                CSGIFY_HAVE_OPENAI_PROVIDER=0)
        endif()
        target_compile_definitions(csgify PRIVATE CSGIFY_HAVE_QT6_VIEWER=1)
        message(STATUS "csgify Qt 6 decision viewer: enabled")
    else()
        target_compile_definitions(csgify PRIVATE CSGIFY_HAVE_QT6_VIEWER=0)
        if(CSGIFY_ENABLE_QT_VIEWER)
            message(STATUS "csgify Qt 6 decision viewer: Qt6 Widgets not found; using X11 viewer")
        endif()
    endif()
else()
    target_compile_definitions(csgify PRIVATE
        CSGIFY_HAVE_TKOPENGL=0
        CSGIFY_HAVE_QT6_VIEWER=0)
endif()
target_include_directories(csgify PRIVATE "${CSGIFY_LIBALEA_ROOT}/include")
set_target_properties(csgify PROPERTIES
    BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
    INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}"
)
target_link_options(csgify PRIVATE "-Wl,--disable-new-dtags")

option(CSGIFY_BUILD_FIXTURE_GENERATOR "Build the developer tool that generates STEP test fixtures" OFF)
if(CSGIFY_BUILD_FIXTURE_GENERATOR)
    add_executable(csgify_generate_fixtures tests/fixtures/generate_fixtures.cpp)
    target_include_directories(csgify_generate_fixtures PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_generate_fixtures PRIVATE ${OpenCASCADE_LIBRARIES})
    set_target_properties(csgify_generate_fixtures PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_generate_fixtures PRIVATE "-Wl,--disable-new-dtags")
endif()

include(CTest)

set(CSGIFY_TEST_STEP
    "/home/giovanni/OCCT-8_0_0/data/step/screw.step"
    CACHE FILEPATH "STEP file used by csgify smoke tests")

if(BUILD_TESTING AND EXISTS "${CSGIFY_TEST_STEP}")
    add_test(
        NAME csgify_phase1_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/phase1_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CSGIFY_TEST_STEP}"
            "${CMAKE_CURRENT_BINARY_DIR}/test-output"
    )
endif()

if(BUILD_TESTING)
    add_executable(csgify_semantic_metadata tests/semantic_metadata.cpp)
    target_link_libraries(csgify_semantic_metadata PRIVATE csgify_engine)
    set_target_properties(csgify_semantic_metadata PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_semantic_metadata PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_semantic_metadata COMMAND csgify_semantic_metadata)

    add_executable(csgify_semantic_import tests/semantic_import.cpp)
    target_link_libraries(csgify_semantic_import PRIVATE csgify_engine)
    set_target_properties(csgify_semantic_import PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_semantic_import PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_semantic_import
        COMMAND csgify_semantic_import
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step")

    add_executable(csgify_artifact_store tests/artifact_store.cpp)
    target_link_libraries(csgify_artifact_store PRIVATE csgify_engine)
    set_target_properties(csgify_artifact_store PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_artifact_store PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_artifact_store COMMAND csgify_artifact_store)

    add_executable(csgify_alea_bridge tests/alea_bridge.cpp)
    target_link_libraries(csgify_alea_bridge PRIVATE csgify_engine)
    set_target_properties(csgify_alea_bridge PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_alea_bridge PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_alea_bridge COMMAND csgify_alea_bridge)

    add_executable(csgify_alea_void_bridge tests/alea_void_bridge.cpp)
    target_link_libraries(csgify_alea_void_bridge PRIVATE csgify_engine)
    set_target_properties(csgify_alea_void_bridge PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_alea_void_bridge PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_alea_void_bridge COMMAND csgify_alea_void_bridge)
    add_test(
        NAME csgify_alea_void_cli_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/alea_void_cli_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/alea-void-cli-output")
    add_test(
        NAME csgify_alea_handoff_cli_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/alea_handoff_cli_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/alea-handoff-cli-output")

    add_executable(csgify_medium_request tests/medium_request.cpp)
    target_include_directories(csgify_medium_request PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_medium_request PRIVATE csgify_engine)
    set_target_properties(csgify_medium_request PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_medium_request PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_medium_request COMMAND csgify_medium_request)

    add_executable(csgify_medium_network tests/medium_network.cpp)
    target_include_directories(csgify_medium_network PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_medium_network PRIVATE csgify_engine)
    set_target_properties(csgify_medium_network PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_medium_network PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_medium_network COMMAND csgify_medium_network)

    add_executable(csgify_project_database tests/project_database.cpp)
    target_link_libraries(csgify_project_database PRIVATE csgify_engine)
    set_target_properties(csgify_project_database PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_project_database PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_project_database COMMAND csgify_project_database)

    add_executable(csgify_definition_recognition_cache
        tests/definition_recognition_cache.cpp)
    target_include_directories(csgify_definition_recognition_cache PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_definition_recognition_cache PRIVATE csgify_engine)
    set_target_properties(csgify_definition_recognition_cache PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_definition_recognition_cache PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_definition_recognition_cache
        COMMAND csgify_definition_recognition_cache)

    add_test(
        NAME csgify_project_cli_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/project_cli_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/project-cli-output")

    if(CSGIFY_OCCT_TKOPENGL AND CSGIFY_ENABLE_QT_VIEWER AND Qt6Widgets_FOUND)
        add_executable(csgify_qt_cad_job_controller
            tests/qt_cad_job_controller.cpp
            src/qt/cad_job_controller.cpp
            src/qt/cad_job_controller.hpp
            src/qt/cad_job_worker.cpp
            src/qt/cad_job_worker.hpp)
        set_target_properties(csgify_qt_cad_job_controller PROPERTIES
            AUTOMOC ON
            BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
            INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
        target_include_directories(csgify_qt_cad_job_controller PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            ${OpenCASCADE_INCLUDE_DIR})
        target_link_libraries(csgify_qt_cad_job_controller PRIVATE
            csgify_engine Qt6::Core)
        target_link_options(csgify_qt_cad_job_controller PRIVATE
            "-Wl,--disable-new-dtags")
        add_test(
            NAME csgify_qt_cad_job_controller
            COMMAND "$<TARGET_FILE:csgify_qt_cad_job_controller>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/four_solids.step")
        set_tests_properties(csgify_qt_cad_job_controller
            PROPERTIES TIMEOUT 30)

        add_executable(csgify_qt_decision_widget
            tests/qt_decision_widget.cpp
            src/viewer_qt.cpp
            src/qt/cad_job_controller.cpp
            src/qt/cad_job_controller.hpp
            src/qt/agent_conversation_dock.cpp
            src/qt/agent_conversation_dock.hpp
            src/qt/cad_job_worker.cpp
            src/qt/cad_job_worker.hpp
            src/qt/job_progress_dock.cpp
            src/qt/job_progress_dock.hpp
            src/qt/job_progress_model.cpp
            src/qt/job_progress_model.hpp
            src/qt/view_artifact_capture.cpp
            src/qt/view_artifact_capture.hpp)
        set_target_properties(csgify_qt_decision_widget PROPERTIES
            AUTOMOC ON
            BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
            INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
        target_include_directories(csgify_qt_decision_widget PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            ${OpenCASCADE_INCLUDE_DIR})
        target_compile_definitions(csgify_qt_decision_widget PRIVATE
            CSGIFY_HAVE_QT6_VIEWER=1
            CSGIFY_HAVE_OPENAI_PROVIDER=0)
        target_link_libraries(csgify_qt_decision_widget PRIVATE
            csgify_engine Qt6::Widgets
            "${CSGIFY_OCCT_TKOPENGL}"
            "${CSGIFY_OCCT_TKV3D}"
            "${CSGIFY_OCCT_TKSERVICE}"
            X11)
        target_link_options(csgify_qt_decision_widget PRIVATE
            "-Wl,--disable-new-dtags")
        add_test(
            NAME csgify_qt_decision_widget
            COMMAND "$<TARGET_FILE:csgify_qt_decision_widget>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/l_shape.step")
        set_tests_properties(csgify_qt_decision_widget PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 60)

        add_executable(csgify_qt_agent_conversation_dock
            tests/qt_agent_conversation_dock.cpp
            src/qt/agent_conversation_dock.cpp
            src/qt/agent_conversation_dock.hpp)
        set_target_properties(csgify_qt_agent_conversation_dock PROPERTIES
            AUTOMOC ON
            BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
            INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
        target_include_directories(csgify_qt_agent_conversation_dock PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            ${OpenCASCADE_INCLUDE_DIR})
        target_link_libraries(csgify_qt_agent_conversation_dock PRIVATE
            csgify_engine Qt6::Widgets)
        target_link_options(csgify_qt_agent_conversation_dock PRIVATE
            "-Wl,--disable-new-dtags")
        add_test(
            NAME csgify_qt_agent_conversation_dock
            COMMAND "$<TARGET_FILE:csgify_qt_agent_conversation_dock>")
        set_tests_properties(csgify_qt_agent_conversation_dock PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 30)

        if(Qt6Network_FOUND)
            add_executable(csgify_openai_agent_provider
                tests/openai_agent_provider.cpp
                src/qt/openai_agent_provider.cpp
                src/qt/openai_agent_provider.hpp)
            set_target_properties(csgify_openai_agent_provider PROPERTIES
                AUTOMOC ON
                BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
                INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
            target_include_directories(csgify_openai_agent_provider PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/src"
                ${OpenCASCADE_INCLUDE_DIR})
            target_link_libraries(csgify_openai_agent_provider PRIVATE
                csgify_engine Qt6::Core Qt6::Network)
            target_link_options(csgify_openai_agent_provider PRIVATE
                "-Wl,--disable-new-dtags")
            add_test(NAME csgify_openai_agent_provider
                COMMAND "$<TARGET_FILE:csgify_openai_agent_provider>")
            set_tests_properties(csgify_openai_agent_provider PROPERTIES
                TIMEOUT 30)
        endif()

        add_test(
            NAME csgify_qt_project_launcher
            COMMAND "$<TARGET_FILE:csgify>" project review --smoke-test)
        set_tests_properties(csgify_qt_project_launcher PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 30)

        add_test(
            NAME csgify_phase9_workspace_qualification
            COMMAND bash
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/phase9_workspace_qualification.sh"
                "$<TARGET_FILE:csgify>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                "${CMAKE_CURRENT_BINARY_DIR}/phase9-workspace-output")
        set_tests_properties(csgify_phase9_workspace_qualification
            PROPERTIES TIMEOUT 60)
    endif()

    add_executable(csgify_worker_process tests/worker_process.cpp)
    target_link_libraries(csgify_worker_process PRIVATE csgify_engine)
    set_target_properties(csgify_worker_process PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_worker_process PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_worker_process COMMAND csgify_worker_process)

    add_executable(csgify_spatial_index tests/spatial_index.cpp)
    target_link_libraries(csgify_spatial_index PRIVATE csgify_engine)
    set_target_properties(csgify_spatial_index PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_spatial_index PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_spatial_index COMMAND csgify_spatial_index)

    add_executable(csgify_spatial_overlap_prescreen
        tests/spatial_overlap_prescreen.cpp)
    target_include_directories(csgify_spatial_overlap_prescreen PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_spatial_overlap_prescreen PRIVATE csgify_engine)
    set_target_properties(csgify_spatial_overlap_prescreen PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_spatial_overlap_prescreen PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_spatial_overlap_prescreen
        COMMAND csgify_spatial_overlap_prescreen)

    add_executable(csgify_decomposition_ledger
        tests/decomposition_ledger.cpp)
    target_include_directories(csgify_decomposition_ledger PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_decomposition_ledger PRIVATE csgify_engine)
    set_target_properties(csgify_decomposition_ledger PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_decomposition_ledger PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_decomposition_ledger
        COMMAND csgify_decomposition_ledger
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/l_shape.step")

    add_executable(csgify_local_partition
        tests/local_partition.cpp)
    target_include_directories(csgify_local_partition PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_local_partition PRIVATE csgify_engine)
    set_target_properties(csgify_local_partition PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_local_partition PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_local_partition COMMAND csgify_local_partition)

    add_executable(csgify_local_overlap tests/local_overlap.cpp)
    target_include_directories(csgify_local_overlap PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_local_overlap PRIVATE csgify_engine)
    set_target_properties(csgify_local_overlap PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_local_overlap PRIVATE
        "-Wl,--disable-new-dtags")
    add_test(NAME csgify_local_overlap COMMAND csgify_local_overlap)

    add_executable(csgify_session_smoke tests/session_smoke.cpp)
    target_link_libraries(csgify_session_smoke PRIVATE csgify_engine)
    set_target_properties(csgify_session_smoke PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}"
    )
    target_link_options(csgify_session_smoke PRIVATE "-Wl,--disable-new-dtags")

    add_executable(csgify_c_api_smoke tests/c_api_smoke.c)
    target_link_libraries(csgify_c_api_smoke PRIVATE csgify_engine)
    set_target_properties(csgify_c_api_smoke PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}"
    )
    target_link_options(csgify_c_api_smoke PRIVATE "-Wl,--disable-new-dtags")

    add_executable(csgify_synthetic_boolean_step
        tests/synthetic_boolean_step.cpp)
    target_include_directories(csgify_synthetic_boolean_step PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_synthetic_boolean_step PRIVATE csgify_engine)
    set_target_properties(csgify_synthetic_boolean_step PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_synthetic_boolean_step PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_geometric_certificate_property
        tests/geometric_certificate_property.cpp)
    target_include_directories(csgify_geometric_certificate_property PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_geometric_certificate_property PRIVATE
        csgify_engine)
    set_target_properties(csgify_geometric_certificate_property PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_geometric_certificate_property PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_fixed_point_progress
        tests/fixed_point_progress.cpp)
    target_link_libraries(csgify_fixed_point_progress PRIVATE csgify_engine)
    set_target_properties(csgify_fixed_point_progress PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_fixed_point_progress PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_fixed_point_callback
        tests/fixed_point_callback.cpp)
    target_link_libraries(csgify_fixed_point_callback PRIVATE csgify_engine)
    set_target_properties(csgify_fixed_point_callback PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_fixed_point_callback PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_job_progress tests/job_progress.cpp)
    target_link_libraries(csgify_job_progress PRIVATE csgify_engine)
    set_target_properties(csgify_job_progress PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_job_progress PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_shell_difference tests/shell_difference.cpp)
    target_include_directories(csgify_shell_difference PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_shell_difference PRIVATE csgify_engine)
    set_target_properties(csgify_shell_difference PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_shell_difference PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_pipe_annulus tests/pipe_annulus.cpp)
    target_include_directories(csgify_pipe_annulus PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_pipe_annulus PRIVATE csgify_engine)
    set_target_properties(csgify_pipe_annulus PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_pipe_annulus PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_extrusion_recognition
        tests/extrusion_recognition.cpp)
    target_include_directories(csgify_extrusion_recognition PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_extrusion_recognition PRIVATE csgify_engine)
    set_target_properties(csgify_extrusion_recognition PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_extrusion_recognition PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_revolution_recognition
        tests/revolution_recognition.cpp)
    target_include_directories(csgify_revolution_recognition PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_revolution_recognition PRIVATE csgify_engine)
    set_target_properties(csgify_revolution_recognition PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_revolution_recognition PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_decomposed_union tests/decomposed_union.cpp)
    target_include_directories(csgify_decomposed_union PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_decomposed_union PRIVATE csgify_engine)
    set_target_properties(csgify_decomposed_union PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_decomposed_union PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_decomposition_preview
        tests/decomposition_preview.cpp)
    target_include_directories(csgify_decomposition_preview PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_decomposition_preview PRIVATE csgify_engine)
    set_target_properties(csgify_decomposition_preview PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_decomposition_preview PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_approximation_metrics
        tests/approximation_metrics.cpp)
    target_include_directories(csgify_approximation_metrics PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_approximation_metrics PRIVATE csgify_engine)
    set_target_properties(csgify_approximation_metrics PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_approximation_metrics PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_spline_fit_evidence
        tests/spline_fit_evidence.cpp)
    target_include_directories(csgify_spline_fit_evidence PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_spline_fit_evidence PRIVATE csgify_engine)
    set_target_properties(csgify_spline_fit_evidence PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_spline_fit_evidence PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_xcaf_document_lifetime
        tests/xcaf_document_lifetime.cpp)
    target_include_directories(csgify_xcaf_document_lifetime PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_xcaf_document_lifetime PRIVATE csgify_engine)
    set_target_properties(csgify_xcaf_document_lifetime PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_xcaf_document_lifetime PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_operation_registry_contract
        tests/operation_registry_contract.cpp)
    target_include_directories(csgify_operation_registry_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_operation_registry_contract PRIVATE csgify_engine)
    set_target_properties(csgify_operation_registry_contract PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_operation_registry_contract PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_autonomy_request_contract
        tests/autonomy_request_contract.cpp)
    target_include_directories(csgify_autonomy_request_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_autonomy_request_contract PRIVATE csgify_engine)
    set_target_properties(csgify_autonomy_request_contract PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_autonomy_request_contract PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_agent_protocol
        tests/agent_protocol.cpp)
    target_include_directories(csgify_agent_protocol PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_agent_protocol PRIVATE csgify_engine)
    set_target_properties(csgify_agent_protocol PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_agent_protocol PRIVATE
        "-Wl,--disable-new-dtags")

    add_executable(csgify_mcnp_geometry_safety
        tests/mcnp_geometry_safety.cpp)
    target_include_directories(csgify_mcnp_geometry_safety PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(csgify_mcnp_geometry_safety PRIVATE csgify_engine)
    set_target_properties(csgify_mcnp_geometry_safety PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(csgify_mcnp_geometry_safety PRIVATE
        "-Wl,--disable-new-dtags")

    add_test(
        NAME csgify_contract_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/contract-output"
    )

    add_test(
        NAME csgify_decision_log_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decision_log_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decision-log-output"
    )

    add_test(
        NAME csgify_decision_pack_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decision_pack_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decision-pack-output"
    )

    add_test(
        NAME csgify_hole_fill_contract_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/hole_fill_contract_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/hole-fill-output"
    )

    add_test(
        NAME csgify_recipe_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/recipe_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/recipe-output"
    )

    add_test(
        NAME csgify_replay_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/replay-output"
    )

    add_test(
        NAME csgify_step_export_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/step_export_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/step-export-output"
    )

    add_test(
        NAME csgify_decompose_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decompose_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decompose-output"
    )

    add_test(
        NAME csgify_shatter_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/shatter_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/shatter-output"
    )

    add_test(
        NAME csgify_viewer_session_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_session_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/viewer-session-output"
    )

    add_test(
        NAME csgify_decompose_apply_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decompose_apply_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decompose-apply-output"
    )

    add_test(
        NAME csgify_tilted_torus_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/tilted_torus_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/tilted-torus-output"
    )

    add_test(
        NAME csgify_help_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/help_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/help-output"
    )

    add_test(
        NAME csgify_units_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/units_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/units-output"
    )

    add_test(
        NAME csgify_explain_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/explain_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/explain-output"
    )

    add_test(
        NAME csgify_safe_all_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/safe_all_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/safe-all-output"
    )

    add_test(
        NAME csgify_propose_apply_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/propose_apply_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/propose-apply-output"
    )

    add_test(
        NAME csgify_autonomy_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/autonomy_smoke.sh"
                "$<TARGET_FILE:csgify>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                "${CMAKE_CURRENT_BINARY_DIR}/autonomy-output")

    add_test(
        NAME csgify_overlap_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/overlap_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/overlap-output"
    )

    add_test(
        NAME csgify_mutation_overlap_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/mutation_overlap_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/mutation-overlap-output"
    )

    add_test(
        NAME csgify_overlap_gate_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/overlap_gate_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/overlap-gate-output"
    )

    add_test(
        NAME csgify_recipe_replay_overlap_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/recipe_replay_overlap_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/recipe-replay-overlap-output"
    )

    add_test(
        NAME csgify_id_stability_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/id_stability_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/id-stability-output"
    )

    add_test(
        NAME csgify_session_smoke
        COMMAND
            "$<TARGET_FILE:csgify_session_smoke>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
            "${CMAKE_CURRENT_BINARY_DIR}/session_box.i"
    )

    add_test(
        NAME csgify_c_api_smoke
        COMMAND
            "$<TARGET_FILE:csgify_c_api_smoke>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
            "${CMAKE_CURRENT_BINARY_DIR}/c_api_box.i"
    )

    add_test(
        NAME csgify_synthetic_boolean_step
        COMMAND
            "$<TARGET_FILE:csgify_synthetic_boolean_step>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
            "${CMAKE_CURRENT_BINARY_DIR}/synthetic_boolean.step"
    )

    add_test(NAME csgify_shell_difference
             COMMAND csgify_shell_difference)

    add_test(NAME csgify_pipe_annulus
             COMMAND csgify_pipe_annulus)

    add_test(NAME csgify_extrusion_recognition
             COMMAND csgify_extrusion_recognition)

    add_test(NAME csgify_revolution_recognition
             COMMAND csgify_revolution_recognition)

    add_test(NAME csgify_decomposed_union
             COMMAND csgify_decomposed_union)

    add_test(NAME csgify_decomposition_preview
             COMMAND csgify_decomposition_preview
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/l_shape.step"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/overlapping_boxes.step")

    add_test(NAME csgify_approximation_metrics
             COMMAND csgify_approximation_metrics
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/l_shape.step")

    add_test(NAME csgify_spline_fit_evidence
             COMMAND csgify_spline_fit_evidence)

    add_test(NAME csgify_xcaf_document_lifetime
             COMMAND csgify_xcaf_document_lifetime
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
                     "${CMAKE_CURRENT_BINARY_DIR}/xcaf-document-lifetime.step")

    add_test(NAME csgify_operation_registry_contract
             COMMAND csgify_operation_registry_contract)

    add_test(NAME csgify_autonomy_request_contract
             COMMAND csgify_autonomy_request_contract)

    add_test(NAME csgify_agent_protocol
             COMMAND csgify_agent_protocol
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/l_shape.step")

    add_test(NAME csgify_mcnp_geometry_safety
             COMMAND csgify_mcnp_geometry_safety
                     "${CMAKE_CURRENT_BINARY_DIR}/mcnp-geometry-safety")

    add_test(NAME csgify_mcnp_domain_cli_smoke
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcnp_domain_cli_smoke.sh"
                     "$<TARGET_FILE:csgify>"
                     "$<TARGET_FILE:csgify_mcnp_geometry_safety>"
                     "${CMAKE_CURRENT_BINARY_DIR}/mcnp-domain-cli-output")

    add_test(
        NAME csgify_approximation_cli_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/approximation_cli_smoke.sh"
                "$<TARGET_FILE:csgify>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                "${CMAKE_CURRENT_BINARY_DIR}/approximation-cli-output")

    add_test(
        NAME csgify_geometric_certificate_property
        COMMAND "$<TARGET_FILE:csgify_geometric_certificate_property>"
    )

    add_test(
        NAME csgify_fixed_point_progress
        COMMAND "$<TARGET_FILE:csgify_fixed_point_progress>"
    )

    add_test(
        NAME csgify_fixed_point_callback
        COMMAND "$<TARGET_FILE:csgify_fixed_point_callback>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/l_shape.step"
    )

    add_test(
        NAME csgify_job_progress
        COMMAND "$<TARGET_FILE:csgify_job_progress>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/four_solids.step"
    )

    add_test(
        NAME csgify_analysis_progress_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_progress_smoke.sh"
                "$<TARGET_FILE:csgify>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/four_solids.step"
                "${CMAKE_CURRENT_BINARY_DIR}/analysis-progress-output"
    )

    add_test(
        NAME csgify_fixed_point_decomposition_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixed_point_decomposition_smoke.sh"
            "$<TARGET_FILE:csgify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/fixed-point-output"
    )

    if(NOT CSGIFY_ENABLE_LUA)
        add_test(
            NAME csgify_lua_not_built
            COMMAND
                "$<TARGET_FILE:csgify>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/inspect.lua"
        )
        set_tests_properties(csgify_lua_not_built PROPERTIES WILL_FAIL TRUE)
    else()
        add_test(
            NAME csgify_lua_smoke
            COMMAND
                bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_smoke.sh"
                "$<TARGET_FILE:csgify>"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/inspect.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
                "${CMAKE_CURRENT_BINARY_DIR}/lua_inspect.json"
        )
        add_test(
            NAME csgify_lua_agent_workflow
            COMMAND
                "$<TARGET_FILE:csgify>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_agent_workflow.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
                "${CMAKE_CURRENT_BINARY_DIR}/lua_agent_box.i"
                "${CMAKE_CURRENT_BINARY_DIR}/lua_agent_report.json"
        )
        add_test(
            NAME csgify_hole_fill_apply
            COMMAND
                "$<TARGET_FILE:csgify>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hole_fill_apply.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bolt_hole.step"
                "${CMAKE_CURRENT_BINARY_DIR}/hole_fill_apply.i"
        )
        add_test(
            NAME csgify_surface_replacement_apply
            COMMAND
                "$<TARGET_FILE:csgify>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/surface_replacement_apply.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/spline_face.step"
                "${CMAKE_CURRENT_BINARY_DIR}/surface_replacement_apply.i"
        )
        add_test(
            NAME csgify_lua_api_smoke
            COMMAND
                bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_api_smoke.sh"
                "$<TARGET_FILE:csgify>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_api.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                "${CMAKE_CURRENT_BINARY_DIR}/lua-api-output"
        )
        add_test(
            NAME csgify_lua_coverage_smoke
            COMMAND
                bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_coverage_smoke.sh"
                "$<TARGET_FILE:csgify>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_coverage.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                "${CMAKE_CURRENT_BINARY_DIR}/lua-coverage-output"
        )
    endif()

    # The C++ test programs use assert() as their test oracle. Keep those
    # checks active even when the surrounding build is optimized; otherwise a
    # Release ctest run can skip its assertions and continue past preconditions.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        get_property(csgify_directory_targets DIRECTORY
                     PROPERTY BUILDSYSTEM_TARGETS)
        foreach(csgify_target IN LISTS csgify_directory_targets)
            if(csgify_target MATCHES "^csgify_"
               AND NOT csgify_target STREQUAL "csgify_engine"
               AND NOT csgify_target STREQUAL "csgify_lua")
                target_compile_options(${csgify_target} PRIVATE
                    $<$<COMPILE_LANGUAGE:C,CXX>:-UNDEBUG>)
            endif()
        endforeach()
    endif()
endif()
