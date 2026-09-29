"""GUI history from computed windows, independent of the 1 Hz status stream."""
from collections import deque
import math
import threading

# Display limits only: full device events remain in events.jsonl and .csi.
HISTORY_SECONDS = 60
HISTORY_MAX_POINTS = 12000
GUI_POLL_MS = 100


class ResultHistory:
    def __init__(self):
        self.points = deque(maxlen=HISTORY_MAX_POINTS)
        self.clocks = {}
        self.previous = {}
        self.lock = threading.Lock()

    def update(self, node, kind, item, elapsed, label, config=None, output_hz=250):
        """Return observed window interval; statuses add gaps, never scores.

        Each USB/boot segment anchors the receiver clock to PC receipt time.
        This preserves within-node timing, but is not clock synchronization.
        """
        if node == 'tx' or kind not in (0, 2, 3):
            return None
        with self.lock:
            def gap(t):
                self.points.append(dict(t=t, node=node, score=None, label=label))

            if kind == 0:
                gap(elapsed)
                self.clocks.pop(node, None)
                self.previous.pop(node, None)
                return None
            if kind == 2:
                if not item.get('measurement_valid',item.get('state') in ('ACTIVE', 'CLEAR')):
                    # Place the break immediately after the last result, even
                    # when USB buffering delays the status message.
                    prev = self.previous.pop(node, None)
                    gap(prev['t'] + 1e-6 if prev else elapsed)
                return None

            us = item['rx_us']
            boot = item['boot']
            clock = self.clocks.get(node)
            prev = self.previous.get(node)
            # USB can retain pre-disconnection records. If that old first
            # record anchors the clock, fresh records appear far in the future.
            # Re-anchor on catch-up, allowing ordinary batches up to one second.
            ahead = clock is not None and us / 1e6 + clock[1] > elapsed + 1.0
            reanchor = clock is None or clock[0] != boot or (prev and us < prev['us']) or ahead
            if reanchor:
                gap(elapsed)
                clock = self.clocks[node] = (boot, elapsed - us / 1e6)
                prev = None
            t = elapsed if reanchor else us / 1e6 + clock[1]
            key = (boot, item.get('reference_id'), item.get('seq0'), item.get('seq1'), us)
            if prev and key == prev['key']:
                return None
            interval = (us - prev['us']) / 1e6 if prev else None
            rate = min(output_hz, (config or {}).get('min_fs', output_hz))
            expected = (config or {}).get('hop', 0) / rate if rate > 0 else 0
            if prev and (key[1] != prev['key'][1] or (expected and interval > 1.8 * expected)):
                gap(prev['t'] + 1e-6)
                interval = None
            score = item.get('score')
            if not item.get('measurement_valid',item.get('state') in ('ACTIVE', 'CLEAR')) or not isinstance(score, (int, float)) or not math.isfinite(score):
                score = None
            self.points.append(dict(t=t, node=node, score=score, label=label,
                                    rx_us=us, seq1=item.get('seq1'),provisional=not item.get('amplitude_calibrated',True)))
            self.previous[node] = dict(t=t, us=us, key=key)
            return interval

    def snapshot(self, elapsed):
        with self.lock:
            return sorted((p for p in self.points if p['t'] >= elapsed - HISTORY_SECONDS),
                          key=lambda p: p['t'])
