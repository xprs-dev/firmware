#!/bin/sh
# The QoS harness (spec QoS.md 8) on the firmware's bearer: the bench trace
# replayed at x1 and x4, every pass criterion checked. Same trace as core's
# test/xprs_qos_replay_test.dart.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -DXB_HOST_TEST -I. -I"$XPRS" \
    -o /tmp/test_xbqos_replay \
    xprsbearer.c xb_airtime.c xb_qos.c "$XPRS"/xprs.c test_xbqos_replay_host.c "$SHA"
python3 -I -c '
import gzip, json, sys
f = gzip.open(sys.argv[1], "rt", encoding="utf-8")
for l in f:
    j = json.loads(l)
    if "start_utc_ms" in j:
        print("#start %d" % j["start_utc_ms"]); continue
    print("%d\t%s\t%s\t%s" % (j["t"], j["bearer"], j["rssi"], j["wire"]))
' fixtures/qos_bench_2026-11-25.jsonl.gz > /tmp/xbqos_trace.tsv
/tmp/test_xbqos_replay 1 < /tmp/xbqos_trace.tsv
/tmp/test_xbqos_replay 4 < /tmp/xbqos_trace.tsv
