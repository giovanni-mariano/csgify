cmake_minimum_required(VERSION 3.20)

project(cad2csg VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CAD2CSG_LIBALEA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libalea" CACHE PATH "Path to libalea checkout")

find_package(OpenCASCADE 8.0.0 EXACT REQUIRED)

add_library(cad2csg_engine
    src/analytic_surface.cpp
    src/analyze.cpp
    src/cleanup.cpp
    src/decompose.cpp
    src/emit.cpp
    src/engine.cpp
    src/engine_internal.cpp
    src/fitted_candidates.cpp
    src/import.cpp
    src/report.cpp
    src/spline_fit.cpp
)

target_include_directories(cad2csg_engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
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

add_executable(cad2csg src/main.cpp)
target_link_libraries(cad2csg PRIVATE cad2csg_engine)
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
    add_test(
        NAME cad2csg_contract_smoke
        COMMAND
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_smoke.sh"
            "$<TARGET_FILE:cad2csg>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            "${CMAKE_CURRENT_BINARY_DIR}/contract-output"
    )
endif()
