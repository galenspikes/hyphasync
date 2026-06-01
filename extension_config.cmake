# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(hyphasync
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# json is required for ::JSON casts on LIST/STRUCT/MAP columns (fingerprinting + COPY).
# Bundling it here ensures it is always available without a network download.
duckdb_extension_load(json)