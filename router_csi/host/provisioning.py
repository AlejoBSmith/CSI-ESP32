"""Validate device-local Wi-Fi setup; redact secrets before persistent logging."""
import ipaddress
import math

def redact(value):
    if isinstance(value,dict):return {k:('[REDACTED]' if k.lower() in ('password','passphrase','psk') else redact(v)) for k,v in value.items()}
    if isinstance(value,list):return [redact(v) for v in value]
    return value

def wifi_command(action):
    ssid=action.get('ssid','CSI_LAB');password=action.get('password','');target=action.get('target','192.168.50.1');hz=action.get('ping_hz',100)
    if not isinstance(ssid,str) or not 1<=len(ssid.encode('utf-8'))<=32:raise ValueError('SSID: 1..32 bytes.')
    if not isinstance(password,str) or not 8<=len(password.encode('utf-8'))<=63:raise ValueError('Password WPA2: 8..63 bytes.')
    try:ip=ipaddress.IPv4Address(target)
    except (ValueError,TypeError):raise ValueError('Direccion IPv4 del AP invalida.') from None
    if ip.is_unspecified or ip.is_multicast or int(ip)==0xffffffff:raise ValueError('Usa la IPv4 local del AP.')
    if isinstance(hz,bool) or not isinstance(hz,(int,float)) or not math.isfinite(hz) or hz!=int(hz) or not 10<=hz<=500:raise ValueError('ping_hz debe ser entero entre 10 y 500.')
    return dict(cmd='wifi',ssid=ssid,password=password,target=str(ip),ping_hz=int(hz))
