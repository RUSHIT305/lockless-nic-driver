#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
MODULE="${MODULE:-src/lockless_nic.ko}"
REQUIRE_SIGNATURE="${REQUIRE_SIGNATURE:-1}"

if [[ ! -d "$KDIR" ]]; then
	echo "missing target kernel build tree: $KDIR" >&2
	exit 1
fi
if [[ ! -f "$KDIR/.config" ]]; then
	echo "target kernel configuration is missing: $KDIR/.config" >&2
	exit 1
fi
if [[ ! -f "$KDIR/Module.symvers" ]]; then
	echo "target Module.symvers is missing; production modpost cannot be trusted" >&2
	exit 1
fi

make -C src clean KDIR="$KDIR"
make -C src KDIR="$KDIR" W=1 KBUILD_MODPOST_WARN=0

if [[ ! -f "$MODULE" ]]; then
	echo "module was not produced: $MODULE" >&2
	exit 1
fi

TARGET_RELEASE="$(make -s -C "$KDIR" kernelrelease)"
MODULE_VERMAGIC="$(modinfo -F vermagic "$MODULE")"
if [[ "$MODULE_VERMAGIC" != "$TARGET_RELEASE"* ]]; then
	echo "vermagic mismatch: module=$MODULE_VERMAGIC target=$TARGET_RELEASE" >&2
	exit 1
fi

if [[ "$REQUIRE_SIGNATURE" == "1" ]]; then
	SIGNATURE_ID="$(modinfo -F sig_id "$MODULE" || true)"
	if [[ -z "$SIGNATURE_ID" || "$SIGNATURE_ID" == "~Module signature appended~" ]]; then
		echo "production policy requires a cryptographically signed module" >&2
		exit 1
	fi
fi

printf 'production gate passed: %s for %s\n' "$MODULE" "$TARGET_RELEASE"
