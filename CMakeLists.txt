cmake_minimum_required(VERSION 3.21)

project(classiCAD VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(CLASSICAD_CORE_SOURCES
    src/core/debug_log.cpp
    src/core/tool_id.cpp
    src/core/geometry/geometry_type.cpp
    src/core/geometry/nurbs_curve.cpp
    src/core/geometry/curve_evaluator.cpp
    src/core/geometry/geometry_transform.cpp
    src/core/document/document.cpp
    src/core/document/selection_model.cpp
    src/core/history/history.cpp
    src/core/serialization/document_serializer.cpp
    src/core/model.cpp
)

set(CLASSICAD_SERVICE_SOURCES
    src/services/viewport/viewport_transform.cpp
    src/services/sampling/curve_sampler.cpp
    src/services/hit_testing/curve_hit_tester.cpp
    src/services/snapping/snap_engine.cpp
)

set(CLASSICAD_TOOL_SOURCES
    src/tools/tool.cpp
    src/tools/tool_context.cpp
    src/tools/tool_registry.cpp
    src/tools/shape_creation_tool.cpp
    src/tools/select_tool.cpp
    src/tools/arc_tool.cpp
    src/tools/erase_tool.cpp
    src/tools/point_tool.cpp
    src/tools/line_tool.cpp
    src/tools/tangent_from_curve_tool.cpp
    src/tools/perpendicular_from_curve_tool.cpp
    src/tools/mirror_tool.cpp
    src/tools/rectangle_tool.cpp
    src/tools/circle_tool.cpp
    src/tools/ellipse_tool.cpp
    src/tools/bezier_tool.cpp
    src/tools/nurbs_tool.cpp
    src/tools/rotate_tool.cpp
    src/tools/trim_tool.cpp
)

set(CLASSICAD_VIEWPORT_SUPPORT_SOURCES
    src/ui/input_helpers.cpp
    src/ui/viewport/viewport_renderer.cpp
    src/ui/viewport/viewport_overlay.cpp
)

set(CLASSICAD_UI_SOURCES
    ${CLASSICAD_VIEWPORT_SUPPORT_SOURCES}
    src/ui/viewport_widget.cpp
    src/ui/main_window.cpp
)

set(CLASSICAD_APPLICATION_SOURCES
    src/main.cpp
    ${CLASSICAD_CORE_SOURCES}
    ${CLASSICAD_SERVICE_SOURCES}
    ${CLASSICAD_TOOL_SOURCES}
    ${CLASSICAD_UI_SOURCES}
)

# Keep IDE source trees aligned with the on-disk architecture. The test
# targets reuse the same core/service/tool modules, so each implementation has
# one canonical CMake group instead of a separately maintained flat list.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
             FILES ${CLASSICAD_APPLICATION_SOURCES})

add_executable(classiCAD
    ${CLASSICAD_APPLICATION_SOURCES}
)

target_include_directories(classiCAD PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(classiCAD PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
)

if (MSVC)
    target_compile_options(classiCAD PRIVATE /W4)
else()
    target_compile_options(classiCAD PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(CLASSICAD_BUILD_TESTS "Build geometry regression tests" OFF)
if (CLASSICAD_BUILD_TESTS)
    enable_testing()

    set(CLASSICAD_CORE_CONTRACT_SOURCES ${CLASSICAD_CORE_SOURCES})
    list(REMOVE_ITEM CLASSICAD_CORE_CONTRACT_SOURCES src/core/debug_log.cpp)

    set(CLASSICAD_TEST_RUNTIME_SOURCES
        ${CLASSICAD_CORE_SOURCES}
        ${CLASSICAD_SERVICE_SOURCES}
        ${CLASSICAD_TOOL_SOURCES}
        ${CLASSICAD_VIEWPORT_SUPPORT_SOURCES}
    )

    set(CLASSICAD_CORE_CONTRACT_TEST_SOURCES
        ${CLASSICAD_CORE_CONTRACT_SOURCES}
        ${CLASSICAD_SERVICE_SOURCES}
        ${CLASSICAD_TOOL_SOURCES}
        src/ui/viewport/viewport_renderer.cpp
        src/ui/viewport/viewport_overlay.cpp
    )

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                 PREFIX "tests"
                 FILES tests/trim_seam.cpp tests/core_contracts.cpp)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
                 FILES ${CLASSICAD_TEST_RUNTIME_SOURCES}
                       ${CLASSICAD_CORE_CONTRACT_TEST_SOURCES})

    add_executable(classicad_trim_seam_test
        tests/trim_seam.cpp
        ${CLASSICAD_TEST_RUNTIME_SOURCES}
    )
    target_include_directories(classicad_trim_seam_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(classicad_trim_seam_test PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
    add_test(NAME trim_seam COMMAND classicad_trim_seam_test)
    set_tests_properties(trim_seam PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(classicad_core_contracts_test
        tests/core_contracts.cpp
        ${CLASSICAD_CORE_CONTRACT_TEST_SOURCES}
    )
    target_include_directories(classicad_core_contracts_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(classicad_core_contracts_test PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
    add_test(NAME core_contracts COMMAND classicad_core_contracts_test)
    set_tests_properties(core_contracts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
