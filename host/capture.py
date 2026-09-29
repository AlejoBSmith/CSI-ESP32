"""Record up to three CSI receivers via USB; local UI, ground truth and OR fusion."""
import argparse
import csv
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import queue
import threading
import time
import webbrowser
import serial
from decode import Decoder
from serial_link import reconnecting_reader
from live_results import ResultHistory, HISTORY_SECONDS, GUI_POLL_MS

# Time to leave the sensing area before any calibration command is sent.
CALIBRATION_DELAY_SECONDS = 20

def fusion(states, names, now):
    observed = [states.get(n, {}) for n in names]
    valid = [s.get('state') if now-s.get('_received',-1e9)<2.5 else 'INVALID' for s in observed]
    if 'ACTIVE' in valid: return 'ACTIVE'
    if any(now-s.get('_received',-1e9)<2.5 and s.get('measurement_valid',False) and not s.get('amplitude_calibrated',False) for s in observed):return 'PROVISIONAL'
    if valid and all(s=='CLEAR' for s in valid): return 'CLEAR'
    return 'INVALID'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rx1');p.add_argument('--rx2');p.add_argument('--rx3');p.add_argument('--tx')
    p.add_argument('--baud',type=int,default=3000000)
    p.add_argument('--raw',action='store_true',help='Guardar CSI crudo ademas de los resultados')
    p.add_argument('--duration',type=float,default=0)
    p.add_argument('--output',type=Path)
    p.add_argument('--metadata',type=Path,help='JSON con geometría, antenas, orientación y fondo observado')
    p.add_argument('--web-port',type=int,default=8766);p.add_argument('--open',action='store_true')
    p.add_argument('--command',help='JSON enviado al iniciar (para verificaciones técnicas)')
    a=p.parse_args()
    ports={n:getattr(a,n) for n in ('rx1','rx2','rx3','tx') if getattr(a,n)}
    if not ports or len(set(ports.values()))!=len(ports):p.error('Indica puertos distintos para los nodos conectados.')
    if not math.isfinite(a.duration) or a.duration<0:p.error('Duración finita y no negativa.')
    folder=a.output or Path('captures')/datetime.now().strftime('session_%Y-%m-%d_%H%M%S')
    folder.mkdir(parents=True,exist_ok=False)
    metadata={'created':datetime.now().astimezone().isoformat(),'transport':'USB serial (see per-node transport)','baud':a.baud,'ports':ports,
              'antenna_model':None,'antenna_orientation':None,'positions_m':None,'floor_material':None,
              'background':'sin movimiento local observado solo cuando lo confirme el operador; otros pisos no controlados',
              'firmware':{},'physical_validation':'pending'}
    if a.metadata: metadata['operator_setup']=json.loads(a.metadata.read_text(encoding='utf-8'))
    (folder/'metadata.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    (folder/'config.json').write_text('{}',encoding='utf-8')
    incoming=queue.Queue(10000);stop=threading.Event();states={};history=ResultHistory();links={};issues={};counters={}
    retries={n:threading.Event() for n in ports}
    event_file=(folder/'events.jsonl').open('x',encoding='utf-8')
    score_file=(folder/'scores.csv').open('x',newline='',encoding='utf-8')
    scores=csv.writer(score_file);scores.writerow(['elapsed_s','receiver','rx_us','state','score','reference_id','amplitude_calibrated','measurement_valid','score_units']);score_file.flush()
    ground_file=(folder/'ground_truth.csv').open('x',newline='',encoding='utf-8')
    ground=csv.writer(ground_file);ground.writerow(['timestamp_pc','elapsed_s','label','notes']);ground_file.flush()
    started=time.monotonic();label='UNLABELED';schedule=[];deadline=0.;last_fused='INVALID';config={}
    def log(event,**values):
        event_file.write(json.dumps(dict(timestamp_pc=datetime.now().astimezone().isoformat(),elapsed_s=time.monotonic()-started,event=event,**values),allow_nan=False)+'\n');event_file.flush()
    def send(node,command):
        if node not in links:raise ValueError('Nodo no conectado')
        links[node].write((json.dumps(command)+'\n').encode())
        log('command',node=node,command=command)
    def receiver(node,port):
        decoder=Decoder();raw_count=0;first_rx_us=last_rx_us=None;segment_count=0;rx_boot=None
        crc_total=discard_total=0;last_flush=time.monotonic();last_request=0.;generation=0
        def event(kind,item):
            try:incoming.put_nowait((node,kind,item))
            except queue.Full:issues[node]='HOST_EVENT_QUEUE_FULL'
        def opened(link,first,identity):
            nonlocal decoder,crc_total,discard_total,first_rx_us,last_rx_us,segment_count,generation
            crc_total+=decoder.bad_crc;discard_total+=decoder.discarded
            decoder=Decoder();first_rx_us=last_rx_us=None;segment_count=0;generation+=1
            links[node]=link
            event(0,{'connection':'opened','port':link.port,'usb_serial':identity,'generation':generation})
            link.write(b'{"cmd":"status"}\n')
            link.write((json.dumps({'cmd':'raw','enabled':a.raw})+'\n').encode())
            if first and a.command:link.write((json.dumps(json.loads(a.command))+'\n').encode())
        def disconnected(error):
            links.pop(node,None);issues[node]=error
            event(0,{'connection':'disconnected','message':error,'generation':generation})
        with (folder/(node+'.csi')).open('xb') as file:
            def consume(data):
                nonlocal raw_count,first_rx_us,last_rx_us,segment_count,rx_boot,last_flush,last_request
                if time.monotonic()-last_request>1.5:
                    links[node].write(b'{"cmd":"status"}\n');last_request=time.monotonic()
                file.write(data);got_status=False
                for kind,item,_ in decoder.feed(data):
                    if kind==1:
                        raw_count+=1
                        if item['rx_boot']!=rx_boot:
                            rx_boot=item['rx_boot'];first_rx_us=None;segment_count=0
                        if first_rx_us is None:first_rx_us=item['rx_us']
                        last_rx_us=item['rx_us'];segment_count+=1
                    else:
                        if kind in (2,3):
                            item['_received']=time.monotonic();item['_generation']=generation
                        if kind==2:
                            got_status=True
                            issues.pop(node,None)
                        event(kind,item)
                counters[node]={'raw_frames':raw_count,'crc_errors':crc_total+decoder.bad_crc,'resync_bytes':discard_total+decoder.discarded,
                                'received_raw_hz':(segment_count-1)*1e6/(last_rx_us-first_rx_us) if first_rx_us is not None and last_rx_us>first_rx_us else None}
                if time.monotonic()-last_flush>1:file.flush();last_flush=time.monotonic()
                return got_status
            reconnecting_reader(port,a.baud,stop,retries[node],opened,consume,disconnected)
    actions=queue.Queue()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*_):pass
        def do_GET(self):
            if self.path=='/':body=Path(__file__).with_name('dashboard.html').read_bytes();mime='text/html; charset=utf-8'
            elif self.path=='/activity_map.js':body=Path(__file__).with_name('activity_map.js').read_bytes();mime='application/javascript; charset=utf-8'
            elif self.path=='/status':
                now=time.monotonic();fresh={n:dict(s,_feature_age_s=now-(s.get('_feature_received') or 0)) if now-s.get('_received',0)<2.5 else dict(s,state='INVALID',reason='USB_DISCONNECTED_OR_STALE',score=None) for n,s in states.items()}
                body=json.dumps(dict(states=fresh,fused=fusion(states,[n for n in ports if n!='tx'],now),label=label,history=history.snapshot(now-started),elapsed_s=now-started,history_seconds=HISTORY_SECONDS,poll_ms=GUI_POLL_MS,issues=issues,counters=counters,folder=str(folder),remaining=max(0,math.ceil(deadline-now)) if deadline else None)).encode();mime='application/json'
            else:self.send_error(404);return
            self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(body)
        def do_POST(self):
            if self.path!='/action':self.send_error(404);return
            origin=self.headers.get('Origin')
            if origin and origin!=f'http://127.0.0.1:{a.web_port}':self.send_error(403);return
            try:
                size=int(self.headers.get('Content-Length','0'))
                if not 0<size<8192:raise ValueError()
                item=json.loads(self.rfile.read(size))
                if not isinstance(item,dict):raise ValueError()
                actions.put(item)
            except ValueError:self.send_error(400);return
            self.send_response(202);self.end_headers();self.wfile.write(b'{}')
    web=ThreadingHTTPServer(('127.0.0.1',a.web_port),Handler)
    workers=[threading.Thread(target=receiver,args=(n,port),daemon=True) for n,port in ports.items()]
    workers.append(threading.Thread(target=web.serve_forever,daemon=True))
    for thread in workers:thread.start()
    print(f'Interfaz: http://127.0.0.1:{a.web_port}/\nSesión: {folder}',flush=True)
    if a.open:webbrowser.open(f'http://127.0.0.1:{a.web_port}/')
    def mark(new_label,notes=''):
        nonlocal label
        label=new_label;ground.writerow([datetime.now().astimezone().isoformat(),time.monotonic()-started,label,notes]);ground_file.flush();log('ground_truth',label=label,notes=notes)
    try:
        while not stop.is_set() and (not a.duration or time.monotonic()-started<a.duration):
            while not actions.empty():
                action=actions.get_nowait()
                try:
                    if action.get('action')=='command':send(action['node'],action['command'])
                    elif action.get('action')=='reconnect':
                        for n in ports:retries[n].set()
                        issues.pop('action',None)
                    elif action.get('action')=='calibrate':
                        receivers=[n for n in ports if n!='tx']
                        if not receivers or any(n not in links for n in receivers):
                            raise ValueError('Conecta todos los receptores antes de calibrar.')
                        for n in receivers:send(n,{'cmd':'calibrate_after_delay'})
                        schedule=['CALIBRATION_START'];deadline=time.monotonic()+CALIBRATION_DELAY_SECONDS
                        mark('CALIBRATION_COUNTDOWN',action.get('notes',''))
                        issues.pop('action',None)
                    elif action.get('action')=='cancel_calibration_wait':
                        if schedule==['CALIBRATION_START'] or any(v.get('calibration_wait_s',0)>0 for v in states.values()):
                            for n in ports:
                                if n!='tx' and n in links:send(n,{'cmd':'cancel_calibration'})
                            schedule=[];deadline=0;mark('CALIBRATION_CANCELLED')
                    elif action.get('action')=='mark':mark(str(action['label'])[:100],str(action.get('notes',''))[:1000]);schedule=[];deadline=0
                    elif action.get('action') in ('sequence','human_sequence'):
                        if fusion(states,[n for n in ports if n!='tx'],time.monotonic())=='INVALID':raise ValueError('Necesitas RX calibrados y adquisición válida.')
                        subject='HUMAN_WALK' if action['action']=='human_sequence' else 'SMALL_OBJECT_PASS'
                        schedule=['LOCAL_EMPTY_1',subject+'_1','LOCAL_EMPTY_2',subject+'_2','LOCAL_EMPTY_3'];deadline=time.monotonic()
                    elif action.get('action')=='metadata':metadata['operator_setup']=action['setup'];(folder/'metadata.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
                    elif action.get('action')=='stop_session':
                        if schedule==['CALIBRATION_START']:
                            for n in ports:
                                if n!='tx' and n in links:send(n,{'cmd':'cancel_calibration'})
                        stop.set()
                    else:raise ValueError('Acción desconocida')
                except (ValueError,KeyError,OSError) as error:log('action_error',message=str(error));issues['action']=str(error)
            if stop.is_set():break
            if deadline and time.monotonic()>=deadline:
                if schedule:
                    phase=schedule.pop(0)
                    if phase=='CALIBRATION_START':
                        deadline=0
                        try:
                            receivers=[n for n in ports if n!='tx']
                            if any(n not in links for n in receivers):raise ValueError('Receptor desconectado durante la espera.')
                            mark('LOCAL_BACKGROUND_CALIBRATION')
                        except (ValueError,OSError) as error:
                            issues['action']=str(error);mark('CALIBRATION_START_FAILED',str(error))
                        continue
                    mark(phase)
                    deadline=time.monotonic()+60
                else:mark('SEQUENCE_COMPLETE');deadline=0
            try:
                node,kind,item=incoming.get(timeout=.1)
                log('device',node=node,type=kind,data=item)
                interval=history.update(node,kind,item,item.get('_received',time.monotonic())-started,label,config.get(node),states.get(node,{}).get('tuning',{}).get('output_hz',250))
                if kind==0:
                    states[node]={'state':'INVALID','reason':'USB_'+item['connection'].upper(),'_received':0}
                    if schedule or deadline:
                        schedule=[];deadline=0;mark('SEQUENCE_INTERRUPTED_USB','Repeat sequence after link recovery')
                if kind==2:
                    if item.get('measurement_valid',item.get('state') in ('ACTIVE','CLEAR')):
                        item['_result_interval_s']=states.get(node,{}).get('_result_interval_s')
                        item['_filter_fs']=states.get(node,{}).get('_filter_fs')
                        item['_feature_received']=states.get(node,{}).get('_feature_received')
                    states[node]=item
                    if node not in metadata['firmware']:
                        metadata['firmware'][node]={k:item.get(k) for k in ['model','board','transport','tuning','idf','commit','source_sha256','elf_sha256','esp_csi_commit','gain_component','device_id','tx_mac','tx_power_quarter_dbm','config']}
                        (folder/'metadata.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
                    config[node]=item.get('config',{});(folder/'config.json').write_text(json.dumps(config,indent=2),encoding='utf-8')
                if kind==3:
                    scores.writerow([time.monotonic()-started,node,item.get('rx_us'),item.get('state'),item.get('score') if item.get('measurement_valid',item.get('state') in ('ACTIVE','CLEAR')) else '',item.get('reference_id'),item.get('amplitude_calibrated'),item.get('measurement_valid'),item.get('score_units')]);score_file.flush()
                    # Keep slow diagnostics/config, but update scores and fusion
                    # for EVERY computed window. Carrier arrays stay in the log.
                    states[node]=dict(states.get(node,{}),**{k:v for k,v in item.items() if k!='carriers'},
                                      _result_interval_s=interval,_filter_fs=item.get('filter_fs'),_feature_received=item.get('_received'))
            except queue.Empty:pass
            fused=fusion(states,[n for n in ports if n!='tx'],time.monotonic())
            if fused!=last_fused:log('fusion',state=fused,label=label);last_fused=fused
    except KeyboardInterrupt:pass
    finally:
        stop.set()
        for thread in workers[:-1]:thread.join(timeout=3)
        web.shutdown();web.server_close();log('session_end',counters=counters,issues=issues)
        metadata['acquisition_summary']=counters
        (folder/'metadata.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
        event_file.close();ground_file.close();score_file.close()
        print(json.dumps(dict(counters=counters,issues=issues)),flush=True)


if __name__=='__main__':main()
