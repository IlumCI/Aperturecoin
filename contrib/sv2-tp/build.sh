#!/usr/bin/env bash
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Build the ApertureCoin Stratum V2 Template Provider (sv2-tp) and the
# reference SV2 CPU miner (aperture-sv2-miner) from the pinned upstream
# stratum-mining/sv2-tp commit plus the ApertureCoin patch and overlay.
#
# Usage: contrib/sv2-tp/build.sh [build-dir]
# Requires: git, cmake >= 3.22, a C++20 compiler, capnproto + libcapnp-dev,
#           libboost headers.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APERTURE_SRC="$(cd "${HERE}/../.." && pwd)"
WORK="${1:-${HERE}/work}"
UPSTREAM_URL="https://github.com/stratum-mining/sv2-tp.git"
UPSTREAM_COMMIT="$(cat "${HERE}/UPSTREAM_COMMIT")"

if [ ! -d "${WORK}/.git" ]; then
    git clone --quiet "${UPSTREAM_URL}" "${WORK}"
fi
git -C "${WORK}" fetch --quiet origin "${UPSTREAM_COMMIT}" || true
git -C "${WORK}" checkout --quiet --force "${UPSTREAM_COMMIT}"
git -C "${WORK}" clean --quiet -fdx -e build

git -C "${WORK}" apply "${HERE}"/patches/*.patch
cp -R "${HERE}/overlay/." "${WORK}/"

cmake -S "${WORK}" -B "${WORK}/build" -DAPERTURE_SRC="${APERTURE_SRC}" -DWITH_CCACHE=OFF -DBUILD_TESTS=ON
cmake --build "${WORK}/build" -j"$(nproc 2>/dev/null || echo 4)" --target sv2-tp aperture-sv2-miner test_sv2

echo
echo "Built:"
echo "  ${WORK}/build/bin/sv2-tp"
echo "  ${WORK}/build/bin/aperture-sv2-miner"
echo "  ${WORK}/build/bin/test_sv2"
