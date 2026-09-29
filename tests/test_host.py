import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'host'))
import unittest
import struct
from decode import Decoder,encode,RAW
from capture import fusion


class ProtocolTests(unittest.TestCase):
    def test_chunked_stream_resynchronizes_after_damage(self):
        good=encode(2,{'node':1,'state':'INVALID'})
        bad=bytearray(good);bad[-1]^=1
        decoder=Decoder();found=[]
        for byte in b'boot log\n'+bad+good:
            found.extend(decoder.feed(bytes([byte])))
        self.assertEqual(len(found),1)
        self.assertEqual(found[0][1]['state'],'INVALID')
        self.assertEqual(decoder.bad_crc,1)

    def test_raw_iq_length_and_order(self):
        meta=RAW.pack(1,3,4,123,456,789,1000000,1000,0,0,-40,-90,6,0,1,0,0,11,20,-3,1.5,b'\x1a\x00\x00\x00\x00\x00')
        found=Decoder().feed(encode(1,meta+struct.pack('4b',3,4,-6,8)))
        self.assertEqual(found[0][1]['iq'],[3,4,-6,8])
        self.assertEqual(found[0][1]['tx_seq'],123)
        self.assertEqual(found[0][1]['gain'],1.5)
        self.assertEqual(Decoder().feed(encode(1,meta+b'\x00')),[])

    def test_provisional_fusion_is_not_invalid_or_clear(self):
        states={'rx1':dict(state='PROVISIONAL',measurement_valid=True,amplitude_calibrated=False,_received=10)}
        self.assertEqual(fusion(states,['rx1'],11),'PROVISIONAL')
        self.assertEqual(fusion(states,['rx1'],20),'INVALID')

    def test_or_fusion_and_invalid_are_distinct(self):
        states={'rx1':{'state':'ACTIVE','_received':10},'rx2':{'state':'INVALID','_received':10}}
        self.assertEqual(fusion(states,['rx1','rx2'],11),'ACTIVE')
        states['rx1']['state']='CLEAR'
        self.assertEqual(fusion(states,['rx1','rx2'],11),'INVALID')
        states['rx2']['state']='CLEAR'
        self.assertEqual(fusion(states,['rx1','rx2'],11),'CLEAR')
        self.assertEqual(fusion(states,['rx1','rx2'],20),'INVALID')


if __name__=='__main__':unittest.main()
