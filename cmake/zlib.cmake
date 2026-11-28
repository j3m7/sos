# zlib (ZLIB_ROOT, lib/zlib by default): inflate for gunzip and zcat, deflate for gzip
# (docs/design/archives.md §5.1). Included by the firmware's CMakeLists.txt and by the host
# tests', so the code under test is the code on the board.
#
# Only the stream code: no gz* file functions (SOS reads and writes files through FatFs), no
# compress/uncompress or infback. Z_SOLO: zlib uses nothing of the C library's allocation or I/O,
# and src/files/gz.c gives it its allocator (the HEAP_ZLIB tag, shown by `mem`). ZLIB_CONST:
# next_in is a const pointer. deflate.c and trees.c are only linked once gzip calls deflate:
# until then the linker drops them (--gc-sections).
#
# Sets ZLIB_SOURCES and ZLIB_DEFINITIONS; the caller builds them as third-party code (SYSTEM
# includes, no SOS warnings).

if (NOT DEFINED ZLIB_ROOT)
    set(ZLIB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../lib/zlib" CACHE PATH "zlib source")
endif()
if (NOT EXISTS "${ZLIB_ROOT}/zlib.h")
    message(FATAL_ERROR "ZLIB_ROOT (${ZLIB_ROOT}) is empty or missing. "
        "Run 'git submodule update --init', or point ZLIB_ROOT at a checkout in local.cmake.")
endif()
set(ZLIB_SOURCES)
foreach(f adler32 crc32 deflate inffast inflate inftrees trees zutil)
    list(APPEND ZLIB_SOURCES ${ZLIB_ROOT}/${f}.c)
endforeach()
set(ZLIB_DEFINITIONS Z_SOLO ZLIB_CONST)
