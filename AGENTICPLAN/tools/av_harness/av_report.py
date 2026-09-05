#!/usr/bin/env python3
"""
av_report.py - reduce ONE 21-minute single-stretch playback run to metrics,
apply the pass gates per-minute and run-wide, and emit av_report.json.

This file is the CONTRACT BOUNDARY between the two agents. The automation test
agent runs it and returns its output verbatim; the main agent reads only its
output. Nothing else crosses.

Design note for the long run
----------------------------
Gates are applied to EVERY complete 60 s bucket, not only to the run aggregate.
A 20-second glitch at minute 14 is invisible in a 21-minute mean but is exactly
the class of fault a single-stretch test exists to find. A run passes only if
all 21 buckets pass AND the run-level drift / leak / completion gates pass.

Usage:
    av_report.py --run-dir runs/<id> [--out av_report.json]
                 [--iteration 14] [--commit 9f2ac31] [--tier full|smoke]

Primary input:  <run-dir>/vmc_tlm_client.ndjson   (see docs/AV_TELEMETRY_SPEC.md)
Optional:       <run-dir>/vmc_tlm_server.ndjson, <run-dir>/resources.ndjson
Fallback:       <run-dir>/dash_client.log  -> DEGRADED, always ERROR
"""

import argparse
import json
import math
import os
import re
import sys

# --------------------------------------------------------------------------
# gates (env-overridable; defaults mirror tools/av_harness/env.sh)
# --------------------------------------------------------------------------


def _f(name, default):
    try:
        return float(os.environ.get(name, default))
    except (TypeError, ValueError):
        return float(default)


G = {
    # per bucket
    "av_offset_mean_abs_ms": _f("GATE_AV_OFFSET_MEAN_ABS_MS", 10.0),
    "av_offset_p95_abs_ms": _f("GATE_AV_OFFSET_P95_ABS_MS", 20.0),
    "av_offset_max_ms": _f("GATE_AV_OFFSET_MAX_MS", 40.0),
    "av_offset_min_ms": _f("GATE_AV_OFFSET_MIN_MS", -60.0),
    "av_envelope_violation_pct": _f("GATE_AV_ENVELOPE_VIOLATION_PCT", 0.1),
    "frame_yield_min": _f("GATE_FRAME_YIELD_MIN", 0.995),
    # Interval tolerance is a FRACTION of the content frame period, so the gate
    # scales with content fps (16.7 ms @60 fps, 41.7 ms @24 fps).
    "frame_interval_frac": _f("GATE_FRAME_INTERVAL_FRAC", 0.10),
    "audio_period_tolerance": _f("GATE_AUDIO_PERIOD_TOLERANCE", 0.005),
    "audio_fifo_level_min_pct": _f("GATE_AUDIO_FIFO_LEVEL_MIN_PCT", 15.0),
    "seg_fetch_p95_ms": _f("GATE_SEG_FETCH_P95_MS", 800.0),
    "seg_fetch_max_ms": _f("GATE_SEG_FETCH_MAX_MS", 2000.0),
    "tcp_rtt_p95_ms": _f("GATE_TCP_RTT_P95_MS", 30.0),
    "decode_p95_ms": _f("GATE_DECODE_P95_MS", 15.0),
    # run level
    "av_drift_abs_ms_per_min": _f("GATE_AV_DRIFT_ABS_MS_PER_MIN", 0.5),
    "av_total_excursion_ms": _f("GATE_AV_TOTAL_EXCURSION_MS", 15.0),
    "av_p95_degrade_ms_per_bucket": _f("GATE_AV_P95_DEGRADE_MS_PER_BUCKET", 0.3),
    "resyncs_max": _f("GATE_RESYNCS_MAX", 1.0),
    "rss_growth_mb": _f("GATE_RSS_GROWTH_MB", 32.0),
    "fd_growth": _f("GATE_FD_GROWTH", 8.0),
    "disk_growth_mb": _f("GATE_DISK_GROWTH_MB", 512.0),
    "disk_flat_last_buckets": int(_f("GATE_DISK_FLAT_LAST_BUCKETS", 10)),
    "require_eos": int(_f("GATE_REQUIRE_EOS", 1)),
    "eos_tolerance_s": _f("GATE_EOS_TOLERANCE_S", 5.0),
    # latency (run level). p95 ceiling sits above the client's 10 s steady / 30 s
    # max live buffer; growth is Theil-Sen over per-bucket mean lag.
    "present_delay_p95_ms": _f("GATE_PRESENT_DELAY_P95_MS", 30000.0),
    "present_delay_growth_ms_per_min": _f("GATE_PRESENT_DELAY_GROWTH_MS_PER_MIN", 10.0),
}

# Causal order: primary_fault is the FIRST red class, not the loudest one.
#
# `longrun` sits BEFORE `av_sync` deliberately. Over 21 minutes a slow drift
# (or a leak) eventually pushes late buckets past the per-minute A/V gate, so a
# run with both red is a drift problem showing up as per-minute symptoms. If
# av_sync came first the Main Agent would go hunting for a static anchor offset
# when the actual defect is the rate-compensation servo.
FAULT_ORDER = [
    "environment",
    "decoder_unavailable",
    "network",
    "decoder_errors",
    "buffers",
    "vsync_async",
    "cadence",
    "longrun",
    "av_sync",
    "latency",
]

# --------------------------------------------------------------------------
# stats helpers (no numpy - the MEC host may not have it)
# --------------------------------------------------------------------------


def pct(values, p):
    if not values:
        return None
    s = sorted(values)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * (p / 100.0)
    lo, hi = math.floor(k), math.ceil(k)
    if lo == hi:
        return s[int(k)]
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def mean(values):
    return sum(values) / len(values) if values else None


