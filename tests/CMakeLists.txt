# tests/CMakeLists.txt - included from top-level via add_subdirectory(tests)
#
# Inherits from parent: FLEDGE_ROOT, FLEDGE_INCLUDE_DIRS, FLEDGE_LIB_DIR,
#                        PLCTAG_LIB, include_directories, C++ standard

# GTest + GMock - build from source if only headers are installed
set(GTEST_SRC_DIR "/usr/src/googletest" CACHE PATH "Path to googletest source")
if(EXISTS "${GTEST_SRC_DIR}/CMakeLists.txt")
	add_subdirectory(${GTEST_SRC_DIR} ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
	set(GTEST_LIBS gtest gmock gmock_main)
else()
	find_package(GTest REQUIRED)
	set(GTEST_LIBS GTest::gtest GTest::gmock GTest::gmock_main)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

link_directories(${FLEDGE_LIB_DIR})

set(FLEDGE_LINK_LIBS common-lib services-common-lib plugins-common-lib dl)

# ---- Unit tests (mock libplctag) ----

add_executable(unit_tests
	${CMAKE_SOURCE_DIR}/plctag.cpp
	mock_libplctag.cpp
	unit/test_plctag.cpp
)

target_link_libraries(unit_tests
	${GTEST_LIBS}
	pthread
	${FLEDGE_LINK_LIBS}
)

set_target_properties(unit_tests PROPERTIES
	BUILD_RPATH "${FLEDGE_LIB_DIR}"
)

# ---- Plugin load tests (dlopen the built .so, no PLC needed) ----

add_executable(plugin_load_tests
	unit/test_plugin_load.cpp
)

target_compile_definitions(plugin_load_tests PRIVATE
	PLUGIN_SO_PATH="$<TARGET_FILE:${PROJECT_NAME}>"
)

target_link_libraries(plugin_load_tests
	gtest
	gtest_main
	pthread
	${FLEDGE_LINK_LIBS}
)

set_target_properties(plugin_load_tests PROPERTIES
	BUILD_RPATH "${FLEDGE_LIB_DIR}"
)

# plugin_load_tests needs the .so to exist first
add_dependencies(plugin_load_tests ${PROJECT_NAME})

# ---- Plugin discovery probe (mimics Fledge's get_plugin_info utility) ----
#
# Links ONLY dl - no Fledge libraries, no gtest. That is the whole point: it
# reproduces the environment of $FLEDGE_ROOT/extras/C/get_plugin_info, in which
# any libcommon-lib call made from plugin_info() is an unresolved symbol and
# takes the process down. Do not add ${FLEDGE_LINK_LIBS} here.

add_executable(plugin_info_probe
	plugin_info_probe.cpp
)

target_compile_definitions(plugin_info_probe PRIVATE
	PLUGIN_SO_PATH="$<TARGET_FILE:${PROJECT_NAME}>"
)

target_link_libraries(plugin_info_probe dl)

add_dependencies(plugin_info_probe ${PROJECT_NAME})

# ---- Integration tests (real libplctag, real vPLC) ----

add_executable(integration_tests
	${CMAKE_SOURCE_DIR}/plctag.cpp
	integration/test_integration.cpp
)

target_include_directories(integration_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/integration
)

target_link_libraries(integration_tests
	gtest
	gtest_main
	pthread
	${PLCTAG_LIB}
	${FLEDGE_LINK_LIBS}
)

set_target_properties(integration_tests PROPERTIES
	BUILD_RPATH "${FLEDGE_LIB_DIR}"
)

# ---- SLC 500 integration tests (real libplctag, ab_server simulator) ----
#
# Separate binary from integration_tests: that one is pointed at a
# ControlLogix vPLC entirely through VPLC_* environment variables, while this
# one is pinned to the SLC 500 simulator managed by tests/vplc/vplc.sh.
# Every test skips itself if the simulator is not listening.

add_executable(slc500_tests
	${CMAKE_SOURCE_DIR}/plctag.cpp
	integration/test_slc500.cpp
)

target_include_directories(slc500_tests PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/integration
)

target_link_libraries(slc500_tests
	gtest
	gtest_main
	pthread
	${PLCTAG_LIB}
	${FLEDGE_LINK_LIBS}
)

set_target_properties(slc500_tests PROPERTIES
	BUILD_RPATH "${FLEDGE_LIB_DIR}"
)

# ---- CTest registration ----

add_test(NAME UnitTests COMMAND unit_tests)
add_test(NAME PluginLoadTests COMMAND plugin_load_tests)
add_test(NAME PluginInfoProbe COMMAND plugin_info_probe)
add_test(NAME IntegrationTests COMMAND integration_tests)
add_test(NAME SLC500Tests COMMAND slc500_tests)

# IntegrationTests needs a ControlLogix vPLC that has been pre-loaded with the
# tag names in vplc_config.h - something this repo cannot provision. Label it
# so the default `make test` run excludes it; `make test-vplc` runs it against
# whatever VPLC_ADDRESS points at. SLC500Tests stays in the default run because
# it brings its own simulator and skips itself when one is not listening.
set_tests_properties(IntegrationTests PROPERTIES LABELS "requires-vplc")
