# Top-level convenience Makefile for fledge-south-ethernetip.
#
# The plugin itself is built with CMake; these targets wrap the common
# workflows so they can be driven with plain `make`. The Debian package is
# built by the shared Fledge plugin workflow (.github/workflows/build-deb.yml);
# this repo carries only the packaging metadata (Package, Description, VERSION,
# fledge.version, requirements.sh).
#
#   make            Build the plugin shared library (libethernetip.so)
#   make install    Install the plugin into $(FLEDGE_ROOT)/plugins/south
#   make test       Build and run the self-contained test suites
#   make test-vplc  Run the ControlLogix suite against an external vPLC
#   make test-slc500  Run the SLC 500 tests against a simulator started here
#   make vplc-start Start the virtual SLC 500 (leaves it running)
#   make vplc-stop  Stop the virtual SLC 500
#   make deb        Build the Debian package (.deb) with the same scripts as CI
#   make clean      Remove build and packaging output
#
# FLEDGE_ROOT may be set on the command line or in the environment and is
# forwarded to CMake, e.g.  make FLEDGE_ROOT=/path/to/fledge
# FLEDGE_REPO points at a checkout of the fledge repo that holds the shared
# packaging scripts in .ci/plugin-deb (default: the sibling ../fledge).
# DEB_IMAGE and DEB_PLATFORM pick the build image, as in the CI matrix.

BUILD_DIR   := build
FLEDGE_ROOT ?=
FLEDGE_REPO ?= ../fledge
DEB_IMAGE   ?= ubuntu:22.04
DEB_PLATFORM ?= linux/amd64

ifeq ($(strip $(FLEDGE_ROOT)),)
CMAKE_FLAGS :=
else
CMAKE_FLAGS := -DFLEDGE_ROOT=$(FLEDGE_ROOT)
endif

VPLC        := tests/vplc/vplc.sh

.PHONY: all build install test test-vplc test-slc500 vplc-start vplc-stop vplc-status deb clean

all: build

build:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake $(CMAKE_FLAGS) .. && $(MAKE)

install: build
	cd $(BUILD_DIR) && $(MAKE) install

# Excludes the ControlLogix integration suite, which needs a vPLC pre-loaded
# with the tag names in tests/integration/vplc_config.h. Run that with
# `make test-vplc` once VPLC_ADDRESS points at a suitably configured PLC.
test:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake $(CMAKE_FLAGS) -DBUILD_TESTS=ON .. && $(MAKE)
	cd $(BUILD_DIR) && ctest --output-on-failure -LE requires-vplc

test-vplc:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake $(CMAKE_FLAGS) -DBUILD_TESTS=ON .. && $(MAKE) integration_tests
	cd $(BUILD_DIR) && ctest --output-on-failure -L requires-vplc

# The SLC 500 tests skip themselves when no simulator is listening, so plain
# `make test` stays green without one. This target brings the simulator up
# around the run and takes it down again even if the tests fail.
test-slc500:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake $(CMAKE_FLAGS) -DBUILD_TESTS=ON .. && $(MAKE) slc500_tests
	$(VPLC) start
	@rc=0; $(BUILD_DIR)/tests/slc500_tests || rc=$$?; $(VPLC) stop; exit $$rc

vplc-start:
	$(VPLC) start

vplc-stop:
	$(VPLC) stop

vplc-status:
	@$(VPLC) status

# Needs Docker with buildx. Clones FLEDGE_REPO into ./fledge, which the full
# build compiles, exactly as CI does. Output goes to build-out/.
deb:
	@test -d $(FLEDGE_REPO)/.ci/plugin-deb/cpp || { \
	  echo "Shared packaging scripts not found at FLEDGE_REPO=$(FLEDGE_REPO)."; \
	  echo "Clone it:  git clone https://github.com/fledge-iot/fledge $(FLEDGE_REPO)"; \
	  exit 1; }
	rm -rf .ci/deb && mkdir -p .ci/deb
	cp -R $(FLEDGE_REPO)/.ci/plugin-deb/cpp/. .ci/deb/
	chmod +x .ci/deb/*.sh
	rm -rf fledge && git clone --depth 1 file://$(abspath $(FLEDGE_REPO)) fledge
	.ci/deb/build.sh $(DEB_IMAGE) $(DEB_PLATFORM)

# Stop the simulator first: its pidfile lives under $(BUILD_DIR), so removing
# that directory with the process still up would orphan it.
# Note this also discards the cached libplctag checkout, so the next
# `make test-slc500` re-fetches it.
clean:
	-@$(VPLC) stop 2>/dev/null || true
	rm -rf $(BUILD_DIR) build-out .ci fledge
