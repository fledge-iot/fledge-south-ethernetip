cmake_minimum_required(VERSION 3.6)

# Plugin version - the VERSION file is the single source of truth. It feeds both
# the generated version.h (reported by plugin_info) and the .deb version built by
# the shared Fledge plugin workflow.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PLUGIN_VERSION LIMIT_COUNT 1)
string(STRIP "${PLUGIN_VERSION}" PLUGIN_VERSION)

project(ethernetip VERSION ${PLUGIN_VERSION})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")

# Fledge source/install directory
#
# Supported options:
#   -DFLEDGE_SRC=<path>      Fledge source tree (headers, and cmake_build/C/lib)
#   -DFLEDGE_INSTALL=<path>  Fledge install root the plugin is installed into
#   -DFLEDGE_ROOT=<path>     Fledge source tree or install root (older option)
#
# If none is given, the FLEDGE_ROOT environment variable is used, then the
# installed Fledge development headers under /usr/include/fledge.
set(FLEDGE_SRC "" CACHE PATH "Path to the Fledge source tree")
set(FLEDGE_INSTALL "" CACHE PATH "Fledge install root to install the plugin into")
set(FLEDGE_ROOT "" CACHE PATH "Path to Fledge source or install root")
if(NOT FLEDGE_ROOT AND FLEDGE_SRC)
	set(FLEDGE_ROOT "${FLEDGE_SRC}")
endif()
if(NOT FLEDGE_ROOT)
	if(DEFINED ENV{FLEDGE_ROOT})
		set(FLEDGE_ROOT $ENV{FLEDGE_ROOT})
	elseif(EXISTS "/usr/include/fledge")
		set(FLEDGE_ROOT "/usr")
	endif()
endif()

if(NOT FLEDGE_ROOT)
	message(FATAL_ERROR "Set FLEDGE_ROOT to the Fledge source or install directory")
endif()

# Find libplctag
find_library(PLCTAG_LIB plctag)
if(NOT PLCTAG_LIB)
	message(FATAL_ERROR "libplctag not found. Install libplctag development package.")
endif()

# Determine include paths based on whether FLEDGE_ROOT points to
# the installed layout or the source tree
if(EXISTS "${FLEDGE_ROOT}/include/plugin_api.h" OR EXISTS "${FLEDGE_ROOT}/include/fledge")
	# Installed layout
	set(FLEDGE_INCLUDE_DIRS ${FLEDGE_ROOT}/include)
elseif(EXISTS "${FLEDGE_ROOT}/C/common/include")
	# Source tree layout
	set(FLEDGE_INCLUDE_DIRS
		${FLEDGE_ROOT}/C/common/include
		${FLEDGE_ROOT}/C/services/common/include
		${FLEDGE_ROOT}/C/services/south/include
		${FLEDGE_ROOT}/C/plugins/common/include
		${FLEDGE_ROOT}/C/thirdparty/rapidjson/include
	)
else()
	message(FATAL_ERROR "Cannot find Fledge headers in ${FLEDGE_ROOT}")
endif()

include_directories(
	include
	${FLEDGE_INCLUDE_DIRS}
)

# Find RapidJSON
find_path(RAPIDJSON_INCLUDE rapidjson/document.h
	PATHS
		${FLEDGE_INCLUDE_DIRS}
		/usr/include
		/usr/local/include
)
if(RAPIDJSON_INCLUDE)
	include_directories(${RAPIDJSON_INCLUDE})
endif()

# Generate the version header. This runs on every build so the embedded git
# commit stays accurate; mkversion only rewrites version.h when its contents
# change, so unchanged checkouts do not trigger a recompile.
set_source_files_properties(${CMAKE_BINARY_DIR}/version.h PROPERTIES GENERATED TRUE)
add_custom_target(version_header ALL
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/mkversion ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}
	BYPRODUCTS ${CMAKE_BINARY_DIR}/version.h
	COMMENT "Generating version header"
	VERBATIM
)
include_directories(${CMAKE_BINARY_DIR})

# Source files
set(SOURCES
	plugin.cpp
	plctag.cpp
)

# Build shared library
add_library(${PROJECT_NAME} SHARED ${SOURCES})
add_dependencies(${PROJECT_NAME} version_header)

target_link_libraries(${PROJECT_NAME}
	${PLCTAG_LIB}
	-lpthread
)

# Install into FLEDGE_INSTALL when given, otherwise into FLEDGE_ROOT
if(FLEDGE_INSTALL)
	set(FLEDGE_INSTALL_DIR "${FLEDGE_INSTALL}")
else()
	set(FLEDGE_INSTALL_DIR "${FLEDGE_ROOT}" CACHE PATH "Fledge install directory")
endif()
message(STATUS "Installing ${PROJECT_NAME} in ${FLEDGE_INSTALL_DIR}/plugins/south/${PROJECT_NAME}")
install(TARGETS ${PROJECT_NAME}
	DESTINATION "${FLEDGE_INSTALL_DIR}/plugins/south/${PROJECT_NAME}")

# Fledge lib directory (needed by tests for linking)
if(EXISTS "${FLEDGE_ROOT}/cmake_build/C/lib")
	set(FLEDGE_LIB_DIR "${FLEDGE_ROOT}/cmake_build/C/lib" CACHE PATH "" FORCE)
else()
	set(FLEDGE_LIB_DIR "${FLEDGE_ROOT}/lib" CACHE PATH "" FORCE)
endif()

# Tests - build with: cmake -DBUILD_TESTS=ON ..
option(BUILD_TESTS "Build unit, plugin-load, and integration tests" OFF)
if(BUILD_TESTS)
	# enable_testing() has to be called in the top-level list file so that a
	# CTestTestfile.cmake is generated in the build root; calling it only in
	# tests/ leaves `ctest` reporting "No tests were found".
	enable_testing()
	add_subdirectory(tests)
endif()
