# MiniVHD static library — source files added after git clone
add_library(minivhd STATIC)
target_include_directories(minivhd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/MiniVHD)

# Placeholder source so the target resolves before MiniVHD is cloned
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/minivhd_placeholder.c "/* placeholder */")
target_sources(minivhd PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/minivhd_placeholder.c)
