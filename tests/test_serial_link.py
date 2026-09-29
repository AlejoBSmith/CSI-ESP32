import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
import threading
import unittest
from types import SimpleNamespace
from serial_link import reconnecting_reader, resolve_port


class RecoveryTests(unittest.TestCase):
    def test_identity_does_not_select_other_board(self):
        ports=lambda:[SimpleNamespace(device='COM47',serial_number='TX')]
        self.assertIsNone(resolve_port('COM46','RX',ports))

    def run_recovery(self, mode):
        stop=threading.Event();retry=threading.Event();made=[];opened=[];errors=[]
        class Link:
            in_waiting=1
            def __init__(self,**kwargs):
                self.index=len(made);self.closed=False;made.append(self)
                assert len(made)<=2, 'Unexpected retry loop'
            def open(self):pass
            def write(self,data):return len(data)
            def close(self):self.closed=True
            def read(self,n):
                if self.index==0:
                    if mode=='read_error':raise OSError('ClearCommError failed')
                    if mode=='manual':retry.set()
                    return b''
                return b'status'
        def ports():
            return [SimpleNamespace(device='COM46' if not made else 'COM48',serial_number='RX')]
        def on_open(link,first,identity):opened.append((link.port,first,identity))
        def consume(data):self.assertEqual(data,b'status');stop.set();return True
        reconnecting_reader('COM46',3000000,stop,retry,on_open,consume,errors.append,
                            factory=Link,enumerate_ports=ports,retry_seconds=.001,silence_seconds=.005)
        self.assertEqual(opened,[('COM46',True,'RX'),('COM48',False,'RX')])
        self.assertEqual(len(errors),1)
        self.assertTrue(all(link.closed for link in made))

    def test_read_error_then_new_com(self):self.run_recovery('read_error')
    def test_silent_device_reopened(self):self.run_recovery('silent')
    def test_manual_reconnect(self):self.run_recovery('manual')


if __name__=='__main__':unittest.main()
