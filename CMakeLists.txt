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
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS OpenGL)

set(CLASSICAD_QT_OPENGL_LIBRARIES Qt${QT_VERSION_MAJOR}::OpenGL)
if (QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets)
    list(APPEND CLASSICAD_QT_OPENGL_LIBRARIES Qt6::OpenGLWidgets)
endif ()

include(FetchContent)
FetchContent_Declare(
    opennurbs
    GIT_REPOSITORY https://github.com/mcneel/opennurbs.git
    GIT_TAG eb92af3ba1806b0a34a99aba0d3bda83e3d46083
    GIT_SHALLOW FALSE
)
FetchContent_GetProperties(opennurbs)
if (NOT opennurbs_POPULATED)
    FetchContent_Populate(opennurbs)
    add_subdirectory(${opennurbs_SOURCE_DIR} ${opennurbs_BINARY_DIR} EXCLUDE_FROM_ALL)
endif ()

include(cmake/ClassicadTargets.cmake)

set(CLASSICAD_LOGGING_SOURCES
    src/core/debug_log.cpp
)

set(CLASSICAD_GEOMETRY_SOURCES
    src/core/tool_id.cpp
    src/core/geometry/arc_mode.cpp
    src/core/geometry/curve_construction.cpp
    src/core/geometry/curve_editing.cpp
    src/core/geometry/curve_erase_intervals.cpp
    src/core/geometry/curve_intersections.cpp
    src/core/geometry/curve_geometry_data.cpp
    src/core/geometry/curve_join.cpp
    src/core/geometry/curve_subdivision.cpp
    src/core/geometry/geometry_type.cpp
    src/core/geometry/nurbs_curve.cpp
    src/core/geometry/nurbs_surface.cpp
    src/core/geometry/nurbs_surface_evaluator.cpp
    src/core/geometry/nurbs_surface_factory.cpp
    src/core/geometry/nurbs_solid.cpp
    src/core/geometry/nurbs_surface_tessellator.cpp
    src/core/geometry/arc_curve_factory.cpp
    src/core/geometry/interpolating_curve_factory.cpp
    src/core/geometry/work_plane.cpp
    src/core/geometry/curve_evaluator.cpp
    src/core/geometry/circle_construction.cpp
    src/core/geometry/geometry_transform.cpp
    src/core/geometry/planar_geometry.cpp
    src/core/geometry/point_text.cpp
    src/core/geometry/shape_mapping.cpp
    src/core/geometry/surface_trim_region.cpp
)

set(CLASSICAD_DOCUMENT_SOURCES
    src/core/document/document.cpp
    src/core/document/document_settings.cpp
    src/core/document/selection_model.cpp
    src/core/history/document_transaction.cpp
    src/core/history/history.cpp
)

set(CLASSICAD_COMMAND_SOURCES
    src/core/commands/delete_command.cpp
    src/core/commands/duplicate_command.cpp
    src/core/commands/explode_command.cpp
    src/core/commands/fill_command.cpp
    src/core/commands/join_command.cpp
    src/core/commands/layer_command.cpp
    src/core/commands/mirror_command.cpp
    src/core/commands/subdivision_command.cpp
    src/core/commands/trim_erase_command.cpp
    src/core/commands/transform_command.cpp
    src/core/commands/weld_command.cpp
)

set(CLASSICAD_SERIALIZATION_SOURCES
    src/core/serialization/document_serializer.cpp
    src/core/serialization/shape_json_codec.cpp
    src/core/serialization/blender_project_file.cpp
    src/core/serialization/blender_project_adapter.qrc
)

set(CLASSICAD_RHINO_INTERCHANGE_SOURCES
    src/core/serialization/rhino3dm_interchange.cpp
)

set(CLASSICAD_CORE_SOURCES
    ${CLASSICAD_LOGGING_SOURCES}
    ${CLASSICAD_GEOMETRY_SOURCES}
    ${CLASSICAD_DOCUMENT_SOURCES}
    ${CLASSICAD_COMMAND_SOURCES}
    ${CLASSICAD_SERIALIZATION_SOURCES}
    ${CLASSICAD_RHINO_INTERCHANGE_SOURCES}
)

