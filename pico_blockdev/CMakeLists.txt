option(PICO_BLOCKDEV "Globablly enable block device support " 1)

pico_add_library(pico_blockdev)

function(pico_enable_blockdev TARGET ENABLED)
    set_target_properties(${TARGET} PROPERTIES PICO_TARGET_BLOCKDEV ${ENABLED})
endfunction()

target_sources(pico_blockdev INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/blockdev.c
    ${CMAKE_CURRENT_LIST_DIR}/partition.c
)
target_link_libraries(pico_blockdev INTERFACE pico_object)

target_include_directories(pico_blockdev INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
