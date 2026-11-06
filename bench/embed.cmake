function(target_embed_file TARGET FILE)
    get_filename_component(FILE_NAME "${FILE}" NAME)
    get_filename_component(FILE_DIR  "${FILE}" DIRECTORY)
    get_filename_component(FILE_WE   "${FILE}" NAME_WE)

    set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/embed")
    set(OUTPUT_OBJ "${OUTPUT_DIR}/${FILE_WE}.o")

    add_custom_command(
        OUTPUT "${OUTPUT_OBJ}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_DIR}"
        COMMAND
            ${CMAKE_COMMAND} -E chdir "${FILE_DIR}"
            ${CMAKE_OBJCOPY}
                -I binary
                -O elf32-littlearm
                --rename-section .data=.rodata,contents,alloc,load,readonly,data
                "${FILE_NAME}"
                "${OUTPUT_OBJ}"
        DEPENDS "${FILE}"
        VERBATIM
    )

    target_sources(${TARGET} PRIVATE "${OUTPUT_OBJ}")
    set_source_files_properties("${OUTPUT_OBJ}" PROPERTIES EXTERNAL_OBJECT TRUE)
endfunction()