set(CLASSICAD_SERVICE_SOURCES
    src/services/viewport/viewport_transform.cpp
    src/services/erase/curve_erase_query.cpp
    src/services/erase/trim_erase_query.cpp
    src/services/input/drawing_plane_resolver.cpp
    src/services/input/input_constraint_service.cpp
    src/services/snapping/snap_types.cpp
    src/services/sampling/curve_sampler.cpp
    src/services/sampling/surface_tessellation_cache.cpp
    src/services/hit_testing/curve_hit_tester.cpp
    src/services/hit_testing/projected_curve_bounds.cpp
    src/services/hit_testing/selection_box_query.cpp
    src/services/dimensions/dimension_association.cpp
    src/services/dimensions/dimension_layout.cpp
    src/services/dimensions/dimension_font.cpp
    src/services/dimensions/dimension_fonts.qrc
    src/services/snapping/snap_engine.cpp
)

set(CLASSICAD_TOOL_SOURCES
    src/tools/tool.cpp
    src/tools/tool_context.cpp
    src/tools/tool_registry.cpp
    src/tools/shape_creation_tool.cpp
    src/tools/select_tool.cpp
    src/tools/grab_tool.cpp
    src/tools/duplicate_tool.cpp
    src/tools/arc_tool.cpp
    src/tools/scale_tool.cpp
    src/tools/join_tool.cpp
    src/tools/subdivision_tool.cpp
    src/tools/erase_tool.cpp
    src/tools/point_tool.cpp
    src/tools/point_extrude_tool.cpp
    src/tools/point_construction_tool.cpp
    src/tools/curve_drawing_tool.cpp
    src/tools/two_curve_line_tool.cpp
    src/tools/line_tool.cpp
    src/tools/tangent_from_curve_tool.cpp
    src/tools/perpendicular_from_curve_tool.cpp
    src/tools/mirror_tool.cpp
    src/tools/rectangle_tool.cpp
    src/tools/polygon_tool.cpp
    src/tools/circle_tangent_tool.cpp
    src/tools/circle_tool.cpp
    src/tools/ellipse_tool.cpp
    src/tools/bezier_tool.cpp
    src/tools/nurbs_tool.cpp
    src/tools/rotate_tool.cpp
    src/tools/trim_tool.cpp
    src/tools/dimension_tool.cpp
)

set(CLASSICAD_APPLICATION_MODULE_SOURCES
    src/app/application_session.cpp
    src/app/command_router.cpp
    src/app/project_controller.cpp
    src/app/session_serializer.cpp
    src/app/update_controller.cpp
)

set(CLASSICAD_VIEWPORT_SHARED_SOURCES
    src/ui/viewport/blender_grid_frame.cpp
    src/ui/viewport/blender_grid_appearance.cpp
    src/ui/viewport/blender_grid_renderer.cpp
    src/ui/viewport/blender_grid_scale.cpp
    src/ui/viewport/blender_grid_shaders.qrc
    src/ui/viewport/viewport_depth_geometry.cpp
    src/ui/viewport/viewport_erase_overlay_renderer.cpp
    src/ui/viewport/viewport_geometry_cache.cpp
    src/ui/viewport/viewport_render_frame.cpp
    src/ui/viewport/navigation_gizmo.cpp
    src/ui/viewport/viewport_arc_compass_renderer.cpp
    src/ui/viewport/viewport_hud_renderer.cpp
    src/ui/viewport/viewport_snap_marker_renderer.cpp
    src/ui/viewport/viewport_tool_preview_renderer.cpp
    src/ui/viewport/viewport_renderer.cpp
    src/ui/viewport/viewport_overlay.cpp
    src/ui/viewport/blender_navigation_icons.qrc
    src/ui/viewport/blender_shading_icons.qrc
    src/ui/viewport/blender_workbench_lighting.qrc
)

set(CLASSICAD_VIEWPORT_ADAPTER_SUPPORT_SOURCES
    src/ui/input_helpers.cpp
    src/ui/input/tool_input_translator.cpp
    src/ui/viewport/navigation_controller.cpp
    src/ui/viewport/viewport_gpu_surface.cpp
    src/ui/viewport/viewport_control_point_renderer.cpp
    src/ui/viewport/viewport_scene_renderer.cpp
    src/ui/viewport/viewport_surface_renderer.cpp
)

set(CLASSICAD_VIEWPORT_SUPPORT_SOURCES
    ${CLASSICAD_VIEWPORT_SHARED_SOURCES}
    ${CLASSICAD_VIEWPORT_ADAPTER_SUPPORT_SOURCES}
)

