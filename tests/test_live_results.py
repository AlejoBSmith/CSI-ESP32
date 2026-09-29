import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
import unittest
from live_results import ResultHistory, HISTORY_MAX_POINTS


def result(us, score=1, boot=1, reference=1):
    return dict(rx_us=us, boot=boot, reference_id=reference, seq0=us-100,
                seq1=us, score=score, state='ACTIVE')


class HistoryTests(unittest.TestCase):
    def test_provisional_results_are_visible_and_marked(self):
        h=ResultHistory();p=result(1000000,3)
        p.update(state='PROVISIONAL',measurement_valid=True,amplitude_calibrated=False)
        h.update('rx3',3,p,1,'')
        points=[v for v in h.snapshot(1) if v['score'] is not None]
        self.assertEqual(len(points),1)
        self.assertTrue(points[0]['provisional'])
        self.assertEqual(points[0]['score'],3)

    def test_all_fast_results_survive_slow_status_and_usb_batching(self):
        h = ResultHistory()
        for i in range(11):
            # Ten results between consecutive 1 Hz status messages, all
            # delivered in a burst; timestamps must retain 100 ms spacing.
            if i in (0, 10):
                h.update('rx1', 2, dict(state='ACTIVE', score=99), 2, '')
            h.update('rx1', 3, result(i*100000, i), 2, '', dict(hop=25))
        points = [p for p in h.snapshot(3) if p['score'] is not None]
        self.assertEqual([p['score'] for p in points], list(range(11)))
        self.assertAlmostEqual(points[-1]['t']-points[0]['t'], 1)

    def test_old_usb_records_do_not_push_live_results_into_the_future(self):
        h=ResultHistory()
        h.update('rx1',3,result(1000000),1,'')
        h.update('rx1',3,result(101000000,7),1.1,'')
        last=h.snapshot(1.1)[-1]
        self.assertAlmostEqual(last['t'],1.1)
        self.assertEqual(last['score'],7)
        self.assertIsNone(h.snapshot(1.1)[-2]['score'])

    def test_hop_changes_spacing_and_missing_windows_break_line(self):
        h = ResultHistory()
        h.update('rx1', 3, result(0), 0, '', dict(hop=200))
        interval = h.update('rx1', 3, result(800000), .8, '', dict(hop=200))
        self.assertAlmostEqual(interval, .8)
        interval = h.update('rx1', 3, result(900000), .9, '', dict(hop=25))
        self.assertAlmostEqual(interval, .1)
        h.update('rx1', 3, result(1200000), 1.2, '', dict(hop=25))
        self.assertIsNone(h.snapshot(2)[-2]['score'])

    def test_invalid_disconnect_and_boot_breaks_do_not_join_scores(self):
        for kind, item in [(2, dict(state='INVALID')), (0, dict(connection='disconnected'))]:
            h = ResultHistory()
            h.update('rx1', 3, result(1000000), 1, '')
            h.update('rx1', kind, item, 2, '')
            h.update('rx1', 3, result(3000000), 3, '')
            self.assertIsNone(h.snapshot(3)[-2]['score'])
        h.update('rx1', 3, result(100, boot=2), 4, '')
        self.assertIsNone(h.snapshot(4)[-2]['score'])

    def test_duplicate_features_and_tx_do_not_add_scores(self):
        h = ResultHistory()
        for node in ('rx1', 'rx1', 'tx'):
            h.update(node, 3, result(100000), 1, '')
        self.assertEqual(len([p for p in h.snapshot(1) if p['score'] is not None]), 1)

    def test_receivers_keep_independent_clocks_and_history_is_bounded(self):
        h = ResultHistory()
        h.update('rx1', 3, result(100000), 1, '')
        h.update('rx2', 3, result(9000000), 1, '')
        points = [p for p in h.snapshot(1) if p['score'] is not None]
        self.assertEqual([p['t'] for p in points], [1, 1])
        self.assertEqual(h.snapshot(62), [])
        for i in range(HISTORY_MAX_POINTS+10):
            h.update('rx1', 3, result(200000+i), 2, '')
        self.assertLessEqual(len(h.snapshot(2)), HISTORY_MAX_POINTS)


if __name__ == '__main__':
    unittest.main()
