#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

NS_NAME="${LNIC_NETNS:-lockless-nic-ns}"
LNIC_IFACE="${LNIC_IFACE:-lnic0}"
VETH_HOST="${VETH_HOST:-lnic-host}"
VETH_PEER="${VETH_PEER:-lnic-peer}"
LNIC_ADDR="${LNIC_ADDR:-192.0.2.1/24}"
PEER_ADDR="${PEER_ADDR:-192.0.2.2/24}"

if [[ ${EUID} -ne 0 ]]; then
	echo "setup_netns.sh must be run as root" >&2
	exit 1
fi

cleanup() {
	set +e
	ip netns del "$NS_NAME" 2>/dev/null || true
	ip link del "$VETH_HOST" 2>/dev/null || true
	modprobe -r lockless_nic 2>/dev/null || true
}
trap cleanup EXIT

modprobe lockless_nic ring_order="${LNIC_RING_ORDER:-12}" loopback=1
ip netns add "$NS_NAME"
ip link add "$VETH_HOST" type veth peer name "$VETH_PEER"
ip link set "$VETH_PEER" netns "$NS_NAME"
ip link set "$LNIC_IFACE" netns "$NS_NAME"

ip link set "$VETH_HOST" up
ip addr add 198.51.100.1/24 dev "$VETH_HOST"
ip netns exec "$NS_NAME" ip link set lo up
ip netns exec "$NS_NAME" ip link set "$VETH_PEER" up
ip netns exec "$NS_NAME" ip addr add "$PEER_ADDR" dev "$VETH_PEER"
ip netns exec "$NS_NAME" ip link set "$LNIC_IFACE" up
ip netns exec "$NS_NAME" ip addr add "$LNIC_ADDR" dev "$LNIC_IFACE"

printf 'Namespace %s is ready.\n' "$NS_NAME"
printf '  virtual NIC: %s (%s)\n' "$LNIC_IFACE" "$LNIC_ADDR"
printf '  veth peer:   %s (%s)\n' "$VETH_PEER" "$PEER_ADDR"
printf '  host peer:   %s (198.51.100.1/24)\n' "$VETH_HOST"
printf 'Press Ctrl-C to tear down the isolated environment.\n'
while :; do sleep 3600; done
