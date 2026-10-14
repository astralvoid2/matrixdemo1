# Emit byte arrays rather than linker-specific _binary_* symbols. Byte arrays
# avoid C string length limits and preserve shader bytes on every compiler.
get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT_FILE}" "/* Generated from the GLSL sources; do not edit. */\n#pragma once\n")
foreach(shader shader post)
    file(READ "${SOURCE_DIR}/${shader}.frag" shader_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," shader_bytes "${shader_hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n" shader_bytes "${shader_bytes}")
    file(APPEND "${OUTPUT_FILE}" "static const unsigned char embedded_${shader}[] = {\n${shader_bytes}0x00};\n")
endforeach()
