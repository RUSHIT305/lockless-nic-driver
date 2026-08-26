#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

MODE="${MODE:-iperf3}"
IFACE="${IFACE:-lnic0}"
DURATION="${DURATION:-30}"
TARGET="${TARGET:-}"
SOURCE_ADDR="${SOURCE_ADDR:-}"
PACKETS="${PACKETS:-1000000}"
PKT_SIZE="${PKT_SIZE:-64}"

usage() {
	cat <<'EOF'
Usage: MODE=iperf3 TARGET=192.0.2.2 ./scripts/test_throughput.sh
       MODE=pktgen IFACE=lnic0 PACKETS=1000000 ./scripts/test_throughput.sh

Environment:
  IFACE       interface to observe or benchmark (default: lnic0)
  DURATION    iperf3 duration in seconds (default: 30)
  TARGET      iperf3 server address; required in iperf3 mode
  SOURCE_ADDR optional local address passed to iperf3 with -B
  PACKETS     pktgen packet count (default: 1000000)
  PKT_SIZE    pktgen packet size in bytes (default: 64)
EOF
}

if [[ ${1:-} == "--help" || ${1:-} == "-h" ]]; then
	usage
	exit 0
fi

if ! ip link show "$IFACE" >/dev/null 2>&1; then
	echo "interface $IFACE does not exist" >&2
	exit 1
fi

printf 'Counters before benchmark:\n'
ip -s link show "$IFACE"

case "$MODE" in
iperf3)
	if [[ -z "$TARGET" ]]; then
		echo "TARGET is required for MODE=iperf3" >&2
		exit 1
	fi
	command -v iperf3 >/dev/null 2>&1 || {
		echo "iperf3 is not installed" >&2
		exit 1
	}
	cmd=(iperf3 -c "$TARGET" -t "$DURATION" -i 1)
	if [[ -n "$SOURCE_ADDR" ]]; then
		cmd+=( -B "$SOURCE_ADDR" )
	fi
	"${cmd[@]}"
	;;
pktgen)
	[[ ${EUID} -eq 0 ]] || { echo "pktgen mode requires root" >&2; exit 1; }
	[[ -d /proc/net/pktgen ]] || { echo "kernel pktgen is unavailable" >&2; exit 1; }
	modprobe pktgen
	echo rem_device_all > /proc/net/pktgen/pgctrl
	echo "add_device=$IFACE" > /proc/net/pktgen/kpktgend_0
	echo "count $PACKETS" > "/proc/net/pktgen/$IFACE"
	echo "pkt_size $PKT_SIZE" > "/proc/net/pktgen/$IFACE"
	echo "clone_skb 0" > "/proc/net/pktgen/$IFACE"
	echo start > /proc/net/pktgen/pgctrl
	cat "/proc/net/pktgen/$IFACE"
	;;
*)
	echo "unsupported MODE=$MODE; choose iperf3 or pktgen" >&2
	exit 1
	;;
esac

printf '\nCounters after benchmark:\n'
ip -s link show "$IFACE"