set(CLASSICAD_UI_SOURCES
    ${CLASSICAD_VIEWPORT_SUPPORT_SOURCES}
    src/ui/panels/document_grid_dialog.cpp
    src/ui/panels/layer_style_widgets.cpp
    src/ui/panels/layers_panel.cpp
    src/ui/panels/preferences_dialog.cpp
    src/ui/panels/preferences_store.cpp
    src/ui/panels/tool_shelf.cpp
    src/ui/viewport_widget.cpp
    src/ui/main_window.cpp
)

set(CLASSICAD_APPLICATION_SOURCES
    src/main.cpp
    ${CLASSICAD_CORE_SOURCES}
    ${CLASSICAD_SERVICE_SOURCES}
    ${CLASSICAD_TOOL_SOURCES}
    ${CLASSICAD_APPLICATION_MODULE_SOURCES}
    ${CLASSICAD_UI_SOURCES}
)

# Keep IDE source trees aligned with the on-disk architecture. The test
# targets reuse the same core/service/tool modules, so each implementation has
# one canonical CMake group instead of a separately maintained flat list.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
             FILES ${CLASSICAD_APPLICATION_SOURCES})

classicad_add_object_module(classicad_geometry_objects
    ${CLASSICAD_GEOMETRY_SOURCES}
)
classicad_add_object_module(classicad_document_objects
    ${CLASSICAD_DOCUMENT_SOURCES}
)
classicad_add_object_module(classicad_command_objects
    ${CLASSICAD_COMMAND_SOURCES}
)
classicad_add_object_module(classicad_serialization_objects
    ${CLASSICAD_SERIALIZATION_SOURCES}
)
classicad_add_object_module(classicad_rhino_interchange_objects
    ${CLASSICAD_RHINO_INTERCHANGE_SOURCES}
)
target_link_libraries(classicad_rhino_interchange_objects PRIVATE opennurbsStatic)
classicad_add_object_module(classicad_logging_objects
    ${CLASSICAD_LOGGING_SOURCES}
)
classicad_add_object_module(classicad_service_objects
    ${CLASSICAD_SERVICE_SOURCES}
)
classicad_add_object_module(classicad_tool_objects
    ${CLASSICAD_TOOL_SOURCES}
)
classicad_add_object_module(classicad_application_objects
    ${CLASSICAD_APPLICATION_MODULE_SOURCES}
)
classicad_add_ui_object_module(classicad_viewport_shared_objects
    ${CLASSICAD_VIEWPORT_SHARED_SOURCES}
)
classicad_add_ui_object_module(classicad_viewport_support_objects
    ${CLASSICAD_VIEWPORT_ADAPTER_SUPPORT_SOURCES}
)
classicad_add_ui_object_module(classicad_viewport_widget_objects
    src/ui/viewport_widget.cpp
)
classicad_add_ui_object_module(classicad_main_window_objects
    src/ui/panels/document_grid_dialog.cpp
    src/ui/panels/layer_style_widgets.cpp
    src/ui/panels/layers_panel.cpp
    src/ui/panels/preferences_dialog.cpp
    src/ui/panels/preferences_store.cpp
    src/ui/panels/tool_shelf.cpp
    src/ui/theme/classicad_theme.cpp
    src/ui/main_window.cpp
)

add_executable(classiCAD src/main.cpp)
classicad_configure_ui_target(classiCAD)
classicad_attach_object_modules(classiCAD
    classicad_geometry_objects
    classicad_document_objects
    classicad_command_objects
    classicad_serialization_objects
    classicad_rhino_interchange_objects
    classicad_logging_objects
    classicad_service_objects
    classicad_tool_objects
    classicad_application_objects
    classicad_viewport_shared_objects
    classicad_viewport_support_objects
    classicad_viewport_widget_objects
    classicad_main_window_objects
)

