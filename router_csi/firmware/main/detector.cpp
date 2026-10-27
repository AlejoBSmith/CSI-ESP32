#include "detector.h"
#include <functional>

float RatDetector::median(float* a,unsigned n){if(!n)return 0;std::nth_element(a,a+n/2,a+n);float x=a[n/2];return n&1?x:(x+*std::max_element(a,a+n/2))/2;}
float RatDetector::quantile(const float* a,unsigned n,float q){if(!n)return 0;float p=q*(n-1);unsigned i=unsigned(p);return a[i]+(p-i)*(a[std::min(i+1,n-1)]-a[i]);}
void RatDetector::selectBand(){
    buildBank();
    selected=0;std::fill_n(mask,MAX_CARRIERS,false);
    if(!config.detector_kind||!config.carrier_mode){for(unsigned b:fixed_bins)active_bins[selected++]=b;}
    else for(unsigned b=0;b<64;++b)if(physical(b))active_bins[selected++]=b;
    required_carriers=unsigned(std::ceil(config.coverage_fraction*selected));
    for(unsigned k=0;k<selected;++k)mask[active_bins[k]]=true;
    top_count=std::min<unsigned>(config.top_k,selected);for(unsigned i=0;i<top_count;++i)top[i]=active_bins[i];
}
void RatDetector::clearContinuity(){
    head=filled=since_hop=0;pending=false;grid_origin=last_committed_us=0;impulse_count=impulse_head=0;lowpass_ready=false;
    usable=window_slots_valid=0;window_coverage=0;measurement_valid=emit=legacy_valid=false;fast={};slow={};legacy_score=0;fusion_source="NONE";fsm.clear();
    std::fill_n(window_valid,MAX_CARRIERS,false);
    for(auto& s:scales)s=ScaleResult{};
    bank_raw=bank_dominant_ms=confirmation_progress=0;bank_valid=bank_support=0;bank_confirmed=false;confirmation.clear();
}
void RatDetector::reset(const char* why){
    calibrated=calibrating=false;cal_frames=cal_kept=0;cal_valid_us=0;calibration_progress=0;
    last_us=rate_start=0;rate_frames=0;signature=0;clearContinuity();selectBand();
    score=turbulence=turbulence_relative_iqr=temporal_autocorr=shape_displacement=0;state="PROVISIONAL";reason=why;
}
void RatDetector::startCalibration(){
    calibrated=false;calibrating=true;cal_frames=cal_kept=0;cal_valid_us=0;calibration_progress=0;
    state="CALIBRATING";reason="AMPLITUDE_REFERENCE";
}
void RatDetector::fail(const char* why){
    if(std::strcmp(reason,why)){last_recovery=why;++recovery_count;}
    clearContinuity();score=0;state="INVALID";reason=why;
}
void RatDetector::idle(uint64_t now){if(!last_us||now-last_us>Experiment::no_csi_timeout_us)fail("NO_CSI");}
void RatDetector::append(bool valid,const float* shape,float value,uint32_t seq){
    if(valid){
        float temp[15];unsigned n=std::min<unsigned>(++impulse_count,config.hampel_window);
        impulses[impulse_head]=value;impulse_head=(impulse_head+1)%config.hampel_window;
        std::copy_n(impulses,n,temp);float center=median(temp,n);
        for(unsigned i=0;i<n;++i)temp[i]=std::fabs(impulses[i]-center);
        float scale=1.4826f*median(temp,n);
        if(n==config.hampel_window&&std::fabs(value-center)>config.hampel_sigma*std::max(scale,Experiment::hampel_min_scale))value=center;
        if(config.cutoff>0){if(!lowpass_ready){lowpass.configure(Experiment::output_hz,config.cutoff,.70710678f);lowpass.seed(value);lowpass_ready=true;}value=lowpass.add(value);}
        ++admitted_count;
    }else{++missing_slots;++missing_values;lowpass_ready=false;impulse_count=impulse_head=0;}
    cv[head]=valid?value:NAN;sequences[head]=valid?seq:0;
    for(unsigned k=0;k<selected;++k)normalized[k][head]=valid?shape[k]:NAN;
    head=(head+1)%capacity();filled=std::min<unsigned>(filled+1,capacity());++since_hop;
}
void RatDetector::commit(){append(pending_valid,pending_shape,pending_cv,pending_seq);if(pending_valid)last_committed_us=pending_us;result_us=pending_us;}
void RatDetector::add(const Frame& f){
    const auto& m=f.meta;emit=false;
    if(m.channel!=config.channel){fail("CHANNEL_MISMATCH");return;}
    if(m.csi_len!=128||m.bandwidth){fail("INVALID_CSI_LAYOUT");return;}
    const uint64_t sig=(uint64_t(m.csi_len)<<40)|(uint64_t(m.channel)<<32)|(m.flags&4);
    if(signature&&sig!=signature){reset("PHY_CHANGED");startCalibration();}signature=sig;
    if(last_us&&m.tx_boot!=tx_boot){fail("LINK_SESSION_CHANGED");last_us=rate_start=0;rate_frames=0;}
    if(last_us&&(m.rx_us<=last_us||m.tx_seq<=last_seq)){++excess_packets;return;}
    const uint64_t dt=last_us?m.rx_us-last_us:0;
    if(!rate_start)rate_start=m.rx_us;
    ++rate_frames;if(m.rx_us-rate_start>=Experiment::rate_interval_us){effective_fs=rate_frames*1e6f/(m.rx_us-rate_start);rate_frames=0;rate_start=m.rx_us;}
    last_us=m.rx_us;last_seq=m.tx_seq;tx_boot=m.tx_boot;
    if(last_drops!=m.queue_drops){last_drops=m.queue_drops;fail("QUEUE_DROP");}
    if(!selected||layout_ht!=bool(m.flags&4)){layout_ht=bool(m.flags&4);selectBand();}
    float amplitudes[64],shape[BAND]{},coarse[FIXED_BAND];
    for(unsigned k=0;k<64;++k)amplitudes[k]=std::hypot(float(f.iq[2*k]),float(f.iq[2*k+1]));
    float coarse_mean=0;
    for(unsigned k=0;k<FIXED_BAND;++k){
        float sum=0;unsigned n=0;int bin=int(fixed_bins[k]);
        for(int j=bin-2;j<=bin+2;++j)if(physical(j)&&!((m.flags&1)&&j<2)){sum+=amplitudes[j];++n;}
        coarse[k]=n?sum/n:0;coarse_mean+=coarse[k];
    }
    coarse_mean/=FIXED_BAND;float value=0;
    if(coarse_mean>Experiment::amplitude_epsilon){for(float x:coarse)value+=(x/coarse_mean-1)*(x/coarse_mean-1);value=std::sqrt(value/FIXED_BAND);}
    float mean=0;unsigned positive=0;
    for(unsigned k=0;k<selected;++k){
        unsigned bin=active_bins[k];float x=0;
        if(!config.detector_kind||!config.carrier_mode)x=coarse[k];
        else if(config.carrier_mode==2){unsigned n=0;for(int j=int(bin)-1;j<=int(bin)+1;++j)if(physical(j)&&!((m.flags&1)&&j<2)){x+=amplitudes[j];++n;}x=n?x/n:0;}
        else x=amplitudes[bin];
        shape[k]=x;mean+=x;positive+=x>Experiment::amplitude_epsilon;
    }
    mean/=selected;bool valid=mean>Experiment::amplitude_epsilon&&positive>=required_carriers;
    if(calibrating&&valid){
        ++cal_frames;rng=1664525*rng+1013904223;
        unsigned slot=cal_frames<=CAL_SAMPLES?cal_frames-1:rng%cal_frames;
        if(slot<CAL_SAMPLES)for(unsigned k=0;k<selected;++k)reservoir[k][slot]=shape[k];
        cal_kept=std::min(cal_frames,CAL_SAMPLES);
        if(dt&&dt<=Experiment::calibration_max_gap_us)cal_valid_us+=dt;
        calibration_progress=std::min(1.f,float(cal_valid_us)/Experiment::amplitude_cal_us);
        if(cal_valid_us>=Experiment::amplitude_cal_us){float temp[CAL_SAMPLES];for(unsigned k=0;k<selected;++k){std::copy_n(reservoir[k],cal_kept,temp);amp_median[active_bins[k]]=median(temp,cal_kept);}calibrated=true;calibrating=false;++reference_id;}
    }
    if(valid)for(unsigned k=0;k<selected;++k)shape[k]/=mean;
    const uint64_t period=uint64_t(1000000/Experiment::output_hz);
    if(!grid_origin)grid_origin=m.rx_us-period/2;
    uint64_t slot=(m.rx_us-grid_origin)/period;
    float phase=float((m.rx_us-grid_origin)%period),distance=std::fabs(phase-float(period)*.5f);
    // One real packet may occupy a vacant neighbor only within 0.75 periods.
    // No packet is reused and adjacent admitted packets stay >= half a period apart.
    if(pending&&slot==pending_slot+2&&phase<period*.25f){--slot;distance=phase+period*.5f;++shifted_packets;}
    else if(pending&&slot==pending_slot&&phase>period*.75f&&m.rx_us>=pending_us+period/2){++slot;distance=period*1.5f-phase;++shifted_packets;}
    else if(pending&&slot<pending_slot){
        distance=std::fabs(float(int64_t(m.rx_us)-int64_t(grid_origin+pending_slot*period+period/2)));
        if(distance>period*.75f){++excess_packets;return;}slot=pending_slot;
    }
    if(pending&&slot>pending_slot){
        if(slot-pending_slot>=capacity()){fail("SAMPLE_GAP");grid_origin=m.rx_us-period/2;slot=0;distance=0;}
        else{
            commit();for(uint64_t missing=pending_slot+1;missing<slot;++missing)append(false,nullptr,0,0);
            result_us=grid_origin+slot*period;
            if(filled>=(config.detector_kind?config.fast_window:config.window)&&since_hop>=config.hop){since_hop%=config.hop;evaluate();}
            else if(filled<(config.detector_kind?config.fast_window:config.window)){measurement_valid=false;state="INVALID";reason="WINDOW_WARMUP";}
        }
        pending=false;
    }
    if(last_committed_us&&m.rx_us-last_committed_us<period/2){++excess_packets;return;}
    if(!pending||distance<pending_distance){
        if(pending)++excess_packets;
        pending=true;pending_valid=valid;pending_slot=slot;pending_distance=distance;pending_us=m.rx_us;pending_seq=m.tx_seq;pending_cv=value;
        std::copy_n(shape,BAND,pending_shape);
    }else ++excess_packets;
}
float RatDetector::legacy(unsigned window,bool& valid,bool diagnostics){
    valid=false;if(filled<window)return 0;
    float temp[MAX_WINDOW],sum=0;unsigned n=0,pairs=0;double sx=0,sy=0,sxx=0,syy=0,sxy=0;
    for(unsigned i=window;i>0;--i){float x=cv[index(i)];if(std::isfinite(x)){temp[n++]=x;sum+=x;}
        if(i>1){float y=cv[index(i-1)];if(std::isfinite(x)&&std::isfinite(y)){sx+=x;sy+=y;sxx+=x*x;syy+=y*y;sxy+=x*y;++pairs;}}}
    if(n<unsigned(std::ceil(Experiment::sample_coverage_fraction*window))||effective_fs<config.min_fs)return 0;
    std::sort(temp,temp+n);float mean=sum/n,iqr=(quantile(temp,n,.75f)-quantile(temp,n,.25f))/std::max(std::fabs(mean),Experiment::feature_epsilon);
    double den=pairs?std::sqrt(std::max(0.,(sxx-sx*sx/pairs)*(syy-sy*sy/pairs))):0;
    float rho=den>1e-12?std::clamp(float((sxy-sx*sy/pairs)/den),-1.f,1.f):0;
    float floor=pairs?std::min(.95f,Experiment::autocorr_noise_sigma/std::sqrt(float(pairs))):1;
    if(diagnostics){turbulence=mean;turbulence_relative_iqr=iqr;temporal_autocorr=rho;coherence_floor=floor;lag_pairs=pairs;}
    valid=true;return 100*iqr*std::sqrt(std::max(0.f,(rho-floor)/(1-floor)));
}
float RatDetector::unsortedQuantile(float* a,unsigned n,float q){
    if(!n){return 0;}
    float p=q*(n-1);unsigned i=unsigned(p);
    std::nth_element(a,a+i,a+n);float lo=a[i];
    return i+1<n?lo+(p-i)*(*std::min_element(a+i+1,a+n)-lo):lo;
}
ScaleResult RatDetector::scale(unsigned window){
    ScaleResult r;if(filled<window)return r;
    const unsigned cap=capacity(),start=index(window),tail=std::min(window,cap-start);
    float temp[MAX_WINDOW],rank[BAND];
    // Copy a whole row before evaluating it: avoid modulo and PSRAM reads
    // for every lag pair. Only real values enter the compact quantile array.
    std::copy_n(normalized[0]+start,tail,temp);std::copy_n(normalized[0],window-tail,temp+tail);
    for(unsigned i=0;i<window;++i){r.count+=std::isfinite(temp[i]);if(i+1<window)r.pairs+=std::isfinite(temp[i])&&std::isfinite(temp[i+1]);}
    r.coverage=float(r.count)/window;
    if(r.count<unsigned(std::ceil(Experiment::sample_coverage_fraction*window))||effective_fs<config.min_fs||r.pairs<3)return r;
    r.valid=true;float floor=std::min(.95f,Experiment::autocorr_noise_sigma/std::sqrt(float(r.pairs)));
    for(unsigned k=0;k<selected;++k){
        std::copy_n(normalized[k]+start,tail,temp);std::copy_n(normalized[k],window-tail,temp+tail);
        unsigned n=0,pairs=0;double sx=0,sy=0,sxx=0,syy=0,sxy=0;
        for(unsigned i=0;i<window;++i){float x=temp[i];
            if(i+1<window){float y=temp[i+1];if(std::isfinite(x)&&std::isfinite(y)){sx+=x;sy+=y;sxx+=x*x;syy+=y*y;sxy+=x*y;++pairs;}}
            if(std::isfinite(x))temp[n++]=x;
        }
        double den=pairs?std::sqrt(std::max(0.,(sxx-sx*sx/pairs)*(syy-sy*sy/pairs))):0;
        float rho=den>1e-12?std::clamp(float((sxy-sx*sy/pairs)/den),-1.f,1.f):0;
        float coherence=std::max(0.f,(rho-floor)/(1-floor));r.coherence+=coherence/selected;
        float lo=unsortedQuantile(temp,n,.25f),hi=unsortedQuantile(temp,n,.75f),iqr=hi-lo;
        r.carrier_iqr[k]=iqr;rank[k]=r.carrier_scores[k]=100*iqr*std::sqrt(coherence);r.votes+=rank[k]>=config.enter_score;
    }
    std::sort(rank,rank+selected,std::greater<float>());unsigned count=std::min<unsigned>(config.top_k,selected);
    r.score=median(rank,count);return r;
}
void RatDetector::buildBank(){
    bank_total=0;unsigned requested=std::clamp<unsigned>(config.scale_count,2,MAX_SCALES);
    const unsigned lo=config.fast_window,hi=config.window;
    for(unsigned j=0;j<requested;++j){
        unsigned w=j+1==requested?hi:unsigned(std::lround(lo*std::pow(double(hi)/lo,double(j)/(requested-1))));
        if(bank_total&&w<=scale_windows[bank_total-1])w=scale_windows[bank_total-1]+1;
        if(w>hi)break;
        scale_windows[bank_total++]=w;
    }
}
void RatDetector::scaleBank(){
    for(auto& s:scales)s=ScaleResult{};
    bank_valid=0;if(!bank_total)return;
    const unsigned n=std::min<unsigned>(filled,config.window),cap=capacity(),start=index(n),tail=std::min(n,cap-start);
    float inverse_pairs[MAX_WINDOW];unsigned count=0,pairs=0,target=0;bool previous=false;
    for(unsigned age=1;age<=n;++age){bool valid=std::isfinite(normalized[0][index(age)]);count+=valid;pairs+=valid&&previous;previous=valid;inverse_pairs[age-1]=pairs?1.f/pairs:0;
        if(target<bank_total&&age==scale_windows[target]){
            auto& r=scales[target];r.count=count;r.pairs=pairs;r.coverage=float(count)/age;
            r.valid=count>=unsigned(std::ceil(Experiment::sample_coverage_fraction*age))&&pairs>=3&&effective_fs>=config.min_fs;
            bank_valid+=r.valid;++target;
        }
    }
    if(!bank_valid)return;
    float row[MAX_WINDOW],coherence[MAX_SCALES],rank[BAND];
    for(unsigned k=0;k<selected;++k){
        std::copy_n(normalized[k]+start,tail,row);std::copy_n(normalized[k],n-tail,row+tail);std::reverse(row,row+n);
        StableLagCorrelation moments;unsigned compact=0,j=0;float newer=NAN;
        std::fill_n(coherence,MAX_SCALES,0.f);
        // Every ACF snapshot precedes quantile selection: compact prefixes are
        // nested multisets, so reordering an earlier prefix cannot corrupt later ones.
        for(unsigned age=1;age<=n;++age){float x=row[age-1];
            if(std::isfinite(x)&&std::isfinite(newer))moments.add(newer,x,inverse_pairs[age-1]);
            newer=x;if(std::isfinite(x))row[compact++]=x;
            if(j<bank_total&&age==scale_windows[j]){
                auto& r=scales[j];r.carrier_iqr[k]=NAN;
                if(r.valid){
                    float rho=moments.correlation();
                    float floor=std::min(.95f,Experiment::autocorr_noise_sigma/std::sqrt(float(r.pairs)));
                    coherence[j]=std::max(0.f,(rho-floor)/(1-floor));r.coherence+=coherence[j]/selected;
                }
                ++j;
            }
        }
        for(j=0;j<bank_total;++j){auto& r=scales[j];
            if(!r.valid||coherence[j]<=0)continue;
            float lo=unsortedQuantile(row,r.count,.25f),hi=unsortedQuantile(row,r.count,.75f),iqr=hi-lo;
            r.carrier_iqr[k]=iqr;r.carrier_scores[k]=100*iqr*std::sqrt(coherence[j]);r.votes+=r.carrier_scores[k]>=config.enter_score;
        }
    }
    for(unsigned j=0;j<bank_total;++j){auto& r=scales[j];if(!r.valid)continue;
        std::copy_n(r.carrier_scores,selected,rank);unsigned count_top=std::min<unsigned>(config.top_k,selected);
        if(count_top<selected)std::nth_element(rank,rank+count_top,rank+selected,std::greater<float>());
        r.score=median(rank,count_top);
    }
}
void RatDetector::evaluate(){
    emit=true;legacy_score=legacy(100,legacy_valid,true);fast={};slow={};
    const ScaleResult* display_ptr=nullptr;
    if(config.detector_kind==2){
        scaleBank();fast=scales[0];slow=scales[bank_total-1];const float* z[MAX_SCALES];bool valid[MAX_SCALES];
        for(unsigned j=0;j<bank_total;++j){z[j]=scales[j].carrier_scores;valid[j]=scales[j].valid;}
        bank_raw=ScaleConsensus::fuse(z,valid,bank_total,selected,config.consensus_scales,config.top_k,winning_scale,bank_support,measurement_valid);
        if(measurement_valid){score=confirmation.add(bank_raw,config.confirm_hops,result_us,uint64_t(config.hop)*10000,config.enter_score,confirmation_progress);}
        else{score=0;confirmation_progress=0;confirmation.clear();fsm.clear();}
        bank_confirmed=measurement_valid&&score>=config.enter_score;
        bank_dominant_ms=bank_support?10*std::sqrt(float(scale_windows[winning_scale])*scale_windows[winning_scale+bank_support-1]):0;
        fusion_source=measurement_valid?"BANK":"NONE";
        if(bank_support)display_ptr=&scales[winning_scale];
        else for(unsigned j=0;j<bank_total;++j)if(scales[j].valid)display_ptr=&scales[j];
        if(!display_ptr)display_ptr=&scales[0];
    }else{
        fast=scale(config.fast_window);slow=scale(config.window);
        if(config.detector_kind){
            measurement_valid=fast.valid||slow.valid;
            score=TemporalFusion::combine(fast.score,fast.valid,slow.score,slow.valid,config.enter_score,config.fast_weight,fast_weight_applied);
            fusion_source=!measurement_valid?"NONE":fast.valid&&slow.valid?"BOTH":fast.valid?"FAST":"SLOW";
        }else{score=legacy(config.window,measurement_valid,true);fusion_source=measurement_valid?"LEGACY":"NONE";}
        display_ptr=slow.valid?&slow:&fast;
    }
    const ScaleResult& display=*display_ptr;window_coverage=measurement_valid?display.coverage:std::max(fast.coverage,slow.coverage);
    if(config.detector_kind==2&&bank_support)for(unsigned j=winning_scale;j<winning_scale+bank_support;++j)window_coverage=std::min(window_coverage,scales[j].coverage);
    window_slots_valid=display.count;usable=measurement_valid?selected:0;votes=display.votes;
    std::fill_n(window_valid,MAX_CARRIERS,false);
    unsigned order[BAND];for(unsigned k=0;k<selected;++k){order[k]=k;unsigned b=active_bins[k];window_valid[b]=display.valid;features[b].z=display.carrier_scores[k];features[b].iqr=display.carrier_iqr[k];
        if(config.detector_kind==2&&bank_support)for(unsigned j=winning_scale;j<winning_scale+bank_support;++j)features[b].z=std::min(features[b].z,scales[j].carrier_scores[k]);}
    std::sort(order,order+selected,[&](unsigned a,unsigned b){return features[active_bins[a]].z>features[active_bins[b]].z;});
    top_count=std::min<unsigned>(config.top_k,selected);for(unsigned i=0;i<top_count;++i)top[i]=active_bins[order[i]];
    shape_displacement=NAN;
    if(config.detector_kind!=2){
        shape_displacement=0;unsigned shape_pairs=0,lag=std::max(1u,unsigned(Experiment::shape_lag_us*Experiment::output_hz/1000000));
        const unsigned n=std::min<unsigned>(filled,config.window),cap=capacity();
        if(n>lag)for(unsigned k=0;k<selected;++k){unsigned a=index(n),b=index(n-lag);
            for(unsigned i=0;i<n-lag;++i){
                if(std::isfinite(normalized[k][a])&&std::isfinite(normalized[k][b])){shape_displacement+=std::fabs(normalized[k][b]-normalized[k][a]);if(!k)++shape_pairs;}
                if(++a==cap){a=0;}if(++b==cap){b=0;}
            }
        }
        if(shape_pairs)shape_displacement*=100.f/(shape_pairs*selected);
    }
    seq0=seq1=0;for(unsigned i=std::min<unsigned>(filled,config.window);i>0;--i){auto s=sequences[index(i)];if(s){if(!seq0)seq0=s;seq1=s;}}
    if(!measurement_valid){score=0;state="INVALID";reason=effective_fs<config.min_fs?"LOW_EFFECTIVE_RATE":config.detector_kind==2?"INSUFFICIENT_SCALE_COVERAGE":"LOW_TEMPORAL_COVERAGE";return;}
    fsm.update(score,config);state=calibrated?(fsm.active?"ACTIVE":"CLEAR"):(calibrating?"CALIBRATING":"PROVISIONAL");
    reason=calibrated?"OK":(calibrating?"AMPLITUDE_REFERENCE":"AMPLITUDE_REFERENCE_REQUIRED");
}
