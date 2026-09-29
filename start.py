"""Discover the connected XIAOs and start the optional local web interface."""
from pathlib import Path
import json
import subprocess
import sys
import time
import serial
from serial.tools import list_ports
sys.path.insert(0,str(Path(__file__).resolve().parent/'host'))
from decode import Decoder


def main():
    root=Path(__file__).resolve().parent
    nodes=[]
    for port in list_ports.comports():
        if port.vid not in (0x303a,0x2886):continue
        try:
            link=serial.Serial(port=None,baudrate=3000000,timeout=.1,write_timeout=1)
            link.dtr=False;link.rts=False;link.port=port.device
            with link:
                link.reset_input_buffer();link.write(b'{"cmd":"status"}\n')
                decoder=Decoder();until=time.monotonic()+3;status=None
                while time.monotonic()<until and status is None:
                    for kind,item,_ in decoder.feed(link.read(max(1,link.in_waiting))):
                        if kind==2 and item.get('role') in ('TX','RX'):status=item;break
                if status:nodes.append((status['role'],status.get('device_id') or port.serial_number or port.device,port.device))
        except (OSError,ValueError) as error:print(f'{port.device}: {error}')
    receivers=sorted(n for n in nodes if n[0]=='RX');transmitters=[n for n in nodes if n[0]=='TX']
    if not receivers:raise SystemExit('No RX found. Connect a receiver, upload the rx firmware and close other serial monitors.')
    if len(receivers)>3 or len(transmitters)>1:raise SystemExit('This interface supports up to 3 RX and 1 TX; use explicit ports in host/capture.py.')
    args=[sys.executable,str(root/'host/capture.py'),'--open',*sys.argv[1:]]
    for i,(_,identity,port) in enumerate(receivers,1):
        print(f'RX{i}: {port} ({identity})');args+=['--rx'+str(i),port]
    if transmitters:args+=['--tx',transmitters[0][2]]
    raise SystemExit(subprocess.call(args,cwd=root))

if __name__=='__main__':main()
