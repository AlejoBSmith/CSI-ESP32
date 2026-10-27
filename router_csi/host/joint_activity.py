"""Causal corroboration of nearby receivers; USB alignment is approximate.

Only computed features refresh the data. Status packets never prolong a score.
The combined index is dimensionless, not a probability or object count.
"""
from collections import deque
from dataclasses import dataclass
import math
import statistics
import threading

ENTER_INDEX=1.0
EXIT_INDEX=.625
PAIR_TOLERANCE_SECONDS=.150
PAIR_WAIT_SECONDS=.200
FEATURE_TTL_SECONDS=.750
CLOCK_WARMUP_SECONDS=1.0
CORRELATION_SECONDS=5.0
CORRELATION_MIN_PAIRS=30
CORRELATION_MAX_LAG_SECONDS=.5
# Thresholds are normalized independently; other DSP choices must agree.
PROCESSING_FIELDS=('window','fast_window','hop','top_k','detector_kind','carrier_mode',
                   'scale_count','consensus_scales','confirm_hops','cutoff',
                   'hampel_window','hampel_sigma','fast_weight','coverage_fraction',
                   'min_fs','profile','enter_hops','exit_hops')


def finite(x):return isinstance(x,(int,float)) and not isinstance(x,bool) and math.isfinite(x)

def corroborated_index(values):
    """Preserve stronger evidence only as far as the weakest RX corroborates it."""
    return min(max(values),2*min(values)) if values else 0.0


@dataclass(frozen=True)
class Sample:
    node:str
    t:float
    received:float
    score:float
    index:float
    raw_index:float
    valid:bool
    calibrated:bool
    quality:object
    period:float
    signature:tuple
    source_sha256:object=None


class Clock:
    def __init__(self):
        self.offset=None;self.first=None;self.offsets=deque();self.last_us=None
    def observe(self,us,received):
        if self.last_us is not None and us<=self.last_us:return None,False
        self.last_us=us
        if self.first is None:self.first=received
        delta=received-us/1e6;self.offsets.append((received,delta))
        while self.offsets and received-self.offsets[0][0]>30:self.offsets.popleft()
        offset=min(x[1] for x in self.offsets);jump=self.offset is not None and self.offset-offset>.150
        self.offset=offset
        if jump:self.first=received
        return us/1e6+offset,jump
    def ready(self,now):return self.first is not None and now-self.first>=CLOCK_WARMUP_SECONDS


