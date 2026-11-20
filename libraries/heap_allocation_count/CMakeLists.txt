# The counting interposer of BUG-0087: a shared library loaded with LD_PRELOAD that counts calls to the heap
# allocator, thread by thread, while a window is open. Used by scripts/heap_allocation_count.py. Written in C; see the
# comment at the top of heap_allocation_count.c for why.
enable_language(C)

# The project's compile options are for C++, and some are refused for C, so this directory sets its own.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")

add_library(heap_allocation_count SHARED
        heap_allocation_count.c
)

set_target_properties(heap_allocation_count PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
)

target_compile_options(heap_allocation_count PRIVATE -O2 -Wall -Wextra -Werror)

install(TARGETS heap_allocation_count
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
