cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME hyphasync)

# libpq (Phase 1). Homebrew installs keg-only libpq; help CMake find it on macOS.
if(APPLE)
	if(EXISTS "/opt/homebrew/opt/libpq")
		list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/libpq")
	endif()
	if(EXISTS "/usr/local/opt/libpq")
		list(APPEND CMAKE_PREFIX_PATH "/usr/local/opt/libpq")
	endif()
endif()

find_package(PostgreSQL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/hyphasync_extension.cpp src/hypha_metadata.cpp src/hypha_postgres.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} PostgreSQL::PostgreSQL)
target_link_libraries(${LOADABLE_EXTENSION_NAME} PostgreSQL::PostgreSQL)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
