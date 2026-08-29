cmake_minimum_required(VERSION 3.20)

project(csgify VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CSGIFY_LIBALEA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libalea" CACHE PATH "Path to libalea checkout")
set(CSGIFY_VENDOR_LUA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua-5.5.0" CACHE PATH "Path to vendored Lua source tree")

find_package(OpenCASCADE 8.0.0 EXACT REQUIRED)

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
    "#include <alea.h>\nint main(void) { alea_system_t *s = alea_create(); if (!s) return 1; alea_simplify_and_prune_cells(s, 0); alea_destroy(s); return 0; }"
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
    src/alea_backend.cpp
    src/analytic_surface.cpp
    src/analyze.cpp
    src/c_api.cpp
    src/cleanup.cpp
    src/decision_pack.cpp
    src/decision_log.cpp
    src/decompose.cpp
    src/emit.cpp
    src/engine.cpp
    src/engine_internal.cpp
    src/fitted_candidates.cpp
    src/hole_fill.cpp
    src/import.cpp
    src/lua_runner.cpp
    src/operation_registry.cpp
    src/recipe.cpp
    src/replay.cpp
    src/report.cpp
    src/session.cpp
    src/shatter.cpp
    src/spline_fit.cpp
    src/step_export.cpp
    src/viewer_controller.cpp
)

target_include_directories(csgify_engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        ${OpenCASCADE_INCLUDE_DIR}
)

target_link_libraries(csgify_engine
    PUBLIC
        Alea::MCNP
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
else()
    target_compile_definitions(csgify PRIVATE CSGIFY_HAVE_TKOPENGL=0)
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

    add_test(
        NAME csgify_geometric_certificate_property
        COMMAND "$<TARGET_FILE:csgify_geometric_certificate_property>"
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
endif()
