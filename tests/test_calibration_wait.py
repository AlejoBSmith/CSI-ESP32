import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
"""Exercise the actual HTTP/main-loop path with simulated serial receivers."""
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.request import Request, urlopen

import capture
from decode import encode


class CalibrationWaitTests(unittest.TestCase):
    def test_delay_cancel_and_shutdown(self):
        self.assertEqual(capture.CALIBRATION_DELAY_SECONDS, 20)
        (Path(__file__).resolve().parents[1]/'.pio').mkdir(exist_ok=True)
        writes = []
        failures = []
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        base = f'http://127.0.0.1:{port}'

        def reader(port, baud, stop, retry, opened, consume, disconnected):
            class Link:
                def write(self, data):
                    writes.append((port, json.loads(data), time.monotonic()))
            link = Link();link.port = port
            opened(link, True, port)
            while not stop.wait(.02):
                consume(encode(2, dict(state='INVALID', reason='NEEDS_CALIBRATION', config={})))

        def get():
            return json.load(urlopen(base+'/status', timeout=2))

        def act(action):
            with urlopen(Request(base+'/action', data=json.dumps(dict(action=action)).encode(),
                                 headers={'Content-Type': 'application/json'}), timeout=2) as r:
                self.assertEqual(r.status, 202)

        def calibrations():
            return [w for w in writes if w[1].get('cmd') == 'calibrate_after_delay']

        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1]/'.pio') as tmp, \
             patch.object(capture, 'CALIBRATION_DELAY_SECONDS', .4), \
             patch.object(capture, 'reconnecting_reader', reader), \
             patch('sys.argv', ['capture.py', '--rx1', 'RX1', '--rx2', 'RX2',
                                '--web-port', str(port), '--output', str(Path(tmp)/'session')]):
            def run():
                try:capture.main()
                except Exception as e:failures.append(e)
            worker = threading.Thread(target=run, daemon=True);worker.start()
            try:
                for _ in range(100):
                    try:
                        if len(get()['states']) == 2:break
                    except OSError:pass
                    time.sleep(.02)
                began = time.monotonic();act('calibrate');time.sleep(.12)
                self.assertEqual(len(calibrations()), 2)
                self.assertEqual(get()['label'], 'CALIBRATION_COUNTDOWN')
                time.sleep(.4)
                self.assertEqual(len(calibrations()), 2)
                self.assertFalse(any(w[1].get('cmd')=='calibrate' for w in writes))
                self.assertEqual(get()['label'], 'LOCAL_BACKGROUND_CALIBRATION')
                act('calibrate');time.sleep(.06);act('cancel_calibration_wait');time.sleep(.5)
                self.assertEqual(len(calibrations()), 4)
                self.assertEqual(len([w for w in writes if w[1].get('cmd')=='cancel_calibration']),2)
                self.assertEqual(get()['label'], 'CALIBRATION_CANCELLED')
                act('calibrate');time.sleep(.06);act('stop_session')
                worker.join(3)
                self.assertFalse(worker.is_alive())
                self.assertEqual(len(calibrations()), 6)
                self.assertEqual(len([w for w in writes if w[1].get('cmd')=='cancel_calibration']),4)
                self.assertEqual(failures, [])
            finally:
                if worker.is_alive():
                    act('stop_session');worker.join(3)


if __name__ == '__main__':unittest.main()
