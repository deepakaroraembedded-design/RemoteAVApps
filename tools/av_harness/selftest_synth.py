#!/usr/bin/env python3
"""
Harness self-test: generate synthetic telemetry for known-good and known-bad
21-minute single-stretch runs, and assert av_report.py returns the right
verdict AND the right primary_fault.

Run after ANY change to av_report.py or the gates:
    python3 tools/av_harness/selftest_synth.py            # fast (3-min scale)
    python3 tools/av_harness/selftest_synth.py --full     # true 21-min scale

The cases that matter most for a long single stretch are the ones a 60-second
test cannot see:
  * transient_minute_14  - one bad minute that the 21-minute aggregate hides
  * progressive_drift    - 0.9 ms/min, invisible in any single bucket
  * memory_leak          - RSS climbing across the run
  * early_eos            - stream died at minute 12 of 21
A gate that cannot detect its own injected fault is worse than no gate.
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPORT = os.path.join(HERE, "av_report.py")

FPS = 60.0      # the sourced 21-min clip is native 1080p60
RATE = 44100    # ...and AAC 44.1 kHz (the 24fps/48kHz legacy constants are gone)
PERIOD = 1024
FIFO_CAP = 8 * 1024 * 1024
BUCKET_S = 60.0
SEG_DUR_S = 1.0
START_NUM = 1000
# client monotonic -> CLOCK_REALTIME anchor. content timeline is anchored so that
# content_realtime(seg=START_NUM, frame 0) == realtime(ts=t0) -> healthy delay ~0.
T0 = 1_000_000_000
RT0 = 1_757_000_000_000_000


def gen(run_dir, span_s, *, av_skew_ms=0.0, av_drift_ms_per_min=0.0,
        drops=0, seg_fail=0, seg_slow_ms=None, xruns=0, vsync_miss=0,
        decoder_unavailable=False, decode_errors=0, empty=False,
        fifo_pct=50.0, resyncs=1, jitter=0.15,
        transient_bucket=None, transient_kind="xrun",
        rss_leak_mb=0.0, fd_leak=0, disk_leak_mb=0.0,
        eos=True, clip_duration_s=None, tier="full",
        present_delay_ms=0.0, present_delay_growth_ms_per_min=0.0):
    os.makedirs(run_dir, exist_ok=True)
    rnd = random.Random(1234)
    clip_duration_s = clip_duration_s if clip_duration_s is not None else span_s
    ev = []
    seq = [0]

    def emit(o):
        seq[0] += 1
        o["seq"] = seq[0]
        ev.append(o)

    t0 = T0
    emit({"t": "clock_sync", "ts": t0, "realtime_us": RT0,
          "host": "ai2", "role": "client", "build": "debug", "git": "deadbee"})

    frame_us = 1e6 / FPS
    n_frames = int(FPS * span_s)
    skipped = set(rnd.sample(range(10, n_frames - 10), drops)) if drops else set()

    a_period_us = PERIOD / RATE * 1e6
    n_periods = int(span_s * RATE / PERIOD)
    for i in range(n_periods):
        ts = t0 + int(i * a_period_us)
        lvl = FIFO_CAP * fifo_pct / 100.0
        emit({"t": "arender", "ts": ts, "pcm_frames": PERIOD,
              "hw_pos_frames": i * PERIOD, "trigger_us": t0,
              "delay_frames": 2304, "render_us": ts + int(2304 / RATE * 1e6),
              "content_pts_us": int(i * a_period_us),
              "fifo_level_bytes": int(lvl), "adelta_ppm": 0, "pad_samples": 0})

    for i in range(n_frames):
        if i in skipped:
            continue
        t_s = i * frame_us / 1e6
        b = int(t_s // BUCKET_S)
        # ts drives bucket assignment + drop detection, so it MUST stay on the
        # content cadence. The present-delay injection is applied to vblank_us
        # only: that is what present_delay is computed from, while buckets and
        # intervals stay correct (constant shifts cancel in the intervals).
        ts = t0 + int(i * frame_us) + int(rnd.gauss(0, jitter) * 1000)
        drift = av_drift_ms_per_min * (t_s / 60.0)
        skew = av_skew_ms
        if transient_bucket is not None and b == transient_bucket \
                and transient_kind == "skew":
            skew += 95.0          # one bad minute out of 21
        apos = int(i * frame_us) - int((skew + drift) * 1000)
        vb = ts + int(present_delay_ms * 1000
                      + present_delay_growth_ms_per_min * (t_s / 60.0) * 1000)
        emit({"t": "vrender", "ts": ts, "frame_idx": i, "seg": START_NUM + int(t_s),
              "pts_us": int(i * frame_us), "deadline_us": t0 + int(i * frame_us),
              "submit_us": vb - 1200, "vblank_us": vb, "buf_idx": i % 5,
              "audio_pos_us": apos, "late_us": 0,
              "decode_us": int(rnd.gauss(7000, 900)), "conv_us": 1500, "repeat": 0})

    for i in range(int(span_s)):
        for kind in ("video", "audio"):
            total = seg_slow_ms * 1000 if seg_slow_ms else int(rnd.gauss(88000, 20000))
            emit({"t": "net", "ts": t0 + i * 1_000_000, "kind": kind, "seg": 1000 + i,
                  "url": "/chunk-stream-%05d.m4s" % (1000 + i), "status": 200,
                  "ttfb_us": 18000, "total_us": max(total, 1000), "bytes": 184320,
                  "retries": 0, "rtt_us": 3900, "stall_us": 0})
    for i in range(seg_fail):
        emit({"t": "net", "ts": t0 + i * 1_000_000, "kind": "video", "seg": 2000 + i,
              "url": "/x.m4s", "status": 0, "ttfb_us": 0, "total_us": 15_000_000,
              "bytes": 0, "retries": 3, "rtt_us": 4000, "stall_us": 15_000_000})

    for i in range(xruns):
        emit({"t": "buf", "ts": t0 + i * 3_000_000, "which": "audio_fifo",
              "event": "underflow", "level": 0, "cap": FIFO_CAP, "count": i + 1})
    if transient_bucket is not None and transient_kind == "xrun":
        base = t0 + int(transient_bucket * BUCKET_S * 1e6) + 5_000_000
        for i in range(3):
            emit({"t": "buf", "ts": base + i * 1_000_000, "which": "audio_fifo",
                  "event": "underflow", "level": 0, "cap": FIFO_CAP, "count": i + 1})

    for i in range(vsync_miss):
        emit({"t": "sync", "ts": t0 + i * 2_000_000, "kind": "vsync_miss",
              "detail": "deadline_exceeded", "us": 19840, "frame_idx": 100 + i})
    for i in range(resyncs):
        emit({"t": "sync", "ts": t0 + i * 1000, "kind": "resync", "detail": "startup"})
    if decoder_unavailable:
        emit({"t": "dec", "ts": t0, "kind": "decoder_unavailable", "codec": "h264_cuvid",
              "rc": -1, "msg": "decoder not found", "fatal": True})
    for i in range(decode_errors):
        emit({"t": "dec", "ts": t0 + i * 1_000_000, "kind": "decode_error",
              "codec": "h264_cuvid", "rc": -1094995529, "msg": "Invalid data",
              "frame_idx": 200 + i, "fatal": False})
    if eos:
        emit({"t": "eos", "ts": t0 + int(span_s * 1e6) - 1000, "reason": "end_of_content"})

    ev.sort(key=lambda e: e["ts"])
    with open(os.path.join(run_dir, "vmc_tlm_client.ndjson"), "w") as fh:
        if not empty:
            for e in ev:
                fh.write(json.dumps(e) + "\n")
    with open(os.path.join(run_dir, "vmc_tlm_server.ndjson"), "w") as fh:
        fh.write(json.dumps({"t": "srv", "ts": t0, "seq": 1, "kind": "mpd_update",
                             "start_number": START_NUM,
                             "avail_start_us": RT0}) + "\n")
    # resource samples, one per minute
    with open(os.path.join(run_dir, "resources.ndjson"), "w") as fh:
        n = max(int(span_s // 60), 1)
        for i in range(n + 1):
            frac = i / float(n)
            fh.write(json.dumps({
                "t": "res", "elapsed_s": i * 60,
                "rss_kb": int(260000 + rss_leak_mb * 1024 * frac),
                "fds": int(42 + fd_leak * frac),
                "disk_mb": round(180.0 + disk_leak_mb * frac, 1)}) + "\n")

    open(os.path.join(run_dir, "dash_client.log"), "w").write(
        "dash dbg: seg ok=120 fail=0 drop=0 resync=1 xrun=0 pad=0\n")
    open(os.path.join(run_dir, "dash_server.log"), "w").write("server up\n")
    json.dump({"run_id": os.path.basename(run_dir), "tier": tier,
               "window_actual_s": span_s, "warmup_s": 15, "window_s": span_s,
               "tail_exclude_s": 10, "bucket_s": BUCKET_S, "content_fps": FPS,
               "audio_rate": RATE, "audio_period": PERIOD,
               "audio_fifo_bytes": FIFO_CAP, "seg_duration_s": 1.0,
               "clip_duration_s": clip_duration_s, "play_once": 1,
               "aborted_early": 0, "commit": "synth"},
              open(os.path.join(run_dir, "meta.json"), "w"))


def run_report(run_dir):
    p = subprocess.run([sys.executable, REPORT, "--run-dir", run_dir],
                       capture_output=True, text=True)
    try:
        return json.loads(p.stdout)
    except json.JSONDecodeError:
        print(p.stdout[-3000:], p.stderr[-3000:])
        raise


# (name, kwargs, want_verdict, want_primary_fault)
CASES = [
    ("healthy",             dict(),                                "PASS",  "none"),
    ("av_skew_120ms",       dict(av_skew_ms=120.0),                "FAIL",  "av_sync"),
    ("frame_drops",         dict(drops=25),                        "FAIL",  "cadence"),
    ("network_fail",        dict(seg_fail=6),                      "FAIL",  "network"),
    ("network_slow",        dict(seg_slow_ms=1400),                "FAIL",  "network"),
    ("audio_underflow",     dict(xruns=3),                         "FAIL",  "buffers"),
    ("vsync_miss",          dict(vsync_miss=4),                    "FAIL",  "vsync_async"),
    ("decode_errors",       dict(decode_errors=5),                 "FAIL",  "decoder_errors"),
    ("decoder_unavailable", dict(decoder_unavailable=True),        "ERROR", "decoder_unavailable"),
    ("no_telemetry",        dict(empty=True),                      "ERROR", "environment"),
    ("fifo_low",            dict(fifo_pct=8.0),                    "FAIL",  "buffers"),
    # --- long-stretch specific: a 60-second test cannot see any of these ---
    ("transient_xrun_min2", dict(transient_bucket=2),              "FAIL",  "buffers"),
    ("transient_skew_min2", dict(transient_bucket=2,
                                 transient_kind="skew"),           "FAIL",  "av_sync"),
    ("progressive_drift",   dict(av_drift_ms_per_min=0.9),         "FAIL",  "longrun"),
    ("memory_leak",         dict(rss_leak_mb=120.0),               "FAIL",  "longrun"),
    ("fd_leak",             dict(fd_leak=40),                      "FAIL",  "longrun"),
    ("disk_unbounded",      dict(disk_leak_mb=900.0),              "FAIL",  "longrun"),
    ("no_eos",              dict(eos=False),                       "FAIL",  "longrun"),
    ("early_eos",           dict(clip_duration_s=None),            "PASS",  "none"),
    # --- latency: A/V stays locked while the live-edge lag is broken ---
    ("present_delay_high",  dict(present_delay_ms=35000.0),        "FAIL",  "latency"),
    ("present_delay_growth", dict(present_delay_growth_ms_per_min=15.0),
                                                                   "FAIL",  "latency"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true",
                    help="run at true 21-minute scale (~130k events per case)")
    args = ap.parse_args()
    span = 1235.0 if args.full else 445.0

    tmp = tempfile.mkdtemp(prefix="av_selftest_")
    failures = 0
    print("span=%.0fs (%d buckets)  cases=%d" % (span, int(span // 60), len(CASES)))
    print("%-22s %-8s %-20s %s" % ("case", "verdict", "primary_fault", "result"))
    print("-" * 78)
    try:
        for name, kw, want_v, want_f in CASES:
            d = os.path.join(tmp, name)
            gen(d, span, **kw)
            r = run_report(d)
            got_v, got_f = r["verdict"], r["primary_fault"]
            ok = (got_v == want_v and got_f == want_f)
            if not ok:
                failures += 1
            extra = ""
            if not ok:
                extra = "MISMATCH (want %s/%s) gates=%s" % (
                    want_v, want_f, [g["gate"] for g in r["failed_gates"]][:4])
            else:
                bs = r.get("longrun", {})
                extra = "ok  buckets %s/%s" % (bs.get("buckets_passed"), bs.get("buckets_total"))
            print("%-22s %-8s %-20s %s" % (name, got_v, got_f, extra))

        # A transient must be pinned to the minute it happened, and the run
        # aggregate must NOT be what catches it.
        d = os.path.join(tmp, "transient_check")
        gen(d, span, transient_bucket=2)
        r = run_report(d)
        bad = [b["index"] for b in r["buckets"] if b["verdict"] == "FAIL"]
        if bad != [2]:
            print("localisation: FAIL - failing buckets %s, expected [2]" % bad)
            failures += 1
        else:
            print("localisation: ok - fault pinned to bucket 2 only")

        print("-" * 78)
        print("%d/%d checks correct" % (len(CASES) + 1 - failures, len(CASES) + 1))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
