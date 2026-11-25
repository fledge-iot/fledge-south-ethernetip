#!/usr/bin/env bash

##--------------------------------------------------------------------
## Copyright (c) 2026 Industrial Intelligence Group, LLC
##
## Licensed under the Apache License, Version 2.0 (the "License");
## you may not use this file except in compliance with the License.
## You may obtain a copy of the License at
##
##     http://www.apache.org/licenses/LICENSE-2.0
##
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS,
## WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
## See the License for the specific language governing permissions and
## limitations under the License.
##--------------------------------------------------------------------

##
## Build-time requirements for fledge-south-ethernetip.
##
## Installs libplctag from source into /usr/local (the version the plugin links
## against). The package build later bundles the resulting shared object into
## /usr/local/fledge/lib. If a suitable libplctag is already installed this is a
## no-op.
##

set -e

# libplctag release the plugin is built and bundled against, pinned by commit
# so a moved tag cannot change what is built. v2.7.2 includes the fixes for
# the 2.6.x / 2.7.0 / 2.7.1 CIP response parsing advisories. Override both
# variables together to build a different release.
LIBPLCTAG_VERSION="${LIBPLCTAG_VERSION:-2.7.2}"
LIBPLCTAG_COMMIT="${LIBPLCTAG_COMMIT:-7712ed7f1ee05ba0ebfca641b3f5c43da52a66ed}"

os_name=$( (grep -o '^NAME=.*' /etc/os-release | cut -f2 -d\" | sed 's/"//g') 2>/dev/null || echo unknown)
os_version=$( (grep -o '^VERSION_ID=.*' /etc/os-release | cut -f2 -d\" | sed 's/"//g') 2>/dev/null || echo unknown)
echo "Platform is ${os_name}, Version: ${os_version}"

# Already installed at exactly this release? Then skip. An older libplctag
# (for example 2.6.x) is not accepted: it has known security fixes missing.
if [ -e "/usr/local/lib/libplctag.so.${LIBPLCTAG_VERSION}" ]; then
    echo "libplctag ${LIBPLCTAG_VERSION} already present in /usr/local/lib - skipping build."
    exit 0
fi

echo "Building libplctag ${LIBPLCTAG_VERSION} from source..."

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT

(
    cd "${workdir}"
    git init -q libplctag
    cd libplctag
    git fetch -q --depth 1 https://github.com/libplctag/libplctag.git "${LIBPLCTAG_COMMIT}"
    git checkout -q FETCH_HEAD
    if [ "$(git rev-parse HEAD)" != "${LIBPLCTAG_COMMIT}" ]; then
        echo "libplctag checkout does not match ${LIBPLCTAG_COMMIT}" >&2
        exit 1
    fi
    # Build only the library the plugin links against. libplctag's tools and
    # proof-of-concept programs (src/tools, src/poc) are always built otherwise,
    # and some fail on older toolchains: the vendored libyafl needs
    # MAP_ANONYMOUS under strict C11 (jammy, bookworm), and modbus_server
    # static-links librt (bullseye). Neither is used by this plugin.
    sed -i -e '/add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}\/src\/tools")/d' \
           -e '/add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}\/src\/poc")/d' CMakeLists.txt
    if grep -q 'src/poc\|src/tools' CMakeLists.txt; then
        echo "Could not remove libplctag tools/poc from the build" >&2
        exit 1
    fi
    mkdir -p build
    cd build
    cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=0 -DBUILD_EXAMPLES=0 \
          -DCMAKE_INSTALL_PREFIX=/usr/local ..
    make
    sudo make install
)

# Make the freshly installed library visible to the dynamic linker.
if command -v ldconfig >/dev/null 2>&1; then
    sudo ldconfig || true
fi

echo "libplctag ${LIBPLCTAG_VERSION} installed."