class JointActivity:
    def __init__(self,names,started):
        self.names=tuple(names);self.started=started;self.lock=threading.RLock()
        self.meta={};self.context={};self.processing={};self.clocks={};self.pending={n:deque(maxlen=100) for n in self.names}
        self.latest={};self.last_us={};self.history=deque(maxlen=6000);self.corr=deque(maxlen=600)
        self.active=False;self.enter=0;self.exit=0;self.last_output=None;self.current=None;self.gap_open=False
        self.correlation=dict(correlation_peak=None,correlation_lag_ms=None,correlation_pairs=0,correlation_valid=False)
    def _clear_joint(self,now,reason):
        for q in self.pending.values():q.clear()
        self.corr.clear();self.correlation=dict(correlation_peak=None,correlation_lag_ms=None,correlation_pairs=0,correlation_valid=False)
        self.active=False;self.enter=self.exit=0;self.current=None
        self._gap(now,reason)
    def _gap(self,now,reason):
        if not self.gap_open:
            # A delayed status must break the curve after the last emitted
            # point, before any buffered pair emitted during recovery.
            gap_time=min(now,self.last_output+1e-6) if self.last_output is not None else now
            self.history.append(dict(t=gap_time-self.started,score=None,state='INVALID',quality=None,pair_skew_ms=None,reason=reason));self.gap_open=True
    def update(self,node,kind,item,received,config=None,output_hz=100):
        if node not in self.names:return
        with self.lock:
            if kind==0:
                self.clocks.pop(node,None);self.context.pop(node,None);self.processing.pop(node,None);self.latest.pop(node,None);self.last_us.pop(node,None)
                self._clear_joint(received,'USB_SEGMENT_CHANGED');return
            if kind==2:
                previous=self.meta.get(node,{});meta=dict(previous)
                for key in ('device_id','algorithm','score_units','source_sha256','config','tuning','router'):
                    if key in item:meta[key]=item[key]
                self.meta[node]=meta
                processing_changed=any(meta.get(k)!=previous.get(k) for k in ('algorithm','score_units','source_sha256','config'))
                ap_changed=meta.get('router',{}).get('bssid')!=previous.get('router',{}).get('bssid')
                if node in self.latest and (processing_changed or ap_changed):
                    self.latest.pop(node,None);self.processing.pop(node,None)
                    self._clear_joint(received,'RECEIVER_PROCESSING_CHANGED')
                if item.get('measurement_valid') is False:
                    self.latest.pop(node,None);self._clear_joint(received,'RECEIVER_INVALID')
                return
            if kind!=3:return
            meta=self.meta.setdefault(node,{})
            for key in ('algorithm','score_units'):
                if key in item:meta[key]=item[key]
            cfg=dict(config or meta.get('config') or {})
            hz=output_hz if finite(output_hz) and output_hz>0 else 100
            hop=cfg.get('hop',10);timing_valid=finite(hop) and hop>0
            period=hop/hz if timing_valid else .1
            context=(item.get('boot'),item.get('_generation',0),item.get('reference_id'))
            prior=self.context.get(node)
            if context!=prior:
                self._clear_joint(received,'RECEIVER_REFERENCE_OR_BOOT_CHANGED')
                if prior is None or context[:2]!=prior[:2]:self.clocks[node]=Clock();self.last_us.pop(node,None)
                self.context[node]=context
            us=item.get('rx_us')
            if not finite(us) or (node in self.last_us and us<=self.last_us[node]):return
            self.last_us[node]=us;clock=self.clocks.setdefault(node,Clock());mapped,jump=clock.observe(us,received)
            if mapped is None:return
            if jump:self._clear_joint(received,'USB_CLOCK_REANCHORED')
            score=item.get('score');threshold=cfg.get('enter_score',4)
            valid=bool(timing_valid and item.get('measurement_valid',item.get('state') in ('ACTIVE','CLEAR')) and finite(score) and score>=0 and finite(threshold) and threshold>0)
            raw=item.get('bank_raw') if item.get('bank_total',0)>0 else score
            if not finite(raw) or raw<0:raw=score
            quality=item.get('window_coverage');quality=max(0,min(1,quality)) if finite(quality) else None
            signature=(meta.get('algorithm'),meta.get('score_units'),hz,*(cfg.get(k) for k in PROCESSING_FIELDS))
            source=meta.get('source_sha256');source=source.casefold() if isinstance(source,str) and source else None
            processing=(signature,source,threshold,cfg.get('exit_score'))
            if node in self.processing and processing!=self.processing[node]:
                self._clear_joint(received,'RECEIVER_PROCESSING_CHANGED')
            self.processing[node]=processing
            point=Sample(node,mapped,received,score if valid else 0,score/threshold if valid else 0,raw/threshold if valid else 0,valid,bool(item.get('amplitude_calibrated',False)),quality,max(.001,period),signature,source)
            self.latest[node]=point
            if not valid:self._clear_joint(received,'RECEIVER_INVALID');return
            self.pending[node].append(point)
    def _limits(self):
        period=max((p.period for p in self.latest.values()),default=.1)
        return max(PAIR_TOLERANCE_SECONDS,.75*period),max(PAIR_WAIT_SECONDS,.85*period),max(FEATURE_TTL_SECONDS,2.5*period)
    def _reason(self,now):
        if not self.names:return 'NO_RECEIVERS'
        if any(n not in self.latest for n in self.names):return 'WAITING_FOR_RECEIVERS'
        tolerance,wait,ttl=self._limits()
        if any(not self.latest[n].valid or now-self.latest[n].t>ttl or now-self.latest[n].received>ttl for n in self.names):return 'STALE_OR_INVALID_FEATURE'
        if any(not self.clocks[n].ready(now) for n in self.names):return 'ALIGNMENT_WARMUP'
        samples=[self.latest[n] for n in self.names]
        if not self._compatible(samples):return 'PROCESSING_CONFIG_MISMATCH'
        ids=[self.meta.get(n,{}).get('device_id') for n in self.names]
        if all(ids) and len(set(ids))!=len(ids):return 'DUPLICATE_DEVICE_ID'
        bssids=[self.meta.get(n,{}).get('router',{}).get('bssid') for n in self.names]
        if all(bssids) and len(set(bssids))!=1:return 'DIFFERENT_AP'
        return None
    @staticmethod
    def _compatible(points):
        if any(p.signature!=points[0].signature for p in points[1:]):return False
        # Missing build metadata is allowed; two known different builds are not.
        return len({p.source_sha256 for p in points if p.source_sha256})<=1
    def _correlation(self):
        rows=list(self.corr);count=len(rows)
        empty=dict(correlation_peak=None,correlation_lag_ms=None,correlation_pairs=count,correlation_valid=False)
        if len(self.names)!=2 or count<CORRELATION_MIN_PAIRS:return empty
        intervals=[b[0]-a[0] for a,b in zip(rows,rows[1:]) if b[0]>a[0]]
        dt=statistics.median(intervals) if intervals else .1
        maxlag=min(10,int(CORRELATION_MAX_LAG_SECONDS/max(.001,dt)));best=None
        for lag in range(-maxlag,maxlag+1):
            if lag>=0:pairs=[(rows[i][1][0],rows[i+lag][1][1]) for i in range(count-lag)]
            else:pairs=[(rows[i-lag][1][0],rows[i][1][1]) for i in range(count+lag)]
            if len(pairs)<CORRELATION_MIN_PAIRS:continue
            ax=math.fsum(x for x,y in pairs)/len(pairs);ay=math.fsum(y for x,y in pairs)/len(pairs)
            vx=math.fsum((x-ax)**2 for x,y in pairs);vy=math.fsum((y-ay)**2 for x,y in pairs)
            if vx/len(pairs)<1e-8 or vy/len(pairs)<1e-8:continue
            rho=math.fsum((x-ax)*(y-ay) for x,y in pairs)/math.sqrt(vx*vy);rho=max(-1,min(1,rho))
            if best is None or rho>best[0]+1e-12 or (abs(rho-best[0])<1e-12 and abs(lag)<abs(best[1])):best=(rho,lag,len(pairs))
        return dict(correlation_peak=best[0],correlation_lag_ms=best[1]*dt*1000,correlation_pairs=best[2],correlation_valid=True) if best else empty
    def _emit(self,points,now):
        # Latest samples can agree while older queued samples straddle a change.
        if not self._compatible(points):
            self._clear_joint(now,'PROCESSING_CONFIG_MISMATCH');return None
        t=math.fsum(p.t for p in points)/len(points)
        if self.last_output is not None and t<=self.last_output:return None
        period=max(p.period for p in points)
        if self.last_output is not None and t-self.last_output>1.8*period:
            self.corr.clear();self.active=False;self.enter=self.exit=0;self._gap(t,'UNPAIRED_INTERVAL')
        self.last_output=t;value=corroborated_index([p.index for p in points])
        if value>=ENTER_INDEX:self.enter+=1;self.exit=0
        elif value<EXIT_INDEX:self.exit+=1;self.enter=0
        else:self.enter=0;self.exit=0
        if not self.active and self.enter>=2:self.active=True
        if self.active and self.exit>=3:self.active=False
        provisional=any(not p.calibrated for p in points)
        state='PROVISIONAL' if provisional else 'ACTIVE' if self.active else 'CLEAR'
        quality=min(p.quality for p in points) if all(p.quality is not None for p in points) else None
        self.corr.append((t,[math.log1p(p.raw_index) for p in points]))
        while self.corr and t-self.corr[0][0]>CORRELATION_SECONDS:self.corr.popleft()
        self.correlation=self._correlation();skew=(max(p.t for p in points)-min(p.t for p in points))*1000
        self.current=dict(t=t-self.started,score=value,state=state,mode='PAIR' if len(points)>1 else 'SINGLE_RX',measurement_valid=True,contributors=[p.node for p in points],expected_receivers=len(self.names),quality=quality,pair_skew_ms=skew,alignment='USB_APPROXIMATE',reason='AMPLITUDE_REFERENCE_PENDING' if provisional else 'OK',enter_score=ENTER_INDEX,exit_score=EXIT_INDEX,**self.correlation)
        self.history.append(dict(t=t-self.started,score=value,state=state,quality=quality,pair_skew_ms=skew));self.gap_open=False
        return dict(self.current)
    def advance(self,now):
        with self.lock:
            reason=self._reason(now)
            if reason:
                if reason not in ('WAITING_FOR_RECEIVERS','ALIGNMENT_WARMUP'):self._clear_joint(now,reason)
                return []
            tolerance,wait,ttl=self._limits();result=[]
            if len(self.names)==1:
                q=self.pending[self.names[0]]
                while q:
                    p=q.popleft()
                    if now-p.t<=ttl:
                        item=self._emit([p],now)
                        if item:result.append(item)
                return result
            while all(self.pending[n] for n in self.names):
                anchor_name=min(self.names,key=lambda n:self.pending[n][0].t);anchor=self.pending[anchor_name][0]
                if now-anchor.received<wait:break
                chosen={anchor_name:anchor};indexes={anchor_name:0}
                for n in self.names:
                    if n==anchor_name:continue
                    q=self.pending[n];i=min(range(len(q)),key=lambda i:abs(q[i].t-anchor.t));p=q[i]
                    if abs(p.t-anchor.t)<=tolerance:chosen[n]=p;indexes[n]=i
                if len(chosen)!=len(self.names):
                    self.pending[anchor_name].popleft();continue
                # An unmatched old anchor must not shift every subsequent
                # pair by one hop. Prefer a strictly closer reciprocal match
                # already present in the anchor queue before consuming points.
                anchor_queue=self.pending[anchor_name]
                inferior=any(min(abs(p.t-other.t) for other in anchor_queue)+1e-9<abs(p.t-anchor.t)
                             for n,p in chosen.items() if n!=anchor_name)
                if inferior:
                    self.pending[anchor_name].popleft();continue
                points=[chosen[n] for n in self.names]
                for n,i in indexes.items():
                    for _ in range(i+1):self.pending[n].popleft()
                if any(now-p.t>ttl or not p.valid for p in points):continue
                if max(p.t for p in points)-min(p.t for p in points)>tolerance:continue
                item=self._emit(points,now)
                if item:result.append(item)
            return result
    def snapshot(self,now):
        with self.lock:
            reason=self._reason(now);ttl=self._limits()[2]
            if not reason and self.current and now-(self.current['t']+self.started)<=ttl:return dict(self.current)
            return dict(score=None,state='INVALID',mode='INVALID',measurement_valid=False,contributors=[],expected_receivers=len(self.names),quality=None,pair_skew_ms=None,alignment='USB_APPROXIMATE',reason=reason or 'NO_FRESH_PAIR',enter_score=ENTER_INDEX,exit_score=EXIT_INDEX,**self.correlation)
    def history_snapshot(self,now):
        with self.lock:
            reason=self._reason(now)
            if reason:self._gap(now,reason)
            return [dict(p) for p in self.history if p['t']>=now-self.started-60]
