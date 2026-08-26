#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
MODULE="${MODULE:-src/lockless_nic.ko}"
SIGNING_KEY="${SIGNING_KEY:-}"
SIGNING_CERT="${SIGNING_CERT:-}"
HASH_ALG="${HASH_ALG:-sha256}"

if [[ -z "$SIGNING_KEY" || -z "$SIGNING_CERT" ]]; then
	echo "SIGNING_KEY and SIGNING_CERT must be provided" >&2
	exit 1
fi
if [[ ! -x "$KDIR/scripts/sign-file" ]]; then
	echo "missing target sign-file helper: $KDIR/scripts/sign-file" >&2
	exit 1
fi
if [[ ! -f "$SIGNING_KEY" || ! -f "$SIGNING_CERT" ]]; then
	echo "signing key or certificate does not exist" >&2
	exit 1
fi
if [[ ! -f "$MODULE" ]]; then
	echo "module does not exist: $MODULE" >&2
	exit 1
fi

"$KDIR/scripts/sign-file" "$HASH_ALG" "$SIGNING_KEY" "$SIGNING_CERT" "$MODULE"
modinfo "$MODULE" | grep -E '^(sig_id|sig_key|sig_hashalgo):' || {
	echo "module signature metadata was not detected" >&2
	exit 1
}
printf 'signed module: %s\n' "$MODULE"
