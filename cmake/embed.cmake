# Turn a binary file into a C source defining
#   const unsigned char <NAME>[];  const unsigned long <NAME>_size;
# Usage: cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR nbytes "${len} / 2")
# one pass: every byte -> "0xNN,", then a newline after each 32 bytes
string(REGEX REPLACE "(..)" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" body "${body}")
file(WRITE "${OUT}"
"/* Generated from ${IN} by cmake/embed.cmake - do not edit. */\n"
"const unsigned char ${NAME}[] = {\n${body}\n};\n"
"const unsigned long ${NAME}_size = ${nbytes}UL;\n")
