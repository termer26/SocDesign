cmake_minimum_required(VERSION 3.16)
project(learning_router LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(router_core
    src/tokenizer.cpp src/lef_parser.cpp src/def_parser.cpp
    src/pin_resolver.cpp src/gcell_grid.cpp src/astar.cpp
    src/net_router.cpp src/def_writer.cpp)
target_include_directories(router_core PUBLIC include)
if(MSVC)
    target_compile_options(router_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(router_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(learning_router src/main.cpp)
target_link_libraries(learning_router PRIVATE router_core)

# Competition executable shares the tested LEF/DEF geometry frontend with the
# educational router, and owns its capacity, search, guide and validation logic.
add_library(contest_core src/contest_resources.cpp src/contest_router.cpp src/contest_guide.cpp)
target_link_libraries(contest_core PUBLIC router_core)
if(MSVC)
    target_compile_options(contest_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(contest_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(contest_router src/contest_main.cpp)
target_link_libraries(contest_router PRIVATE contest_core)
if(WIN32)
    target_link_libraries(contest_router PRIVATE psapi)
endif()
if(MINGW)
    target_link_options(contest_router PRIVATE -static -static-libgcc -static-libstdc++)
endif()
install(TARGETS contest_router RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    if(NOT DEFINED ROUTER_SAMPLE_DIR)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../case/cases/sample001/sample001.input.def")
            set(ROUTER_SAMPLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../case/cases/sample001")
        else()
            set(ROUTER_SAMPLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../case/case1/cases/sample001")
        endif()
    endif()
    add_executable(router_tests tests/router_tests.cpp)
    target_link_libraries(router_tests PRIVATE router_core)
    target_compile_definitions(router_tests PRIVATE
        ROUTER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" ROUTER_SAMPLE_DIR="${ROUTER_SAMPLE_DIR}")
    add_test(NAME router_tests COMMAND router_tests)
    add_test(NAME sample001 COMMAND learning_router
        --lef ${ROUTER_SAMPLE_DIR}/sample001.input.lef
        --def ${ROUTER_SAMPLE_DIR}/sample001.input.def
        --out ${CMAKE_CURRENT_BINARY_DIR}/sample001.routed.def
        --pins ${CMAKE_CURRENT_BINARY_DIR}/sample001.pins.csv)
    add_executable(contest_guide_tests tests/contest_guide_tests.cpp)
    target_link_libraries(contest_guide_tests PRIVATE contest_core)
    add_test(NAME contest_guide_tests COMMAND contest_guide_tests)
    add_executable(contest_search_tests tests/contest_search_tests.cpp)
    target_link_libraries(contest_search_tests PRIVATE contest_core)
    add_test(NAME contest_search_tests COMMAND contest_search_tests)
    add_executable(contest_resources_tests tests/contest_resources_tests.cpp)
    target_link_libraries(contest_resources_tests PRIVATE contest_core)
    add_test(NAME contest_resources_tests COMMAND contest_resources_tests)
    # Optional real-input check against the scorer's heat_gcell.csv; not part
    # of CTest because contest inputs/scorer output are external artifacts.
    add_executable(contest_thermal_check tests/contest_thermal_check.cpp)
    target_link_libraries(contest_thermal_check PRIVATE contest_core)
    add_test(NAME contest_sample001 COMMAND contest_router
        --lef ${ROUTER_SAMPLE_DIR}/sample001.input.lef
        --def ${ROUTER_SAMPLE_DIR}/sample001.input.def
        --thermal ${ROUTER_SAMPLE_DIR}/sample001.thermal.input.json
        --out ${CMAKE_CURRENT_BINARY_DIR}/sample001.contest.guide)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME contest_cli_tests COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/contest_cli_tests.py $<TARGET_FILE:contest_router>)
    endif()
endif()
