#!/usr/bin/env bash
# Run the publisher and both subscribers on one host and print RESULT lines.
#
#   scripts/bench.sh [build_dir=build] [ticks=20000] [interval_us=100]
#
# The publisher waits 1.5 s so both subscribers can attach, then sends
# `ticks` ticks, one every `interval_us`, to shared memory and TCP at once.
# Each subscriber drops its first 1,000 samples as warm-up and reports
# nearest-rank percentiles of (receive - send) on CLOCK_MONOTONIC.
set -euo pipefail
B=${1:-build}; N=${2:-20000}; I=${3:-100}; W=1000
LOG=$(mktemp -d)

"$B/publisher" "$N" "$I" 1500 > "$LOG/pub.log" 2>&1 &
PUB=$!
sleep 0.5
"$B/tcp_subscriber" "$N" "$W" > "$LOG/tcp.log" 2>&1 &
TCP=$!
"$B/shm_subscriber" "$N" "$W" > "$LOG/shm.log" 2>&1 || { echo "shm_subscriber failed:"; tail -5 "$LOG/shm.log"; }
wait "$TCP" || true
wait "$PUB" || true

echo "# host: $(uname -sm), $(getconf _NPROCESSORS_ONLN) CPUs, $(date -u +%Y-%m-%dT%H:%MZ)"
echo "# workload: $N ticks, one every ${I}us, first $W samples per path dropped as warm-up"
grep -h '^RESULT' "$LOG/shm.log" "$LOG/tcp.log" || { echo "no RESULT lines"; tail -5 "$LOG"/*.log; exit 1; }