option(CLASSICAD_BUILD_TESTS "Build geometry regression tests" OFF)
if (CLASSICAD_BUILD_TESTS)
    enable_testing()

    set(CLASSICAD_TEST_RUNTIME_SOURCES
        ${CLASSICAD_CORE_SOURCES}
        ${CLASSICAD_SERVICE_SOURCES}
        ${CLASSICAD_TOOL_SOURCES}
        ${CLASSICAD_VIEWPORT_SUPPORT_SOURCES}
    )

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                 PREFIX "tests"
                 FILES tests/arc_tool_contracts.cpp
                       tests/command_contracts.cpp
                       tests/scene_query_contracts.cpp
                       tests/trim_seam.cpp
                       tests/core_contracts.cpp
                       tests/viewport_event_precedence.cpp
                       tests/viewport_render_contracts.cpp
                       tests/viewport_interaction.cpp
                       tests/vignola_file.cpp)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
                 FILES ${CLASSICAD_TEST_RUNTIME_SOURCES}
                       ${CLASSICAD_VIEWPORT_SHARED_SOURCES})

    add_executable(classicad_trim_seam_test tests/trim_seam.cpp)
    classicad_configure_ui_target(classicad_trim_seam_test)
    classicad_attach_object_modules(classicad_trim_seam_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
        classicad_serialization_objects
        classicad_rhino_interchange_objects
        classicad_logging_objects
        classicad_service_objects
        classicad_tool_objects
        classicad_application_objects
        classicad_viewport_shared_objects
        classicad_viewport_support_objects
    )
    add_test(NAME trim_seam COMMAND classicad_trim_seam_test)
    set_tests_properties(trim_seam PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME box_selection COMMAND classicad_trim_seam_test --box-only)
    set_tests_properties(box_selection PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;CLASSICAD_LOG_PATH=/tmp/classicad-box-selection-test.log")

    add_executable(classicad_command_contracts_test
        tests/command_contracts.cpp
    )
    classicad_configure_cpp_target(classicad_command_contracts_test)
    classicad_attach_object_modules(classicad_command_contracts_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
    )
    add_test(NAME command_contracts COMMAND classicad_command_contracts_test)

    add_executable(classicad_arc_tool_contracts_test
        tests/arc_tool_contracts.cpp
    )
    classicad_configure_cpp_target(classicad_arc_tool_contracts_test)
    classicad_attach_object_modules(classicad_arc_tool_contracts_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
        classicad_service_objects
        classicad_tool_objects
    )
    add_test(NAME arc_tool_contracts COMMAND classicad_arc_tool_contracts_test)

    add_executable(classicad_scene_query_contracts_test
        tests/scene_query_contracts.cpp
    )
    classicad_configure_cpp_target(classicad_scene_query_contracts_test)
    classicad_attach_object_modules(classicad_scene_query_contracts_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_service_objects
    )
    add_test(NAME scene_query_contracts
        COMMAND classicad_scene_query_contracts_test
    )

    add_executable(classicad_core_contracts_test tests/core_contracts.cpp)
    classicad_configure_ui_target(classicad_core_contracts_test)
    classicad_attach_object_modules(classicad_core_contracts_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
        classicad_serialization_objects
        classicad_service_objects
        classicad_tool_objects
        classicad_viewport_shared_objects
    )
    add_test(NAME core_contracts COMMAND classicad_core_contracts_test)
    set_tests_properties(core_contracts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME connected_line_loops
        COMMAND classicad_core_contracts_test --connected-line-loops)
    set_tests_properties(connected_line_loops PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(classicad_solid_extrusion_contracts_test tests/solid_extrusion_contracts.cpp)
    classicad_configure_ui_target(classicad_solid_extrusion_contracts_test)
    classicad_attach_object_modules(classicad_solid_extrusion_contracts_test
        classicad_geometry_objects classicad_document_objects
        classicad_command_objects classicad_serialization_objects
        classicad_service_objects classicad_tool_objects classicad_viewport_shared_objects
        classicad_viewport_support_objects)
    add_test(NAME solid_extrusion_contracts COMMAND classicad_solid_extrusion_contracts_test)
    set_tests_properties(solid_extrusion_contracts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME surface_boundary_geometry COMMAND classicad_solid_extrusion_contracts_test)
    set_tests_properties(surface_boundary_geometry PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;CLASSICAD_SURFACE_BOUNDARY_GEOMETRY_ONLY=1")

    add_executable(classicad_viewport_render_contracts_test
        tests/viewport_render_contracts.cpp
    )
    classicad_configure_ui_target(classicad_viewport_render_contracts_test)
    classicad_attach_object_modules(classicad_viewport_render_contracts_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
        classicad_serialization_objects
        classicad_service_objects
        classicad_tool_objects
        classicad_viewport_shared_objects
        classicad_viewport_support_objects
    )
    add_test(NAME viewport_render_contracts COMMAND classicad_viewport_render_contracts_test)
    set_tests_properties(viewport_render_contracts PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(classicad_viewport_event_precedence_test
        tests/viewport_event_precedence.cpp
    )
    classicad_configure_ui_target(classicad_viewport_event_precedence_test)
    classicad_attach_object_modules(classicad_viewport_event_precedence_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
        classicad_serialization_objects
        classicad_rhino_interchange_objects
        classicad_logging_objects
        classicad_service_objects
        classicad_tool_objects
        classicad_application_objects
        classicad_viewport_shared_objects
        classicad_viewport_support_objects
        classicad_viewport_widget_objects
    )
    add_test(NAME viewport_event_precedence
        COMMAND classicad_viewport_event_precedence_test
    )
    set_tests_properties(viewport_event_precedence PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(classicad_viewport_interaction_test
        tests/viewport_interaction.cpp
    )
    classicad_configure_ui_target(classicad_viewport_interaction_test)
    classicad_attach_object_modules(classicad_viewport_interaction_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
        classicad_serialization_objects
        classicad_rhino_interchange_objects
        classicad_logging_objects
        classicad_service_objects
        classicad_tool_objects
        classicad_application_objects
        classicad_viewport_shared_objects
        classicad_viewport_support_objects
        classicad_viewport_widget_objects
    )
    add_test(NAME viewport_interaction COMMAND classicad_viewport_interaction_test)
    add_test(NAME viewport_surface_boundary COMMAND classicad_viewport_interaction_test)
    set_tests_properties(viewport_surface_boundary PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;CLASSICAD_SURFACE_BOUNDARY_ONLY=1")
    set_tests_properties(viewport_interaction PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_test(NAME viewport_duplicate_polycurve_closure
        COMMAND classicad_viewport_interaction_test
    )
    set_tests_properties(viewport_duplicate_polycurve_closure PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;CLASSICAD_DUPLICATE_POLYCURVE_CLOSURE_ONLY=1"
    )
    add_test(NAME viewport_duplicate_nurbs_modes
        COMMAND classicad_viewport_interaction_test
    )
    set_tests_properties(viewport_duplicate_nurbs_modes PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;CLASSICAD_DUPLICATE_NURBS_MODES_ONLY=1"
    )

    add_executable(classicad_vignola_file_test
        tests/vignola_file.cpp
    )
    classicad_configure_ui_target(classicad_vignola_file_test)
    classicad_attach_object_modules(classicad_vignola_file_test
        classicad_geometry_objects
        classicad_document_objects
        classicad_serialization_objects
        classicad_rhino_interchange_objects
    )
    add_test(NAME vignola_file COMMAND classicad_vignola_file_test)
    set_tests_properties(vignola_file PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

option(CLASSICAD_BUILD_BENCHMARKS "Build standalone geometry benchmarks" OFF)
if (CLASSICAD_BUILD_BENCHMARKS)
    add_executable(classicad_nurbs_surface_benchmark
        benchmarks/nurbs_surface_benchmark.cpp
        src/services/sampling/surface_tessellation_cache.cpp
    )
    classicad_configure_cpp_target(classicad_nurbs_surface_benchmark)
    classicad_attach_object_modules(classicad_nurbs_surface_benchmark
        classicad_geometry_objects
    )

    add_executable(classicad_erase_scene_query_benchmark
        benchmarks/erase_scene_query_benchmark.cpp
    )
    classicad_configure_cpp_target(classicad_erase_scene_query_benchmark)
    classicad_attach_object_modules(classicad_erase_scene_query_benchmark
        classicad_geometry_objects
        classicad_document_objects
        classicad_service_objects
    )

    add_executable(classicad_viewport_query_benchmark
        benchmarks/viewport_query_benchmark.cpp
    )
    classicad_configure_cpp_target(classicad_viewport_query_benchmark)
    classicad_attach_object_modules(classicad_viewport_query_benchmark
        classicad_geometry_objects
        classicad_document_objects
        classicad_service_objects
    )

    add_executable(classicad_viewport_runtime_benchmark
        benchmarks/viewport_runtime_benchmark.cpp
    )
    classicad_configure_ui_target(classicad_viewport_runtime_benchmark)
    classicad_attach_object_modules(classicad_viewport_runtime_benchmark
        classicad_geometry_objects
        classicad_document_objects
        classicad_command_objects
        classicad_serialization_objects
        classicad_service_objects
        classicad_tool_objects
        classicad_viewport_shared_objects
        classicad_viewport_support_objects
    )
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_custom_target(classicad_dependency_audit ALL
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/cmake/check_dependencies.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Auditing source dependency direction"
        VERBATIM
    )
endif()