def slope(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0:
        return None
    return sum((xs[i] - mx) * (ys[i] - my) for i in range(n)) / den


def median(v):
    if not v:
        return None
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def theil_sen(xs, ys):
    """
    Median of pairwise slopes. Used instead of least squares for the long-run
    drift estimate: a single bad minute out of 21 is a step, and a step near the
    start tilts an LS fit enough to look like drift. Theil-Sen ignores it, so
    "is this drifting?" and "did it glitch once?" stay separate questions.
    """
    n = len(xs)
    if n < 5:
        return None
    slopes = []
    for i in range(n):
        for j in range(i + 1, n):
            dx = xs[j] - xs[i]
            if dx:
                slopes.append((ys[j] - ys[i]) / dx)
    return median(slopes)


def r2(x, nd=2):
    return None if x is None else round(x, nd)


# --------------------------------------------------------------------------
# bucket accumulator
# --------------------------------------------------------------------------


class Bucket(object):
    __slots__ = ("index", "t_start_s", "span_s", "complete",
                 "v_count", "v_repeat", "v_drop", "v_last_idx",
                 "intervals", "last_vts", "decode_ms",
                 "offsets", "offset_times", "present_delays",
                 "a_count", "a_pad", "fifo_min_pct",
                 "buf", "sync", "dec",
                 "net_ok", "net_fail", "net_miss", "fetch_ms", "rtt_ms")

    def __init__(self, index, t_start_s, span_s, complete):
        self.index = index
        self.t_start_s = t_start_s
        self.span_s = span_s
        self.complete = complete
        self.v_count = 0
        self.v_repeat = 0
        self.v_drop = 0
        self.v_last_idx = None
        self.intervals = []
        self.last_vts = None
        self.decode_ms = []
        self.offsets = []
        self.offset_times = []
        self.present_delays = []
        self.a_count = 0
        self.a_pad = 0
        self.fifo_min_pct = None
        self.buf = {}
        self.sync = {}
        self.dec = {}
        self.net_ok = 0
        self.net_fail = 0
        self.net_miss = 0
        self.fetch_ms = []
        self.rtt_ms = []

    def bump(self, d, key):
        d[key] = d.get(key, 0) + 1


# --------------------------------------------------------------------------
# load
# --------------------------------------------------------------------------


def load_meta(run_dir):
    p = os.path.join(run_dir, "meta.json")
    if os.path.exists(p):
        try:
            with open(p) as fh:
                return json.load(fh)
        except json.JSONDecodeError:
            pass
    return {}


def iter_ndjson(path):
    """Stream: a 21-minute run is ~130k events / ~60 MB. Never load it all."""
    if not os.path.exists(path):
        return
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line[0] != "{":
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                yield {"t": "__bad__"}


# --------------------------------------------------------------------------
# pass 1: find the time base
# --------------------------------------------------------------------------


def find_t0(path):
    t0 = None
    t1 = None
    for e in iter_ndjson(path):
        ts = e.get("ts")
        if ts is None:
            continue
        if t0 is None or ts < t0:
            t0 = ts
        if t1 is None or ts > t1:
            t1 = ts
    return t0, t1


# --------------------------------------------------------------------------
# pass 2: fill buckets
# --------------------------------------------------------------------------


def realtime_of(rt_pairs, mono_us):
    """
    Map a client monotonic timestamp to CLOCK_REALTIME using the clock_sync
    pairings (1 Hz). Linear interpolation between consecutive pairs handles NTP
    steps; clamp at the ends.
    """
    if not rt_pairs:
        return None
    if mono_us <= rt_pairs[0][0]:
        return rt_pairs[0][1] + (mono_us - rt_pairs[0][0])
    if mono_us >= rt_pairs[-1][0]:
        return rt_pairs[-1][1] + (mono_us - rt_pairs[-1][0])
    lo, hi = 0, len(rt_pairs) - 1
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        if rt_pairs[mid][0] <= mono_us:
            lo = mid
        else:
            hi = mid
    (t_a, r_a), (t_b, r_b) = rt_pairs[lo], rt_pairs[hi]
    if t_b == t_a:
        return r_a
    return r_a + (r_b - r_a) * (mono_us - t_a) / (t_b - t_a)


def build_buckets(path, t0, span_s, bucket_s, fifo_cap, lat_ctx, fps):
    n_complete = int(span_s // bucket_s)
    buckets = [Bucket(i, i * bucket_s, bucket_s, True) for i in range(n_complete)]
    tail_span = span_s - n_complete * bucket_s
    tail = Bucket(n_complete, n_complete * bucket_s, tail_span, False)
    buckets.append(tail)

    bad = 0
    eos = None
    a_timeline = []          # (render_us, content_pts_us) for the fallback path
    need_timeline = True     # decided after the first few vrenders
    direct_seen = 0
    vr_seen = 0

    rt = lat_ctx.get("rt") or []
    avail = lat_ctx.get("avail_start_us")
    lat_ok = bool(rt and avail is not None)

    for e in iter_ndjson(path):
        t = e.get("t")
        if t == "__bad__":
            bad += 1
            continue
        ts = e.get("ts")
        if ts is None:
            continue
        rel = (ts - t0) / 1e6
        bi = int(rel // bucket_s)
        if bi < 0:
            continue
        if bi >= len(buckets):
            bi = len(buckets) - 1
        b = buckets[bi]

        if t == "vrender":
            vr_seen += 1
            b.v_count += 1
            if e.get("repeat"):
                b.v_repeat += 1
            fi = e.get("frame_idx")
            if fi is not None:
                if b.v_last_idx is not None and fi - b.v_last_idx > 1:
                    b.v_drop += (fi - b.v_last_idx - 1)
                b.v_last_idx = fi
            vts = e.get("vblank_us") or ts
            if b.last_vts is not None:
                b.intervals.append((vts - b.last_vts) / 1000.0)
            b.last_vts = vts
            if e.get("decode_us") is not None:
                b.decode_ms.append(e["decode_us"] / 1000.0)
            ap, pt = e.get("audio_pos_us"), e.get("pts_us")
            if ap is not None and pt is not None:
                direct_seen += 1
                b.offsets.append((pt - ap) / 1000.0)
                b.offset_times.append(rel)
            # Presentation latency: wall-clock at scanout minus the content
            # realtime timestamp. content_realtime = avail_start_us + pts_us,
            # where avail_start_us is the wall-clock (realtime µs) of
            # availabilityStartTime (content PTS 0) and pts_us is the frame's
            # container PTS on the content timeline. This matches the README's
            # live-edge math: at wall time T the live content PTS is (T - avail),
            # so present_delay = rt(vblank) - (avail + pts) is how late the
            # client presented the frame. A constant avail/start offset only
            # shifts the mean (which is NOT gated); growth and the p95 ceiling
            # are unaffected by it.
            if lat_ok:
                vblank = e.get("vblank_us") or ts
                rt_now = realtime_of(rt, vblank)
                pt = e.get("pts_us")
                if rt_now is not None and pt is not None:
                    b.present_delays.append((rt_now - avail - pt) / 1000.0)

        elif t == "arender":
            b.a_count += 1
            b.a_pad += e.get("pad_samples", 0) or 0
            lvl = e.get("fifo_level_bytes")
            if lvl is not None and fifo_cap:
                p = 100.0 * lvl / fifo_cap
                if b.fifo_min_pct is None or p < b.fifo_min_pct:
                    b.fifo_min_pct = p
            if need_timeline and e.get("render_us") is not None \
                    and e.get("content_pts_us") is not None:
                a_timeline.append((e["render_us"], e["content_pts_us"], rel))

        elif t == "buf":
            b.bump(b.buf, "%s.%s" % (e.get("which"), e.get("event")))
        elif t == "sync":
            b.bump(b.sync, e.get("kind"))
        elif t == "dec":
            b.bump(b.dec, e.get("kind"))
        elif t == "net":
            if e.get("kind") == "mpd":
                # manifest reloads are counted separately from segment fetches
                if e.get("status") != 200:
                    b.bump(b.sync, "mpd_reload_fail")
                continue
            st = e.get("status")
            if st == 200:
                b.net_ok += 1
                if e.get("total_us") is not None:
                    b.fetch_ms.append(e["total_us"] / 1000.0)
            elif st == 404:
                b.net_miss += 1
            else:
                b.net_fail += 1
            if e.get("rtt_us") is not None:
                b.rtt_ms.append(e["rtt_us"] / 1000.0)
        elif t == "eos":
            eos = rel

        # once enough direct pairs exist, stop growing the fallback timeline
        if need_timeline and vr_seen > 500:
            if direct_seen >= 0.5 * vr_seen:
                need_timeline = False
                a_timeline = []

    return buckets, bad, eos, a_timeline, direct_seen, vr_seen


def audio_pts_at(tl, t_us):
    if not tl or t_us <= tl[0][0] or t_us >= tl[-1][0]:
        return None
    lo, hi = 0, len(tl) - 1
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        if tl[mid][0] <= t_us:
            lo = mid
        else:
            hi = mid
    (t_a, p_a, _), (t_b, p_b, _) = tl[lo], tl[lo + 1]
    if t_b == t_a:
        return p_a
    return p_a + (p_b - p_a) * (t_us - t_a) / (t_b - t_a)


# --------------------------------------------------------------------------
# per-bucket / aggregate views
# --------------------------------------------------------------------------


def bucket_view(b, fps, rate, period):
    exp_frames = int(round(fps * b.span_s))
    exp_periods = int(round(b.span_s * rate / period)) if period else None
    nominal = 1000.0 / fps if fps else None
    ierr = [abs(v - nominal) for v in b.intervals] if nominal else []
    abs_off = [abs(o) for o in b.offsets]
    pd = b.present_delays
    # EBU R37 envelope violations as a rate, not an absolute max: over tens of
    # thousands of frames one outlier is noise, a real glitch is hundreds.
    outside = sum(1 for o in b.offsets
                  if o > G["av_offset_max_ms"] or o < G["av_offset_min_ms"])
    return {
        "index": b.index,
        "t_start_s": round(b.t_start_s, 1),
        "span_s": round(b.span_s, 1),
        "complete": b.complete,
        "frame_interval_ms": r2(nominal, 3),
        "av_offset_mean_ms": r2(mean(b.offsets)),
        "av_offset_p95_ms": r2(pct(abs_off, 95)),
        "av_offset_min_ms": r2(min(b.offsets)) if b.offsets else None,
        "av_offset_max_ms": r2(max(b.offsets)) if b.offsets else None,
        "av_envelope_violations": outside,
        "av_envelope_violation_pct": r2(100.0 * outside / len(b.offsets), 3) if b.offsets else None,
        "av_samples": len(b.offsets),
        "present_delay_mean_ms": r2(mean(pd)) if pd else None,
        "present_delay_p95_ms": r2(pct(pd, 95)) if pd else None,
        "present_delay_samples": len(pd),
        "frames_presented": b.v_count,
        "frames_expected": exp_frames,
        "frames_dropped": b.v_drop,
        "frames_repeated": b.v_repeat,
        "frame_interval_p95_err_ms": r2(pct(ierr, 95)) if ierr else None,
        "audio_periods_written": b.a_count,
        "audio_periods_expected": exp_periods,
        "audio_pad_samples": b.a_pad,
        "audio_fifo_level_min_pct": r2(b.fifo_min_pct),
        "audio_fifo_underflows": b.buf.get("audio_fifo.underflow", 0),
        "audio_fifo_overflows": b.buf.get("audio_fifo.overflow", 0),
        "jitter_buffer_underflows": b.buf.get("jitter_buffer.underflow", 0),
        "jitter_buffer_overflows": b.buf.get("jitter_buffer.overflow", 0),
        "drm_pool_exhausted": b.buf.get("drm_pool.exhausted", 0),
        "seg_queue_starved": b.buf.get("seg_queue.starved", 0),
        "seg_fetch_ok": b.net_ok,
        "seg_fetch_fail": b.net_fail,
        "seg_fetch_miss": b.net_miss,
        "seg_fetch_p95_ms": r2(pct(b.fetch_ms, 95)),
        "seg_fetch_max_ms": r2(max(b.fetch_ms)) if b.fetch_ms else None,
        "tcp_rtt_p95_ms": r2(pct(b.rtt_ms, 95)),
        "mpd_reload_fail": b.sync.get("mpd_reload_fail", 0),
        "resyncs": b.sync.get("resync", 0),
        "vsync_miss": b.sync.get("vsync_miss", 0),
        "vsync_dup": b.sync.get("vsync_dup", 0),
        "flip_error": b.sync.get("flip_error", 0),
        "flip_timeout": b.sync.get("flip_timeout", 0),
        "drm_event_starve": b.sync.get("drm_event_starve", 0),
        "async_cuda": b.sync.get("async_cuda", 0),
        "async_copy": b.sync.get("async_copy", 0),
        "async_stage": b.sync.get("async_stage", 0),
        "clock_fallback": b.sync.get("clock_fallback", 0),
        "decode_errors": b.dec.get("decode_error", 0),
        "decoder_unavailable": b.dec.get("decoder_unavailable", 0),
        "decoder_reinit": b.dec.get("decoder_reinit", 0),
        "corrupt_frames": b.dec.get("corrupt_frame", 0),
        "missing_ref": b.dec.get("missing_ref", 0),
        "decode_p95_ms": r2(pct(b.decode_ms, 95)),
        "verdict": "PASS",
        "failed_gates": [],
    }


ZERO_TOLERANCE = [
    ("buffers", "audio_fifo_underflows"),
    ("buffers", "audio_fifo_overflows"),
    ("buffers", "audio_pad_samples"),
    ("buffers", "jitter_buffer_underflows"),
    ("buffers", "jitter_buffer_overflows"),
    ("buffers", "drm_pool_exhausted"),
    ("buffers", "seg_queue_starved"),
    ("network", "seg_fetch_fail"),
    ("network", "seg_fetch_miss"),
    ("network", "mpd_reload_fail"),
    ("vsync_async", "vsync_miss"),
    ("vsync_async", "vsync_dup"),
    ("vsync_async", "flip_error"),
    ("vsync_async", "flip_timeout"),
    ("vsync_async", "drm_event_starve"),
    ("vsync_async", "async_cuda"),
    ("vsync_async", "async_copy"),
    ("vsync_async", "async_stage"),
    ("vsync_async", "clock_fallback"),
    ("cadence", "frames_dropped"),
    ("cadence", "frames_repeated"),
    ("decoder_errors", "decode_errors"),
    ("decoder_errors", "decoder_reinit"),
    ("decoder_errors", "corrupt_frames"),
    ("decoder_errors", "missing_ref"),
    ("decoder_unavailable", "decoder_unavailable"),
]


def gate_bucket(v):
    """Gates applied to one complete 60 s bucket."""
    failed = []

    def add(gate, value, limit, cls):
        failed.append({"gate": gate, "value": value, "limit": limit,
                       "class": cls, "bucket": v["index"]})

    for cls, key in ZERO_TOLERANCE:
        n = v.get(key)
        if n is None:
            add(key, None, 0, "environment")
        elif n > 0:
            add(key, n, 0, cls)

    if v["frames_expected"]:
        yld = v["frames_presented"] / float(v["frames_expected"])
        if yld < G["frame_yield_min"]:
            add("frame_yield", round(yld, 4), G["frame_yield_min"], "cadence")
    fe = v["frame_interval_p95_err_ms"]
    iv_lim = (G["frame_interval_frac"] * v["frame_interval_ms"]
              if v.get("frame_interval_ms") else None)
    if fe is None:
        add("frame_interval_p95_err_ms", None, iv_lim or 0, "environment")
    elif iv_lim and fe > iv_lim:
        add("frame_interval_p95_err_ms", fe, iv_lim, "cadence")
    if v["audio_periods_expected"]:
        dev = abs(v["audio_periods_written"] - v["audio_periods_expected"]) \
            / float(v["audio_periods_expected"])
        if dev > G["audio_period_tolerance"]:
            add("audio_period_deviation", round(dev, 4), G["audio_period_tolerance"], "cadence")

    lv = v["audio_fifo_level_min_pct"]
    if lv is not None and lv < G["audio_fifo_level_min_pct"]:
        add("audio_fifo_level_min_pct", lv, G["audio_fifo_level_min_pct"], "buffers")

    for key, lim in (("seg_fetch_p95_ms", G["seg_fetch_p95_ms"]),
                     ("seg_fetch_max_ms", G["seg_fetch_max_ms"]),
                     ("tcp_rtt_p95_ms", G["tcp_rtt_p95_ms"])):
        val = v.get(key)
        if val is not None and val > lim:
            add(key, val, lim, "network")

    dp = v["decode_p95_ms"]
    if dp is not None and dp > G["decode_p95_ms"]:
        add("decode_p95_ms", dp, G["decode_p95_ms"], "decoder_errors")

    if v["av_samples"] < 10:
        add("av_offset_samples", v["av_samples"], 10, "environment")
    else:
        if abs(v["av_offset_mean_ms"]) > G["av_offset_mean_abs_ms"]:
            add("av_offset_mean_ms", v["av_offset_mean_ms"],
                G["av_offset_mean_abs_ms"], "av_sync")
        if v["av_offset_p95_ms"] > G["av_offset_p95_abs_ms"]:
            add("av_offset_p95_ms", v["av_offset_p95_ms"],
                G["av_offset_p95_abs_ms"], "av_sync")
        vp = v["av_envelope_violation_pct"]
        if vp is not None and vp > G["av_envelope_violation_pct"]:
            add("av_envelope_violation_pct", vp, G["av_envelope_violation_pct"], "av_sync")

    v["failed_gates"] = failed
    v["verdict"] = "FAIL" if failed else "PASS"
    return failed


def aggregate(views, buckets, fps, rate, period):
    """Run-wide blocks. Sums over ALL buckets including the tail remainder."""
    def s(key):
        return sum(v.get(key) or 0 for v in views)

    all_off, all_t = [], []
    all_dec, all_fetch, all_rtt, all_ierr = [], [], [], []
    buckets_lag = []
    nominal = 1000.0 / fps if fps else None
    fifo_mins = [v["audio_fifo_level_min_pct"] for v in views
                 if v["audio_fifo_level_min_pct"] is not None]
    for b in buckets:
        all_off.extend(b.offsets)
        all_t.extend(b.offset_times)
        all_dec.extend(b.decode_ms)
        all_fetch.extend(b.fetch_ms)
        all_rtt.extend(b.rtt_ms)
        buckets_lag.extend(b.present_delays)
        if nominal:
            all_ierr.extend(abs(x - nominal) for x in b.intervals)

    abs_off = [abs(o) for o in all_off]
    sl = slope(all_t, all_off)
    s_env = sum(v.get("av_envelope_violations") or 0 for v in views)
    return {
        "av_sync": {
            "av_offset_mean_ms": r2(mean(all_off)),
            "av_offset_p95_ms": r2(pct(abs_off, 95)),
            "av_offset_min_ms": r2(min(all_off)) if all_off else None,
            "av_offset_max_ms": r2(max(all_off)) if all_off else None,
            "av_offset_drift_ms_per_min": r2(None if sl is None else sl * 60.0),
            "av_envelope_violations": s_env,
            "samples": len(all_off),
        },
        "render": {
            "frames_presented": s("frames_presented"),
            "frames_expected": s("frames_expected"),
            "frames_dropped": s("frames_dropped"),
            "frames_repeated": s("frames_repeated"),
            "frame_interval_p95_err_ms": r2(pct(all_ierr, 95)) if all_ierr else None,
            "audio_periods_written": s("audio_periods_written"),
            "audio_periods_expected": s("audio_periods_expected"),
        },
        "buffers": {
            "audio_fifo_underflows": s("audio_fifo_underflows"),
            "audio_fifo_overflows": s("audio_fifo_overflows"),
            "audio_pad_samples": s("audio_pad_samples"),
            "audio_fifo_level_min_pct": r2(min(fifo_mins)) if fifo_mins else None,
            "jitter_buffer_underflows": s("jitter_buffer_underflows"),
            "jitter_buffer_overflows": s("jitter_buffer_overflows"),
            "drm_pool_exhausted": s("drm_pool_exhausted"),
            "seg_queue_starved": s("seg_queue_starved"),
        },
        "network": {
            "seg_fetch_ok": s("seg_fetch_ok"),
            "seg_fetch_fail": s("seg_fetch_fail"),
            "seg_fetch_miss": s("seg_fetch_miss"),
            "seg_fetch_mean_ms": r2(mean(all_fetch)),
            "seg_fetch_p95_ms": r2(pct(all_fetch, 95)),
            "seg_fetch_max_ms": r2(max(all_fetch)) if all_fetch else None,
            "tcp_rtt_p95_ms": r2(pct(all_rtt, 95)),
            "mpd_reload_fail": s("mpd_reload_fail"),
            "resyncs": s("resyncs"),
        },
        "vsync_async": {k: s(k) for k in
                        ("vsync_miss", "vsync_dup", "flip_error", "flip_timeout",
                         "drm_event_starve", "async_cuda", "async_copy",
                         "async_stage", "clock_fallback")},
        "decoder": {
            "decode_errors": s("decode_errors"),
            "decoder_unavailable": s("decoder_unavailable"),
            "decoder_reinit": s("decoder_reinit"),
            "corrupt_frames": s("corrupt_frames"),
            "missing_ref": s("missing_ref"),
            "decode_mean_ms": r2(mean(all_dec)),
            "decode_p95_ms": r2(pct(all_dec, 95)),
        },
        "latency": {
            "present_delay_mean_ms": r2(mean(buckets_lag)),
            "present_delay_p95_ms": r2(pct(buckets_lag, 95)),
            "present_delay_samples": len(buckets_lag),
        },
    }


# --------------------------------------------------------------------------
# run-level (long-stretch) analysis
# --------------------------------------------------------------------------


def load_resources(run_dir):
    p = os.path.join(run_dir, "resources.ndjson")
    rows = []
    for e in iter_ndjson(p):
        if e.get("t") == "res":
            rows.append(e)
    rows.sort(key=lambda r: r.get("elapsed_s", 0))
    return rows


def longrun_block(views, agg, res, meta, eos_rel, span_s):
    complete = [v for v in views if v["complete"]]
    p95s = [v["av_offset_p95_ms"] for v in complete if v["av_offset_p95_ms"] is not None]
    means = [v["av_offset_mean_ms"] for v in complete if v["av_offset_mean_ms"] is not None]
    idx = list(range(len(p95s)))

    # Robust drift over per-minute means, and a robust start-vs-end excursion
    # (median of the first/last three minutes) so one spike cannot fake either.
    robust_drift = theil_sen(list(range(len(means))), means)
    excursion = None
    if len(means) >= 6:
        excursion = abs(median(means[-3:]) - median(means[:3]))
    elif len(means) >= 2:
        excursion = abs(means[-1] - means[0])

    # Present-delay (live-edge lag) series: robust growth over the stretch.
    lag_means = [v["present_delay_mean_ms"] for v in complete
                 if v["present_delay_mean_ms"] is not None]
    lag_growth = theil_sen(list(range(len(lag_means))), lag_means)

    rss0 = rss1 = fd0 = fd1 = disk0 = disk1 = None
    disk_tail_growth = None
    if res:
        rss0, rss1 = res[0].get("rss_kb"), res[-1].get("rss_kb")
        fd0, fd1 = res[0].get("fds"), res[-1].get("fds")
        disk0, disk1 = res[0].get("disk_mb"), res[-1].get("disk_mb")
        k = G["disk_flat_last_buckets"]
        tail = [r.get("disk_mb") for r in res[-k:] if r.get("disk_mb") is not None]
        if len(tail) >= 3:
            disk_tail_growth = tail[-1] - tail[0]

    clip = float(meta.get("clip_duration_s", 0) or 0)
    return {
        "buckets_total": len(complete),
        "buckets_passed": sum(1 for v in complete if v["verdict"] == "PASS"),
        "buckets_failed": sum(1 for v in complete if v["verdict"] == "FAIL"),
        "worst_bucket": (max(complete, key=lambda v: len(v["failed_gates"]))["index"]
                         if complete and any(v["failed_gates"] for v in complete) else None),
        "av_offset_drift_ms_per_min": r2(robust_drift),
        "av_offset_drift_ls_ms_per_min": agg["av_sync"]["av_offset_drift_ms_per_min"],
        "av_offset_total_excursion_ms": r2(excursion),
        "av_p95_degrade_ms_per_bucket": r2(theil_sen(idx, p95s)),
        "av_offset_bucket_means_ms": [r2(m) for m in means],
        "present_delay_bucket_means_ms": [r2(m) for m in lag_means],
        "present_delay_growth_ms_per_min": r2(lag_growth),
        "rss_start_mb": r2(rss0 / 1024.0) if rss0 else None,
        "rss_end_mb": r2(rss1 / 1024.0) if rss1 else None,
        "rss_growth_mb": r2((rss1 - rss0) / 1024.0) if (rss0 and rss1) else None,
        "fd_start": fd0,
        "fd_end": fd1,
        "fd_growth": (fd1 - fd0) if (fd0 is not None and fd1 is not None) else None,
        "disk_start_mb": r2(disk0),
        "disk_end_mb": r2(disk1),
        "disk_growth_mb": r2(disk1 - disk0) if (disk0 is not None and disk1 is not None) else None,
        "disk_growth_last_buckets_mb": r2(disk_tail_growth),
        "eos_reached": eos_rel is not None,
        "eos_at_s": r2(eos_rel) if eos_rel is not None else None,
        "playback_span_s": r2(span_s),
        "clip_duration_s": clip,
    }


def gate_run(lr, agg, tier):
    failed = []

    def add(gate, value, limit, cls):
        failed.append({"gate": gate, "value": value, "limit": limit,
                       "class": cls, "bucket": None})

    # Drift is only a meaningful question over a long stretch. The smoke tier is
    # a cheap pre-filter, not a drift measurement, so these gates are skipped
    # there rather than fired on noise.
    drift_gradable = (tier == "full" and lr["buckets_total"] >= 5)
    if drift_gradable:
        d = lr["av_offset_drift_ms_per_min"]
        if d is None:
            add("av_offset_drift_ms_per_min", None,
                G["av_drift_abs_ms_per_min"], "environment")
        elif abs(d) > G["av_drift_abs_ms_per_min"]:
            add("av_offset_drift_ms_per_min", d, G["av_drift_abs_ms_per_min"], "longrun")

        ex = lr["av_offset_total_excursion_ms"]
        if ex is not None and ex > G["av_total_excursion_ms"]:
            add("av_offset_total_excursion_ms", ex, G["av_total_excursion_ms"], "longrun")

        dg = lr["av_p95_degrade_ms_per_bucket"]
        if dg is not None and dg > G["av_p95_degrade_ms_per_bucket"]:
            add("av_p95_degrade_ms_per_bucket", dg,
                G["av_p95_degrade_ms_per_bucket"], "longrun")

    # Latency (live-edge lag). p95 ceiling applies whenever we could compute the
    # delay at all; growth needs a long stretch so it follows the drift rules.
    lat = agg["latency"]
    if lat.get("present_delay_samples"):
        p95 = lat["present_delay_p95_ms"]
        if p95 is not None and p95 > G["present_delay_p95_ms"]:
            add("present_delay_p95_ms", p95, G["present_delay_p95_ms"], "latency")
        if drift_gradable:
            g = lr["present_delay_growth_ms_per_min"]
            if g is None:
                add("present_delay_growth_ms_per_min", None,
                    G["present_delay_growth_ms_per_min"], "environment")
            elif g > G["present_delay_growth_ms_per_min"]:
                add("present_delay_growth_ms_per_min", g,
                    G["present_delay_growth_ms_per_min"], "latency")

    if agg["network"]["resyncs"] > G["resyncs_max"]:
        add("resyncs", agg["network"]["resyncs"], G["resyncs_max"], "network")

    # Leak gates need the resource samples; missing them is an environment fault,
    # not a silent pass.
    for key, lim in (("rss_growth_mb", G["rss_growth_mb"]),
                     ("fd_growth", G["fd_growth"]),
                     ("disk_growth_mb", G["disk_growth_mb"])):
        val = lr.get(key)
        if val is None:
            add(key, None, lim, "environment")
        elif val > lim:
            add(key, val, lim, "longrun")

    # A rolling window that is working keeps disk flat late in the run.
    dtl = lr.get("disk_growth_last_buckets_mb")
    if dtl is not None and dtl > G["disk_growth_mb"] / 4.0:
        add("disk_growth_last_buckets_mb", dtl, G["disk_growth_mb"] / 4.0, "longrun")

    # Completion: only the FULL tier is expected to reach end of clip.
    if tier == "full" and G["require_eos"]:
        if not lr["eos_reached"]:
            add("eos_reached", 0, 1, "longrun")
        else:
            expected = lr["clip_duration_s"]
            if expected:
                short = expected - (lr["playback_span_s"] or 0)
                if short > G["eos_tolerance_s"]:
                    add("stream_ended_early_s", r2(short), G["eos_tolerance_s"], "longrun")

    if lr["buckets_total"] == 0:
        add("buckets_total", 0, 1, "environment")

    return failed


def empty_blocks():
    """
    A zeroed skeleton so that EVERY report - including the degraded
    no-telemetry path - satisfies the schema. A contract that only holds on the
    happy path is not a contract, and the Main Agent must be able to read the
    same fields whatever went wrong.
    """
    return {
        "av_sync": {"av_offset_mean_ms": None, "av_offset_p95_ms": None,
                    "av_offset_min_ms": None, "av_offset_max_ms": None,
                    "av_offset_drift_ms_per_min": None,
                    "av_envelope_violations": None, "samples": 0},
        "render": {"frames_presented": 0, "frames_expected": 0, "frames_dropped": 0,
                   "frames_repeated": 0, "frame_interval_mean_ms": None,
                   "frame_interval_p95_err_ms": None, "audio_periods_written": 0,
                   "audio_periods_expected": None},
        "buffers": {"audio_fifo_underflows": 0, "audio_fifo_overflows": 0,
                    "audio_pad_samples": 0, "audio_fifo_level_min_pct": None,
                    "jitter_buffer_underflows": 0, "jitter_buffer_overflows": 0,
                    "drm_pool_exhausted": 0, "seg_queue_starved": 0},
        "network": {"seg_fetch_ok": 0, "seg_fetch_fail": 0, "seg_fetch_miss": 0,
                    "seg_fetch_mean_ms": None, "seg_fetch_p95_ms": None,
                    "seg_fetch_max_ms": None, "tcp_rtt_p95_ms": None,
                    "mpd_reload_fail": 0, "resyncs": 0},
        "vsync_async": {k: 0 for k in
                        ("vsync_miss", "vsync_dup", "flip_error", "flip_timeout",
                         "drm_event_starve", "async_cuda", "async_copy",
                         "async_stage", "clock_fallback")},
        "decoder": {"decode_errors": 0, "decoder_unavailable": 0,
                    "decoder_reinit": 0, "corrupt_frames": 0, "missing_ref": 0,
                    "decode_mean_ms": None, "decode_p95_ms": None},
        "latency": {"present_delay_mean_ms": None, "present_delay_p95_ms": None,
                    "present_delay_samples": 0},
        "longrun": {"buckets_total": 0, "buckets_passed": 0, "buckets_failed": 0,
                    "worst_bucket": None, "av_offset_drift_ms_per_min": None,
                    "av_offset_drift_ls_ms_per_min": None,
                    "av_offset_total_excursion_ms": None,
                    "av_p95_degrade_ms_per_bucket": None,
                    "av_offset_bucket_means_ms": [],
                    "present_delay_bucket_means_ms": [],
                    "present_delay_growth_ms_per_min": None,
                    "rss_growth_mb": None, "fd_growth": None,
                    "disk_growth_mb": None, "disk_growth_last_buckets_mb": None,
                    "eos_reached": False, "eos_at_s": None,
                    "playback_span_s": None, "clip_duration_s": None},
    }


def primary_fault(failed):
    classes = {f["class"] for f in failed}
    for c in FAULT_ORDER:
        if c in classes:
            return c
    return "none"


# --------------------------------------------------------------------------
# degraded fallback
# --------------------------------------------------------------------------


def fallback_from_text(run_dir):
    path = os.path.join(run_dir, "dash_client.log")
    counters = {"drop": 0, "resync": 0, "xrun": 0, "pad": 0, "fail": 0, "miss": 0}
    lines = 0
    if os.path.exists(path):
        with open(path, errors="replace") as fh:
            for line in fh:
                if "dash dbg:" not in line:
                    continue
                lines += 1
                for k in counters:
                    m = re.search(r"\b%s[=:](\d+)" % k, line)
                    if m:
                        counters[k] = max(counters[k], int(m.group(1)))
    return counters, lines


# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--out", default=None)
    ap.add_argument("--iteration", type=int, default=0)
    ap.add_argument("--commit", default=None)
    ap.add_argument("--tier", default=None, choices=["smoke", "full"])
    args = ap.parse_args()

    run_dir = args.run_dir
    meta = load_meta(run_dir)
    tier = args.tier or meta.get("tier", "full")
    fps = float(meta.get("content_fps", 60.0))
    rate = float(meta.get("audio_rate", 44100))
    period = float(meta.get("audio_period", 1024))
    fifo_cap = float(meta.get("audio_fifo_bytes", 8 * 1024 * 1024))
    bucket_s = float(meta.get("bucket_s", 60.0))
    cli_path = os.path.join(run_dir, "vmc_tlm_client.ndjson")

    notes = []
    report = {
        "iteration": args.iteration,
        "run_id": meta.get("run_id", os.path.basename(run_dir.rstrip("/"))),
        "commit": args.commit or meta.get("commit", "unknown"),
        "tier": tier,
        "verdict": "FAIL",
        "primary_fault": "none",
        "window_actual_s": meta.get("window_actual_s"),
        "content_fps": fps,
        "bucket_s": bucket_s,
        "buckets": [],
        "failed_gates": [],
        "artifacts": {
            "client_ndjson": cli_path,
            "server_ndjson": os.path.join(run_dir, "vmc_tlm_server.ndjson"),
            "resources": os.path.join(run_dir, "resources.ndjson"),
            "client_log": os.path.join(run_dir, "dash_client.log"),
            "server_log": os.path.join(run_dir, "dash_server.log"),
        },
        "notes": notes,
    }
    report.update(empty_blocks())

    t0, t1 = find_t0(cli_path)
    if t0 is None:
        counters, nlines = fallback_from_text(run_dir)
        notes.append(
            "DEGRADED: no client telemetry; parsed %d 'dash dbg:' lines %s. "
            "Per-frame A/V offset unavailable - av_sync cannot be judged."
            % (nlines, json.dumps(counters)))
        report["verdict"] = "ERROR"
        report["primary_fault"] = "environment"
        report["failed_gates"] = [{"gate": "telemetry_present", "value": 0,
                                   "limit": 1, "class": "environment", "bucket": None}]
        out(report, args.out)
        return 1

    span_s = (t1 - t0) / 1e6

    # Latency context: align the client monotonic timeline to realtime via the
    # 1 Hz clock_sync pairs, and the content timeline via the server's mpd_update
    # (avail_start_us = realtime µs of availabilityStartTime, absolute segment
    # numbering). present_delay is derived, not a new event.
    rt_pairs = sorted((e["ts"], e["realtime_us"]) for e in iter_ndjson(cli_path)
                      if e.get("t") == "clock_sync" and e.get("realtime_us") is not None)
    avail = snum = None
    for e in iter_ndjson(os.path.join(run_dir, "vmc_tlm_server.ndjson")):
        if e.get("t") == "srv" and e.get("kind") == "mpd_update":
            if e.get("avail_start_us") is not None:
                avail = e["avail_start_us"]
            if e.get("start_number") is not None:
                snum = e["start_number"]
    lat_ctx = {"rt": rt_pairs, "avail_start_us": avail, "start_number": snum,
               "seg_dur_s": float(meta.get("seg_duration_s", 1.0))}

    buckets, bad, eos_rel, a_tl, direct, vr = build_buckets(
        cli_path, t0, span_s, bucket_s, fifo_cap, lat_ctx, fps)

    # Fallback A/V pairing when audio_pos_us was not stamped in the flip handler.
    if vr and direct < 0.5 * vr and a_tl:
        notes.append("A/V offset from arender interpolation (audio_pos_us absent "
                     "in %d%% of vrender events)" % (100 - int(100.0 * direct / max(vr, 1))))
        tl = sorted((r[0], r[1], r[2]) for r in a_tl)
        for e in iter_ndjson(cli_path):
            if e.get("t") != "vrender" or e.get("pts_us") is None:
                continue
            tv = e.get("vblank_us") or e.get("ts")
            ap = audio_pts_at(tl, tv)
            if ap is None:
                continue
            rel = (e["ts"] - t0) / 1e6
            bi = min(int(rel // bucket_s), len(buckets) - 1)
            buckets[bi].offsets.append((e["pts_us"] - ap) / 1000.0)
            buckets[bi].offset_times.append(rel)

    views = [bucket_view(b, fps, rate, period) for b in buckets]
    failed = []
    for v in views:
        if v["complete"]:
            failed.extend(gate_bucket(v))
        else:
            # trailing remainder: only zero-tolerance counters, rates are noise
            for cls, key in ZERO_TOLERANCE:
                n = v.get(key) or 0
                if n > 0:
                    failed.append({"gate": key, "value": n, "limit": 0,
                                   "class": cls, "bucket": v["index"]})
                    v["failed_gates"].append({"gate": key, "value": n, "limit": 0,
                                              "class": cls, "bucket": v["index"]})
            v["verdict"] = "FAIL" if v["failed_gates"] else "PASS"

    agg = aggregate(views, buckets, fps, rate, period)
    report.update(agg)
    res = load_resources(run_dir)
    lr = longrun_block(views, agg, res, meta, eos_rel, span_s)
    report["longrun"] = lr
    failed.extend(gate_run(lr, agg, tier))

    # server context
    srv_restart = srv_404 = srv_eos = 0
    for e in iter_ndjson(os.path.join(run_dir, "vmc_tlm_server.ndjson")):
        k = e.get("kind")
        if k == "encoder_restart":
            srv_restart += 1
        elif k == "hold_404":
            srv_404 += 1
        elif k == "eos":
            srv_eos += 1
    if srv_restart:
        notes.append("server: %d encoder_restart event(s) - in a play-once run a "
                     "restart means the watchdog mistook EOS or a stall for a hang"
                     % srv_restart)
        failed.append({"gate": "server_encoder_restart", "value": srv_restart,
                       "limit": 0, "class": "longrun", "bucket": None})
    if srv_404:
        notes.append("server: %d hold-then-404 responses" % srv_404)
    if bad > 2:
        notes.append("%d unparsable NDJSON lines" % bad)

    # sanity - a broken harness must not be able to emit a PASS
    if agg["render"]["frames_presented"] == 0:
        notes.append("no vrender events: telemetry not flowing")
        failed.append({"gate": "frames_presented", "value": 0, "limit": 1,
                       "class": "environment", "bucket": None})
    if agg["render"]["audio_periods_written"] == 0:
        notes.append("no arender events: audio path never started")
        failed.append({"gate": "audio_periods_written", "value": 0, "limit": 1,
                       "class": "environment", "bucket": None})
    expected_buckets = int(float(meta.get("window_s", span_s)) // bucket_s)
    if lr["buckets_total"] < expected_buckets:
        notes.append("only %d of %d expected complete buckets - run ended early"
                     % (lr["buckets_total"], expected_buckets))
        failed.append({"gate": "buckets_total", "value": lr["buckets_total"],
                       "limit": expected_buckets, "class": "environment", "bucket": None})

    report["buckets"] = views
    report["failed_gates"] = failed
    report["primary_fault"] = primary_fault(failed)
    if report["decoder"]["decoder_unavailable"] > 0:
        report["verdict"] = "ERROR"
        report["primary_fault"] = "decoder_unavailable"
    elif report["primary_fault"] == "environment":
        report["verdict"] = "ERROR"
    elif failed:
        report["verdict"] = "FAIL"
    else:
        report["verdict"] = "PASS"

    out(report, args.out)
    return 0 if report["verdict"] == "PASS" else 1


def out(report, path):
    text = json.dumps(report, indent=2, sort_keys=False)
    if path:
        with open(path, "w") as fh:
            fh.write(text + "\n")
    print(text)


if __name__ == "__main__":
    sys.exit(main())
