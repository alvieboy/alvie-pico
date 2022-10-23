if (NOT TARGET pico_object)
    pico_add_impl_library(pico_object)

    target_include_directories(pico_object
    	INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/include
    )
endif()
