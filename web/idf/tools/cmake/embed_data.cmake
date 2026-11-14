# A file as C data with ESP-IDF's symbols (target_add_binary_data, EMBED_FILES, EMBED_TXTFILES):
#   cmake -D DATA_FILE=<file> -D SOURCE_FILE=<out.c> -D SYMBOL=<name> -D FILE_TYPE=BINARY|TEXT -P embed_data.cmake
# <name> and _binary_<name>_start are the data, _binary_<name>_end the byte after it (a TEXT file has a
# terminating zero before it), <name>_length its size (without that zero). The data and its end share
# one section, in that order, so end - start is the size as on the device.

file(READ "${DATA_FILE}" data HEX)
string(LENGTH "${data}" length)
math(EXPR length "${length} / 2")
set(size ${length})
if(FILE_TYPE STREQUAL "TEXT")
    set(data "${data}00")
    math(EXPR size "${size} + 1")
endif()
if(size EQUAL 0)
    set(data "00")
    set(size 1)
    set(empty TRUE)
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," data "${data}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " data "${data}")
if(empty)
    # An empty file: the end is the start.
    set(array_size 1)
    set(end_line "extern const unsigned char _binary_${SYMBOL}_end[1] __attribute__((alias(\"${SYMBOL}\")));")
else()
    set(array_size ${size})
    set(end_line "CW_SECTION __attribute__((aligned(1))) const unsigned char _binary_${SYMBOL}_end[1] = {0};")
endif()

file(WRITE "${SOURCE_FILE}"
"/* ${DATA_FILE}, embedded (${FILE_TYPE}) by the CardenzaHAL web target. */
#define CW_SECTION __attribute__((section(\".rodata.embedded.${SYMBOL}\"), used))
CW_SECTION __attribute__((aligned(4))) const unsigned char ${SYMBOL}[${array_size}] = {
    ${data}
};
extern const unsigned char _binary_${SYMBOL}_start[${array_size}] __attribute__((alias(\"${SYMBOL}\")));
${end_line}
const unsigned int ${SYMBOL}_length = ${length};
")
