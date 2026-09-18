# Usage: cmake -DINPUT=<file> -DOUTPUT=<header> -DSYMBOL=<name> -P EmbedFile.cmake
# Writes a header with `inline const unsigned char SYMBOL[]` and `SYMBOL_size`.
file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _bytes "${_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _body "${_hex}")
file(WRITE "${OUTPUT}"
"#pragma once\n#include <cstddef>\n// Generated from ${INPUT}. Do not edit.\ninline const unsigned char ${SYMBOL}[] = {${_body}};\ninline const size_t ${SYMBOL}_size = ${_bytes};\n")
