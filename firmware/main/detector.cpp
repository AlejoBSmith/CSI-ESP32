#include "detector.h"

float RatDetector::median(float* a,unsigned n){
    if(!n){return 0;}
    std::nth_element(a,a+n/2,a+n);float m=a[n/2];
    if(!(n&1)){m=(m+*std::max_element(a,a+n/2))/2;}
    return m;
}
float RatDetector::deviation(float* a,unsigned n,float m){
    for(unsigned i=0;i<n;++i){a[i]=std::fabs(a[i]-m);}
    return median(a,n);
}
void RatDetector::clearContinuity(){
    filled=head=since_hop=0;next_grid_us=previous_filter_us=0;
    usable=missing_now=0;measurement_valid=false;emit=false;fsm.clear();
    std::fill_n(window_valid,MAX_CARRIERS,false);
    std::fill_n(valid_run,MAX_CARRIERS,0);
    std::fill_n(carrier_count,MAX_CARRIERS,0);
    std::fill_n(carrier_head,MAX_CARRIERS,0);
    std::fill_n(filter_ready,MAX_CARRIERS,false);
    std::fill_n(previous_filtered,MAX_CARRIERS,NAN);
}
void RatDetector::reset(const char* why){
    calibrated=calibrating=amplitude_ready=false;cal_start=cal_valid_us=0;
    cal_frames=cal_kept=0;selected=required_carriers=0;calibration_progress=0;
    last_us=rate_start=0;rate_frames=rate_missing=0;signature=0;filter_fs=0;
    std::fill_n(mask,MAX_CARRIERS,false);std::fill_n(finite_count,MAX_CARRIERS,0);
    clearContinuity();score=0;votes=top_count=0;state="PROVISIONAL";reason=why;
}
void RatDetector::startCalibration(){
    // Keep producing provisional windows while collecting amplitude only.
    calibrated=false;calibrating=true;cal_start=cal_valid_us=0;cal_frames=cal_kept=0;
    calibration_progress=0;std::fill_n(finite_count,MAX_CARRIERS,0);
    fsm.clear();state="CALIBRATING";reason="AMPLITUDE_REFERENCE";
}
void RatDetector::fail(const char* why){
    if(std::strcmp(reason,why)){last_recovery=why;++recovery_count;}
    clearContinuity();score=0;votes=top_count=0;state="INVALID";reason=why;
    // Transient acquisition faults never destroy a valid amplitude reference.
}
void RatDetector::idle(uint64_t now){
    if(!last_us||now-last_us>Experiment::no_csi_timeout_us)fail("NO_CSI");
}
void RatDetector::provisionalAmplitude(const Frame& f){
    selected=0;
    for(unsigned k=0;k<carriers;++k){
        unsigned bin=k%64;bool physical=(bin>=2&&bin<=26)||(bin>=38&&bin<=63);
        float a=f.meta.gain*std::hypot(float(f.iq[2*k]),float(f.iq[2*k+1]));
        mask[k]=physical&&a>Experiment::amplitude_epsilon&&std::isfinite(a);
        if(mask[k]){amp_median[k]=a;++selected;}
    }
    amplitude_ready=selected>=config.top_k;
    required_carriers=std::max<unsigned>(config.top_k,unsigned(std::ceil(config.coverage_fraction*selected)));
}
void RatDetector::establishAmplitude(){
    float temp[CAL_SAMPLES];selected=0;
    for(unsigned k=0;k<carriers;++k){
        unsigned bin=k%64;bool physical=(bin>=2&&bin<=26)||(bin>=38&&bin<=63);
        mask[k]=physical&&finite_count[k]>Experiment::amplitude_presence*cal_frames;
        if(!mask[k])continue;
        std::copy_n(reservoir[k],cal_kept,temp);amp_median[k]=median(temp,cal_kept);
        mask[k]=amp_median[k]>Experiment::amplitude_epsilon&&std::isfinite(amp_median[k]);
        if(mask[k])++selected;
    }
    amplitude_ready=selected>=config.top_k;
    required_carriers=std::max<unsigned>(config.top_k,unsigned(std::ceil(config.coverage_fraction*selected)));
    clearContinuity();
}
void RatDetector::add(const Frame& f){
    float temp[MAX_CARRIERS];const auto&m=f.meta;emit=false;
    if(m.channel!=config.channel){fail("CHANNEL_MISMATCH");return;}
    if(m.csi_len<128||m.csi_len>MAX_CSI_BYTES||(m.csi_len&1)||m.bandwidth){fail("INVALID_CSI_LAYOUT");return;}
    uint64_t sig=(uint64_t(m.csi_len)<<40)|(uint64_t(m.channel)<<32)|(m.bandwidth<<24)|(m.sig_mode<<20)|(m.mcs<<12)|(m.stbc<<8)|(m.sig_mode?0:m.rate);
    if(signature&&signature!=sig){reset("PHY_CHANGED");startCalibration();last_recovery="PHY_CHANGED";++recovery_count;}
    signature=sig;carriers=m.csi_len/2;
    if(last_us&&(m.rx_us<=last_us||m.tx_boot!=tx_boot)){
        fail("TX_RESTART");last_us=rate_start=0;rate_frames=rate_missing=0;
    }
    if(last_us&&m.tx_seq<=last_seq)return;
    uint64_t dt=last_us?m.rx_us-last_us:0;
    if(dt>Experiment::max_sample_gap_us){fail("SAMPLE_GAP");rate_start=0;rate_frames=rate_missing=0;}
    if(!rate_start)rate_start=m.rx_us;
    if(last_us&&dt<=Experiment::max_sample_gap_us)rate_missing+=m.tx_seq-last_seq-1;
    ++rate_frames;
    if(m.rx_us-rate_start>=Experiment::rate_interval_us){
        effective_fs=rate_frames*1e6f/(m.rx_us-rate_start);
        loss_fraction=float(rate_missing)/(rate_missing+rate_frames);
        rate_start=m.rx_us;rate_frames=rate_missing=0;
    }
    last_us=m.rx_us;last_seq=m.tx_seq;tx_boot=m.tx_boot;
    if(m.queue_drops!=last_drops){last_drops=m.queue_drops;fail("QUEUE_DROP");return;}
    if(!(m.flags&2)){fail("GAIN_WARMUP");return;}
    if(effective_fs<config.min_fs||effective_fs<=2*config.cutoff){fail("LOW_EFFECTIVE_RATE");return;}
    if(!filter_fs||std::fabs(effective_fs-filter_fs)>Experiment::rate_change_fraction*filter_fs){
        filter_fs=effective_fs;fail("FILTER_RATE_ADAPTED");
    }
    if(!amplitude_ready)provisionalAmplitude(f);
    if(!amplitude_ready){fail("TOO_FEW_VALID_CARRIERS");return;}
    if(calibrating){
        ++cal_frames;rng=1664525*rng+1013904223;
        unsigned index=cal_frames<=CAL_SAMPLES?cal_frames-1:rng%cal_frames;
        for(unsigned k=0;k<carriers;++k){
            float a=m.gain*std::hypot(float(f.iq[2*k]),float(f.iq[2*k+1]));
            if((m.flags&1)&&k<2)a=0;
            if(a>0&&std::isfinite(a))++finite_count[k];
            if(index<CAL_SAMPLES)reservoir[k][index]=a;
        }
        cal_kept=std::min(cal_frames,CAL_SAMPLES);
        if(dt<=Experiment::max_interpolation_gap_us)cal_valid_us+=dt;
        calibration_progress=std::min(1.f,float(cal_valid_us)/Experiment::amplitude_cal_us);
        if(cal_valid_us>=Experiment::amplitude_cal_us){
            establishAmplitude();
            if(amplitude_ready){calibrated=true;calibrating=false;++reference_id;}
            else{startCalibration();fail("AMPLITUDE_REFERENCE_RETRY");return;}
        }
    }
    const uint64_t interpolation_limit=std::min<uint64_t>(Experiment::max_interpolation_gap_us,uint64_t(500000/config.cutoff));
    if(dt>interpolation_limit)fail("INTERPOLATION_GAP");
    missing_now=0;
    for(unsigned k=0;k<carriers;++k){
        if(!mask[k])continue;
        float a=m.gain*std::hypot(float(f.iq[2*k]),float(f.iq[2*k+1]));
        if(!(a>0)||!std::isfinite(a)){
            ++missing_now;++missing_values;valid_run[k]=carrier_count[k]=carrier_head[k]=0;
            filter_ready[k]=false;current_filtered[k]=NAN;continue;
        }
        float x=20*std::log10((a+Experiment::amplitude_epsilon)/(amp_median[k]+Experiment::amplitude_epsilon));
        if(!filter_ready[k]){
            low1[k].configure(filter_fs,config.cutoff,.5411961f);low2[k].configure(filter_fs,config.cutoff,1.306563f);
            low1[k].seed(x);low2[k].seed(x);filter_ready[k]=true;
        }
        unsigned nh=std::min<unsigned>(++carrier_count[k],config.hampel_window);
        impulses[k][carrier_head[k]]=x;carrier_head[k]=(carrier_head[k]+1)%config.hampel_window;
        std::copy_n(impulses[k],nh,temp);float center=median(temp,nh);
        std::copy_n(impulses[k],nh,temp);float scale=1.4826f*deviation(temp,nh,center);
        if(nh==config.hampel_window&&std::fabs(x-center)>config.hampel_sigma*std::max(scale,Experiment::hampel_min_scale))x=center;
        current_filtered[k]=low2[k].add(low1[k].add(x));
    }
    const uint64_t period=uint64_t(1000000/std::min(Experiment::output_hz,config.min_fs));
    if(!next_grid_us)next_grid_us=m.rx_us;
    bool keep=false;
    // Catch up a bounded, short gap instead of dropping a grid point on jitter.
    while(next_grid_us<=m.rx_us){
        float fraction=previous_filter_us?float(next_grid_us-previous_filter_us)/float(m.rx_us-previous_filter_us):1;
        fraction=std::clamp(fraction,0.f,1.f);
        for(unsigned k=0;k<carriers;++k)if(mask[k]){
            bool valid=std::isfinite(previous_filtered[k])&&std::isfinite(current_filtered[k]);
            ring[k][head]=valid?previous_filtered[k]+fraction*(current_filtered[k]-previous_filtered[k]):NAN;
            valid_run[k]=valid?std::min<unsigned>(valid_run[k]+1,config.window):0;
        }
        seq_ring[head]=m.tx_seq;head=(head+1)%config.window;
        filled=std::min<unsigned>(filled+1,config.window);++since_hop;next_grid_us+=period;keep=true;
    }
    std::copy_n(current_filtered,carriers,previous_filtered);previous_filter_us=m.rx_us;
    if(!keep)return;
    if(filled<config.window){measurement_valid=false;state="INVALID";reason="WINDOW_WARMUP";return;}
    if(since_hop<config.hop)return;
    since_hop=0;seq1=m.tx_seq;seq0=seq_ring[head];usable=0;
    for(unsigned k=0;k<carriers;++k){window_valid[k]=mask[k]&&valid_run[k]>=config.window;if(window_valid[k])++usable;}
    if(usable<required_carriers){measurement_valid=false;fsm.clear();score=0;votes=top_count=0;state="INVALID";reason="INSUFFICIENT_CARRIER_COVERAGE";return;}
    windowFeatures();emit=measurement_valid=true;
    unsigned order[MAX_CARRIERS],n=0;votes=0;
    for(unsigned k=0;k<carriers;++k)if(window_valid[k]){
        // Robust log-amplitude spread expressed as relative amplitude percent.
        // No learned activity mean or activity variance is subtracted/divided.
        features[k].z=100*std::expm1(std::min(10.f,1.4826f*features[k].mad*std::log(10.f)/20));
        if(features[k].z>=config.enter_score){++votes;}
        order[n++]=k;
    }
    std::sort(order,order+n,[&](unsigned a,unsigned b){return features[a].z>features[b].z;});
    top_count=std::min<unsigned>(config.top_k,n);
    for(unsigned i=0;i<top_count;++i){top[i]=order[i];temp[i]=features[order[i]].z;}
    score=median(temp,top_count);fsm.update(score,config);
    state=calibrated?(fsm.active?"ACTIVE":"CLEAR"):(calibrating?"CALIBRATING":"PROVISIONAL");
    reason=calibrated?"OK":(calibrating?"AMPLITUDE_REFERENCE":"AMPLITUDE_REFERENCE_REQUIRED");
}
void RatDetector::windowFeatures(){
    float temp[MAX_WINDOW];
    for(unsigned k=0;k<carriers;++k)if(window_valid[k]){
        float mean=0,var=0,diff=0;
        for(unsigned i=0;i<config.window;++i){float v=ring[k][(head+i)%config.window];temp[i]=v;mean+=v;if(i)diff=std::max(diff,std::fabs(v-ring[k][(head+i-1)%config.window]));}
        mean/=config.window;
        for(unsigned i=0;i<config.window;++i)var+=(temp[i]-mean)*(temp[i]-mean);
        std::sort(temp,temp+config.window);
        float center=(temp[(config.window-1)/2]+temp[config.window/2])/2;
        auto& f=features[k];f.iqr=temp[3*config.window/4]-temp[config.window/4];f.sd=std::sqrt(var/config.window);f.derivative=diff;
        f.mad=deviation(temp,config.window,center);
    }
}
