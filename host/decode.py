"""Versioned little-endian USB records, CRC32 and byte-stream resynchronization."""
import json
import struct
import zlib

MAGIC = b'RAT1'
HEADER = struct.Struct('<4sBBH')
RAW = struct.Struct('<BBHIIIQIIIbbBBBBBB Bbf6s')
RAW_NAMES = ('node flags csi_len tx_seq tx_boot rx_boot rx_us radio_us queue_drops telemetry_drops '
             'rssi noise_floor channel bandwidth sig_mode mcs stbc rate agc fft gain source').split()


class Decoder:
    def __init__(self):
        self.buffer = bytearray()
        self.bad_crc = self.discarded = 0

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while len(self.buffer) >= HEADER.size:
            offset = self.buffer.find(MAGIC)
            if offset < 0:
                self.discarded += len(self.buffer)-3
                del self.buffer[:-3]
                break
            if offset:
                self.discarded += offset
                del self.buffer[:offset]
            if len(self.buffer) < HEADER.size: break
            _, version, kind, size = HEADER.unpack_from(self.buffer)
            if version != 1 or kind not in range(1,8) or size > 60000:
                del self.buffer[0]; self.discarded += 1; continue
            length = HEADER.size+size+4
            if len(self.buffer) < length: break
            wire = bytes(self.buffer[:length])
            expected, = struct.unpack_from('<I', wire, length-4)
            if zlib.crc32(wire[:-4]) != expected:
                self.bad_crc += 1; del self.buffer[0]; continue
            del self.buffer[:length]
            payload = wire[HEADER.size:-4]
            try:
                if kind == 1:
                    if len(payload) < RAW.size: raise ValueError('short raw record')
                    item = dict(zip(RAW_NAMES, RAW.unpack_from(payload)))
                    if item['csi_len'] > 612 or item['csi_len']%2 or len(payload) != RAW.size+item['csi_len']:
                        raise ValueError('CSI length mismatch')
                    item['source'] = item['source'].hex(':')
                    item['iq'] = list(struct.unpack(f"{item['csi_len']}b", payload[RAW.size:]))
                else:
                    item = json.loads(payload)
                    if not isinstance(item, dict): raise ValueError('expected object')
            except (ValueError, UnicodeError, struct.error):
                self.discarded += length; continue
            result.append((kind, item, wire))
        return result


def records(path):
    decoder = Decoder()
    with open(path, 'rb') as file:
        while chunk := file.read(65536):
            yield from decoder.feed(chunk)


def encode(kind, payload):
    if not isinstance(payload, bytes): payload=json.dumps(payload, separators=(',', ':')).encode()
    packet=HEADER.pack(MAGIC,1,kind,len(payload))+payload
    return packet+struct.pack('<I',zlib.crc32(packet))


if __name__ == '__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('file');p.add_argument('--raw', action='store_true');a=p.parse_args()
    for kind,item,_ in records(a.file):
        if kind!=1 or a.raw: print(json.dumps(dict(type=kind, **item)))
