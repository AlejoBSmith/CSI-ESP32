"""Reopen USB serial after removal, read errors or a silent device.

Keep the USB serial number so a changed COM number cannot swap TX and RX.
Callbacks run on the reader thread. No calibration is started on reconnect.
"""
import time
import serial
from serial.tools import list_ports


def identify(port, enumerate_ports=list_ports.comports):
    return next((p.serial_number for p in enumerate_ports() if p.device == port), None)


def resolve_port(port, identity, enumerate_ports=list_ports.comports):
    if not identity:
        return port
    return next((p.device for p in enumerate_ports() if p.serial_number == identity), None)


def reconnecting_reader(port, baud, stop, retry, opened, consume, disconnected,
                        factory=serial.Serial, enumerate_ports=list_ports.comports,
                        retry_seconds=1, silence_seconds=5):
    identity=identify(port, enumerate_ports)
    first=True
    while not stop.is_set():
        link=None
        try:
            target=resolve_port(port,identity,enumerate_ports)
            if not target:
                raise OSError('USB device absent; waiting for reconnection')
            if not identity:
                identity=identify(target,enumerate_ports)
            link=factory(port=None,baudrate=baud,timeout=.1,write_timeout=2)
            link.dtr=False;link.rts=False;link.port=target;link.open()
            retry.clear()
            opened(link,first,identity)
            first=False
            last_status=time.monotonic()
            while not stop.is_set() and not retry.is_set():
                data=link.read(max(1,min(link.in_waiting,65536)))
                if data and consume(data):
                    last_status=time.monotonic()
                if time.monotonic()-last_status>silence_seconds:
                    raise OSError('No valid device status for 5 s; reconnecting USB')
            if retry.is_set():
                raise OSError('USB reconnection requested')
        except (OSError,ValueError) as error:
            disconnected(str(error))
        finally:
            if link:
                try:link.close()
                except (OSError,ValueError):pass
        if not stop.is_set():stop.wait(retry_seconds)
