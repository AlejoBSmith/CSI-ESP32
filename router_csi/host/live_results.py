"""GUI history from computed windows, independent of the 1 Hz status stream."""
from collections import deque
import math
import threading

# Display limits only: full device events remain in events.jsonl and .csi.
HISTORY_SECONDS = 60
HISTORY_MAX_POINTS = 12000
GUI_POLL_MS = 100


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def scale_history(scales):
    """Keep each scale's validity; a rejected scale must break only its curve."""
    result = []
    for scale in scales:
        if not isinstance(scale, dict):
            continue
        valid = bool(scale.get('valid', False)) and finite_number(scale.get('score'))
        result.append(dict(window=scale.get('window') if finite_number(scale.get('window')) else None,
                           period_ms=scale.get('period_ms') if finite_number(scale.get('period_ms')) else None,
                           score=scale.get('score') if valid else None, valid=valid,
                           coverage=scale.get('coverage') if finite_number(scale.get('coverage')) else None,
                           coherence=scale.get('coherence') if finite_number(scale.get('coherence')) else None))
    return result


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
            rate = output_hz
            expected = (config or {}).get('hop', 0) / rate if rate > 0 else 0
            if prev and (key[1] != prev['key'][1] or (expected and interval > 1.8 * expected)):
                gap(prev['t'] + 1e-6)
                interval = None
            score = item.get('score')
            if not item.get('measurement_valid',item.get('state') in ('ACTIVE', 'CLEAR')) or not isinstance(score, (int, float)) or not math.isfinite(score):
                score = None
            point=dict(t=t, node=node, score=score, label=label,rx_us=us,seq1=item.get('seq1'),provisional=not item.get('amplitude_calibrated',True))
            for component_key,flag in [('score_fast','fast_valid'),('score_slow','slow_valid'),('score_legacy','legacy_valid')]:
                value=item.get(component_key)
                point[component_key]=value if item.get(flag,False) and isinstance(value,(int,float)) and math.isfinite(value) else None
            has_bank = bool(item.get('bank_total', 0) > 0 or item.get('scales'))
            if has_bank and isinstance(item.get('scales'), list):
                point['scales'] = scale_history(item['scales'])
            if has_bank and 'bank_raw' in item:
                measurement_valid = item.get('measurement_valid', item.get('state') in ('ACTIVE', 'CLEAR'))
                point['bank_raw'] = item['bank_raw'] if measurement_valid and finite_number(item['bank_raw']) and item.get('bank_valid', 0) > 0 else None
                for bank_key in ('bank_valid', 'bank_total', 'bank_support', 'bank_dominant_ms', 'confirmation_progress'):
                    point[bank_key] = item.get(bank_key) if finite_number(item.get(bank_key)) else None
                point['bank_confirmed'] = bool(item.get('bank_confirmed', False))
            self.points.append(point)
            self.previous[node] = dict(t=t, us=us, key=key)
            return interval

    def snapshot(self, elapsed):
        with self.lock:
            return sorted((p for p in self.points if p['t'] >= elapsed - HISTORY_SECONDS),
                          key=lambda p: p['t'])
