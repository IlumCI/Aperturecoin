#!/usr/bin/env bash
# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Build the ApertureCoin fork of the Stratum V2 reference applications (SRI
# pool, JD server, JD client, translator, CPU mining device) from the pinned
# upstream stratum-mining/sv2-apps and stratum-mining/stratum commits plus the
# ApertureCoin patches (ApertureMatMul share validation, ApertureCoin
# addresses). See README.md.
#
# Usage: contrib/sri-pool/build.sh [work-dir]
# Requires: git, a Rust toolchain (rustup honours sv2-apps' rust-toolchain.toml).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${1:-${HERE}/work}"
SV2_APPS="$(sed -n 's/^SV2_APPS=//p' "${HERE}/UPSTREAM_COMMITS")"
STRATUM="$(sed -n 's/^STRATUM=//p' "${HERE}/UPSTREAM_COMMITS")"

fetch() { # url dir commit
    if [ ! -d "$2/.git" ]; then
        git clone --quiet "$1" "$2"
    fi
    git -C "$2" fetch --quiet origin "$3" || true
    git -C "$2" checkout --quiet --force "$3"
    git -C "$2" clean --quiet -fdx -e target
}

fetch https://github.com/stratum-mining/sv2-apps.git "${WORK}/sv2-apps" "${SV2_APPS}"
fetch https://github.com/stratum-mining/stratum.git "${WORK}/stratum" "${STRATUM}"

APPS="${WORK}/sv2-apps"
git -C "${APPS}" apply "${HERE}/patches/0001-sv2-apps-aperture.patch"
mkdir -p "${APPS}/aperture/stratum-patches"
cp -R "${WORK}/stratum/sv2/channels-sv2" "${APPS}/aperture/stratum-patches/channels-sv2"
git -C "${APPS}" apply --directory=aperture/stratum-patches/channels-sv2 "${HERE}/patches/0002-channels-sv2-aperture.patch"

cd "${APPS}"
cargo build --release -p pool_sv2 -p jd_server_sv2 -p jd_client_sv2 -p translator_sv2 --bins
cargo build --release -p integration_tests_sv2 --bin mining_device

echo
echo "Built (in ${CARGO_TARGET_DIR:-${APPS}/target}/release):"
echo "  pool_sv2 jd_server_sv2 jd_client_sv2 translator_sv2 mining_device"
