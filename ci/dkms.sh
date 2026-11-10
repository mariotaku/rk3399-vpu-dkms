#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me>
#
# Builds the DKMS .deb as README.md describes, installs it with Debian's
# arm64 kernel headers, has DKMS build and install the module for the
# headers' kernel (not the running one) and checks the result with
# modinfo. Runs as root in a debian:trixie arm64 container from the
# source tree; the .deb goes to OUTDIR.
#
# Usage: ci/dkms.sh OUTDIR [TAG]
# With TAG (v<version>), fails unless it names debian/changelog's version
# and writes that version's changelog entry to OUTDIR/notes.txt.

set -eu

[ $# -ge 1 ] || { echo "usage: $0 OUTDIR [TAG]" >&2; exit 2; }
out=$1
tag=${2:-}
mkdir -p "$out"
out=$(cd "$out" && pwd)
cd "$(dirname "$0")/.."

fail() {
	echo "error: $*" >&2
	exit 1
}

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends dpkg-dev
apt-get build-dep -y --no-install-recommends ./

package=$(dpkg-parsechangelog -S Source)
version=$(dpkg-parsechangelog -S Version)
if [ -n "$tag" ]; then
	[ "$tag" = "v$version" ] || fail "tag $tag does not match debian/changelog version $version"
	dpkg-parsechangelog -S Changes | tail -n +4 > "$out/notes.txt"
fi

echo "::group::Build ${package} ${version}"
dpkg-buildpackage -us -uc -b
deb=${package}_${version}_all.deb
cp "../$deb" "$out/"
echo "::endgroup::"

echo "::group::Install the package and linux-headers-arm64"
apt-get install -y --no-install-recommends "$out/$deb"
apt-get install -y --no-install-recommends linux-headers-arm64
echo "::endgroup::"

# The kernel version the metapackage points at, e.g. 6.12.57+deb13-arm64.
kver=$(dpkg-query -W -f '${Depends}' linux-headers-arm64 |
	sed -n 's/^linux-headers-\([^ ,]*\).*/\1/p')
[ -n "$kver" ] || fail "no kernel version from linux-headers-arm64"
[ -d "/usr/lib/modules/$kver/build" ] || fail "no headers at /usr/lib/modules/$kver/build"

echo "::group::DKMS build and install for $kver"
dkms_name=$(sed -n 's/^PACKAGE_NAME="\(.*\)"$/\1/p' dkms.conf)
# The headers' kernel hook may already have built the module; --force
# builds it again here so that this step's log shows the build.
dkms build --force -m "$dkms_name" -v "$version" -k "$kver" ||
	{ cat "/var/lib/dkms/$dkms_name/$version/build/make.log" >&2 || true; exit 1; }
dkms install --force -m "$dkms_name" -v "$version" -k "$kver"
dkms status
echo "::endgroup::"

ko=$(find "/usr/lib/modules/$kver/updates/dkms" -name 'hantro-vpu.ko*' | head -n 1)
[ -n "$ko" ] || fail "no hantro-vpu module under /usr/lib/modules/$kver/updates/dkms"
echo "module: $ko"
modinfo "$ko"

field() {
	modinfo -F "$1" "$ko"
}

[ "$(field name)" = hantro_vpu ] || fail "module name is not hantro_vpu"
[ "$(field vermagic | cut -d ' ' -f 1)" = "$kver" ] || fail "vermagic does not start with $kver"
[ "$(field license)" = "GPL v2" ] || fail "licence is not GPL v2"
for alias in 'of:N*T*Crockchip,rk3399-vpu' 'of:N*T*Crockchip,rk3399-vpuC*'; do
	field alias | grep -qxF "$alias" || fail "alias $alias missing"
done
for parm in debug hevc_use_compression; do
	field parm | grep -q "^$parm:" || fail "parameter $parm missing"
done

# The H.264 encoder is linked in: its entry points are in the symbol table.
obj="$out/hantro-vpu.ko"
case "$ko" in
*.xz) xz -dc "$ko" > "$obj" ;;
*.zst) zstd -dcq "$ko" > "$obj" ;;
*.gz) gzip -dc "$ko" > "$obj" ;;
*) cp "$ko" "$obj" ;;
esac
for sym in rockchip_vpu2_h264_enc_run hantro_h264_enc_prepare; do
	nm "$obj" | grep -q " $sym\$" || fail "symbol $sym missing"
done
rm -f "$obj"

echo "ok: $deb builds hantro-vpu for $kver"
