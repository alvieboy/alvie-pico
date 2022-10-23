option(PICO_VFS "Globablly enable VFS " 1)

if (NOT TARGET pico_vfs)
    pico_add_impl_library(pico_vfs)

    function(pico_enable_vfs TARGET ENABLED)
        set_target_properties(${TARGET} PROPERTIES PICO_TARGET_VFS ${ENABLED})
    endfunction()
    
    target_sources(pico_vfs INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/vfs.c
    )

    target_include_directories(pico_vfs INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
endif()
