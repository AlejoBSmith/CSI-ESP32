import sys
from pathlib import Path
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
import start
from decode import encode


class LauncherTests(unittest.TestCase):
    def test_identical_rx_firmware_is_labeled_by_identity_not_com_order(self):
        devices={'COM8':('RX','bb'), 'COM9':('TX','cc'), 'COM7':('RX','aa'), 'COM10':('RX','dd')}
        class Serial:
            def __init__(self,**kwargs):self.port=None;self.dtr=True;self.rts=True
            def __enter__(self):
                assert self.port in devices and not self.dtr and not self.rts
                return self
            def __exit__(self,*args):pass
            def reset_input_buffer(self):pass
            def write(self,data):pass
            @property
            def in_waiting(self):return 1000
            def read(self,size):
                role,identity=devices[self.port]
                return encode(2,dict(role=role,device_id=identity,node=1))
        ports=[SimpleNamespace(device=n,vid=0x303a,serial_number=n) for n in devices]
        with patch.object(start.serial,'Serial',Serial),patch.object(start.list_ports,'comports',return_value=ports),patch.object(start.subprocess,'call',return_value=0) as call,patch('sys.argv',['start.py']):
            with self.assertRaises(SystemExit) as result:start.main()
            self.assertEqual(result.exception.code,0)
            args=call.call_args.args[0]
            self.assertEqual(args[args.index('--rx1')+1],'COM7')
            self.assertEqual(args[args.index('--rx2')+1],'COM8')
            self.assertEqual(args[args.index('--rx3')+1],'COM10')
            self.assertEqual(args[args.index('--tx')+1],'COM9')

if __name__=='__main__':unittest.main()
