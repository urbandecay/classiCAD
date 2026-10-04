function(classicad_configure_cpp_target target)
    # classiCAD has no Q_OBJECT/Q_GADGET declarations or .ui forms. Disable
    # the global Qt code generators for project targets; AUTORCC remains on
    # for the embedded resources listed in the target sources.
    set_target_properties(${target} PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
    )

    target_include_directories(${target} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
    )

    target_link_libraries(${target} PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
    )

    if (MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(classicad_configure_ui_target target)
    classicad_configure_cpp_target(${target})
    target_link_libraries(${target} PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        ${CLASSICAD_QT_OPENGL_LIBRARIES}
    )
endfunction()

function(classicad_add_object_module target)
    add_library(${target} OBJECT ${ARGN})
    classicad_configure_cpp_target(${target})
endfunction()

function(classicad_add_ui_object_module target)
    add_library(${target} OBJECT ${ARGN})
    classicad_configure_ui_target(${target})
endfunction()

function(classicad_attach_object_modules target)
    foreach(module IN LISTS ARGN)
        target_sources(${target} PRIVATE "$<TARGET_OBJECTS:${module}>")
        if (module STREQUAL "classicad_rhino_interchange_objects")
            target_link_libraries(${target} PRIVATE
                opennurbsStatic
                zlib
                # The bundled zlib archive calls allocator hooks defined by
                # openNURBS. Keep openNURBS after zlib for static linkers.
                opennurbsStatic
            )
        endif ()
    endforeach()
endfunction()
