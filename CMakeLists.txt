cmake_minimum_required(VERSION 3.20)

project(cad2csg VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CAD2CSG_LIBALEA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libalea" CACHE PATH "Path to libalea checkout")
set(CAD2CSG_VENDOR_LUA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua-5.5.0" CACHE PATH "Path to vendored Lua source tree")

find_package(OpenCASCADE 8.0.0 EXACT REQUIRED)
find_library(CAD2CSG_OCCT_TKOPENGL
    NAMES TKOpenGl
    PATHS "${OpenCASCADE_LIBRARY_DIR}"
    NO_DEFAULT_PATH
)

option(CAD2CSG_ENABLE_LUA "Build the batch Lua runner from vendored Lua sources" ON)
if(CAD2CSG_ENABLE_LUA)
    set(CAD2CSG_VENDOR_LUA_SRC "${CAD2CSG_VENDOR_LUA_ROOT}/src")
    if(NOT EXISTS "${CAD2CSG_VENDOR_LUA_SRC}/lua.h")
        message(FATAL_ERROR
            "CAD2CSG_ENABLE_LUA=ON but vendored Lua was not found at "
            "${CAD2CSG_VENDOR_LUA_SRC}. Set CAD2CSG_VENDOR_LUA_ROOT or disable Lua.")
    endif()

    add_library(cad2csg_lua STATIC
        "${CAD2CSG_VENDOR_LUA_SRC}/lapi.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lauxlib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lbaselib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lcode.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lcorolib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lctype.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/ldblib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/ldebug.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/ldo.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/ldump.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lfunc.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lgc.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/linit.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/liolib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/llex.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lmathlib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lmem.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/loadlib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lobject.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lopcodes.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/loslib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lparser.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lstate.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lstring.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lstrlib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/ltable.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/ltablib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/ltm.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lundump.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lutf8lib.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lvm.c"
        "${CAD2CSG_VENDOR_LUA_SRC}/lzio.c"
    )
    target_include_directories(cad2csg_lua PUBLIC "${CAD2CSG_VENDOR_LUA_SRC}")
    target_compile_definitions(cad2csg_lua PRIVATE LUA_USE_LINUX)
    set_target_properties(cad2csg_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(cad2csg_engine
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
    src/recipe.cpp
    src/replay.cpp
    src/report.cpp
    src/session.cpp
    src/spline_fit.cpp
    src/step_export.cpp
)

target_include_directories(cad2csg_engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CAD2CSG_LIBALEA_ROOT}/include"
        ${OpenCASCADE_INCLUDE_DIR}
)

target_link_libraries(cad2csg_engine
    PUBLIC
        "${CAD2CSG_LIBALEA_ROOT}/bin/libalea_mcnp.a"
        "${CAD2CSG_LIBALEA_ROOT}/bin/libalea.a"
        ${OpenCASCADE_LIBRARIES}
        m
)
if(CAD2CSG_ENABLE_LUA)
    target_compile_definitions(cad2csg_engine PRIVATE CAD2CSG_ENABLE_LUA=1)
    target_link_libraries(cad2csg_engine PRIVATE cad2csg_lua dl)
else()
    target_compile_definitions(cad2csg_engine PRIVATE CAD2CSG_ENABLE_LUA=0)
endif()

add_executable(cad2csg src/main.cpp)
target_link_libraries(cad2csg PRIVATE cad2csg_engine)
if(CAD2CSG_OCCT_TKOPENGL)
    find_library(CAD2CSG_OCCT_TKV3D
        NAMES TKV3d
        PATHS "${OpenCASCADE_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    find_library(CAD2CSG_OCCT_TKSERVICE
        NAMES TKService
        PATHS "${OpenCASCADE_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    target_sources(cad2csg PRIVATE src/viewer.cpp)
    target_link_libraries(cad2csg PRIVATE
        "${CAD2CSG_OCCT_TKOPENGL}"
        "${CAD2CSG_OCCT_TKV3D}"
        "${CAD2CSG_OCCT_TKSERVICE}"
        X11
    )
    target_compile_definitions(cad2csg PRIVATE CAD2CSG_HAVE_TKOPENGL=1)
else()
    target_compile_definitions(cad2csg PRIVATE CAD2CSG_HAVE_TKOPENGL=0)
endif()
target_include_directories(cad2csg PRIVATE "${CAD2CSG_LIBALEA_ROOT}/include")
set_target_properties(cad2csg PROPERTIES
    BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
    INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}"
)
target_link_options(cad2csg PRIVATE "-Wl,--disable-new-dtags")

option(CAD2CSG_BUILD_FIXTURE_GENERATOR "Build the developer tool that generates STEP test fixtures" OFF)
if(CAD2CSG_BUILD_FIXTURE_GENERATOR)
    add_executable(cad2csg_generate_fixtures tests/fixtures/generate_fixtures.cpp)
    target_include_directories(cad2csg_generate_fixtures PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    target_link_libraries(cad2csg_generate_fixtures PRIVATE ${OpenCASCADE_LIBRARIES})
    set_target_properties(cad2csg_generate_fixtures PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}")
    target_link_options(cad2csg_generate_fixtures PRIVATE "-Wl,--disable-new-dtags")
endif()

include(CTest)

set(CAD2CSG_TEST_STEP
    "/home/giovanni/OCCT-8_0_0/data/step/screw.step"
    CACHE FILEPATH "STEP file used by cad2csg smoke tests")

if(BUILD_TESTING AND EXISTS "${CAD2CSG_TEST_STEP}")
    add_test(
        NAME cad2csg_phase1_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/phase1_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CAD2CSG_TEST_STEP}"
            "${CMAKE_CURRENT_BINARY_DIR}/test-output"
    )
endif()

if(BUILD_TESTING)
    add_executable(cad2csg_session_smoke tests/session_smoke.cpp)
    target_link_libraries(cad2csg_session_smoke PRIVATE cad2csg_engine)
    set_target_properties(cad2csg_session_smoke PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}"
    )
    target_link_options(cad2csg_session_smoke PRIVATE "-Wl,--disable-new-dtags")

    add_executable(cad2csg_c_api_smoke tests/c_api_smoke.c)
    target_link_libraries(cad2csg_c_api_smoke PRIVATE cad2csg_engine)
    set_target_properties(cad2csg_c_api_smoke PROPERTIES
        BUILD_RPATH "${OpenCASCADE_LIBRARY_DIR}"
        INSTALL_RPATH "${OpenCASCADE_LIBRARY_DIR}"
    )
    target_link_options(cad2csg_c_api_smoke PRIVATE "-Wl,--disable-new-dtags")

    add_test(
        NAME cad2csg_contract_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/contract-output"
    )

    add_test(
        NAME cad2csg_decision_log_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decision_log_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decision-log-output"
    )

    add_test(
        NAME cad2csg_decision_pack_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decision_pack_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decision-pack-output"
    )

    add_test(
        NAME cad2csg_hole_fill_contract_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/hole_fill_contract_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/hole-fill-output"
    )

    add_test(
        NAME cad2csg_recipe_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/recipe_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/recipe-output"
    )

    add_test(
        NAME cad2csg_replay_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/replay-output"
    )

    add_test(
        NAME cad2csg_step_export_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/step_export_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/step-export-output"
    )

    add_test(
        NAME cad2csg_decompose_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decompose_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decompose-output"
    )

    add_test(
        NAME cad2csg_decompose_apply_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/decompose_apply_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/decompose-apply-output"
    )

    add_test(
        NAME cad2csg_tilted_torus_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/tilted_torus_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/tilted-torus-output"
    )

    add_test(
        NAME cad2csg_help_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/help_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/help-output"
    )

    add_test(
        NAME cad2csg_units_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/units_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/units-output"
    )

    add_test(
        NAME cad2csg_explain_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/explain_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/explain-output"
    )

    add_test(
        NAME cad2csg_safe_all_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/safe_all_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/safe-all-output"
    )

    add_test(
        NAME cad2csg_propose_apply_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/propose_apply_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/propose-apply-output"
    )

    add_test(
        NAME cad2csg_id_stability_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/id_stability_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/id-stability-output"
    )

    add_test(
        NAME cad2csg_session_smoke
        COMMAND
            "$<TARGET_FILE:cad2csg_session_smoke>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
            "${CMAKE_CURRENT_BINARY_DIR}/session_box.i"
    )

    add_test(
        NAME cad2csg_c_api_smoke
        COMMAND
            "$<TARGET_FILE:cad2csg_c_api_smoke>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
            "${CMAKE_CURRENT_BINARY_DIR}/c_api_box.i"
    )

    if(NOT CAD2CSG_ENABLE_LUA)
        add_test(
            NAME cad2csg_lua_not_built
            COMMAND
                "$<TARGET_FILE:cad2csg>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/inspect.lua"
        )
        set_tests_properties(cad2csg_lua_not_built PROPERTIES WILL_FAIL TRUE)
    else()
        add_test(
            NAME cad2csg_lua_smoke
            COMMAND
                bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_smoke.sh"
                "$<TARGET_FILE:cad2csg>"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/inspect.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
                "${CMAKE_CURRENT_BINARY_DIR}/lua_inspect.json"
        )
        add_test(
            NAME cad2csg_lua_agent_workflow
            COMMAND
                "$<TARGET_FILE:cad2csg>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_agent_workflow.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/box.step"
                "${CMAKE_CURRENT_BINARY_DIR}/lua_agent_box.i"
                "${CMAKE_CURRENT_BINARY_DIR}/lua_agent_report.json"
        )
        add_test(
            NAME cad2csg_hole_fill_apply
            COMMAND
                "$<TARGET_FILE:cad2csg>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/hole_fill_apply.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bolt_hole.step"
                "${CMAKE_CURRENT_BINARY_DIR}/hole_fill_apply.i"
        )
        add_test(
            NAME cad2csg_surface_replacement_apply
            COMMAND
                "$<TARGET_FILE:cad2csg>"
                lua
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/surface_replacement_apply.lua"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/spline_face.step"
                "${CMAKE_CURRENT_BINARY_DIR}/surface_replacement_apply.i"
        )
    endif()
endif()
