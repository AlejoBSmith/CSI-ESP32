#pragma once
#include "radio_config.h"
#include "scale_consensus.h"
#include <cmath>
#include <algorithm>
#include <cstring>

struct ActivityFSM {
    bool active=false;unsigned enter=0,exit=0;
    void clear(){active=false;enter=exit=0;}
    void update(float score,const Config& c){
        if(!active){enter=score>=c.enter_score?enter+1:0;if(enter>=c.enter_hops){active=true;enter=0;}}
        else{exit=score<c.exit_score?exit+1:0;if(exit>=c.exit_hops){active=false;exit=0;}}
    }
};
struct Biquad {
    float b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    void configure(float fs,float fc,float q){float w=2*3.14159265359f*fc/fs,cs=std::cos(w),alpha=std::sin(w)/(2*q),a0=1+alpha;b0=(1-cs)/(2*a0);b1=(1-cs)/a0;b2=b0;a1=-2*cs/a0;a2=(1-alpha)/a0;z1=z2=0;}
    float add(float x){float y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
    void seed(float x){z1=(1-b0)*x;z2=(b2-a2)*x;}
};
struct CarrierFeature {float mad=0,iqr=0,sd=0,derivative=0,z=0;};
// Scores share units, but windows overlap: fusion is a rule, not a probability.
struct TemporalFusion {
    static float combine(float fast,bool fv,float slow,bool sv,float threshold,float alone_weight,float& applied_weight){
        float support=sv?std::clamp(slow/threshold,0.f,1.f):0;
        applied_weight=alone_weight+(1-alone_weight)*support;
        return std::max(sv?slow:0.f,fv?fast*applied_weight:0.f);
    }
};
struct ScaleResult {
    bool valid=false;unsigned count=0,pairs=0,votes=0;float coverage=0,score=0,coherence=0;
    float carrier_scores[56]{},carrier_iqr[56]{};
};
class RatDetector {
public:
    static constexpr unsigned CAL_SAMPLES=Experiment::amplitude_reservoir,MAX_WINDOW=Experiment::max_window,BAND=56,FIXED_BAND=12;
    static constexpr unsigned fixed_bins[FIXED_BAND]={4,9,14,19,24,26,38,40,45,50,55,60};
    Config config;ActivityFSM fsm;
    const char* state="INVALID";const char* reason="NO_CSI";
    float effective_fs=0,loss_fraction=0,score=0,filter_fs=Experiment::output_hz;
    float turbulence=0,turbulence_relative_iqr=0,temporal_autocorr=0,coherence_floor=0,shape_displacement=0,window_coverage=0;
    static constexpr unsigned MAX_SCALES=ScaleConsensus::MAX_SCALES;
    ScaleResult scales[MAX_SCALES]{};unsigned scale_windows[MAX_SCALES]{},bank_total=0,bank_valid=0,bank_support=0;
    float bank_raw=0,bank_dominant_ms=0,confirmation_progress=0;bool bank_confirmed=false;
    ScaleResult fast,slow;float fast_weight_applied=0,legacy_score=0;bool legacy_valid=false;
    const char* fusion_source="NONE";
    unsigned admitted_count=0,excess_packets=0,shifted_packets=0,missing_slots=0,window_slots_valid=0,lag_pairs=0;
    uint64_t result_us=0;
    unsigned selected=0,votes=0,top[32]{},top_count=0,reference_id=0;
    uint32_t seq0=0,seq1=0;
    bool emit=false,calibrated=false,calibrating=false,measurement_valid=false;
    const char* last_recovery="NONE";uint32_t recovery_count=0;
    float calibration_progress=0;
    CarrierFeature features[MAX_CARRIERS]{};bool mask[MAX_CARRIERS]{},window_valid[MAX_CARRIERS]{};
    unsigned usable=0,required_carriers=0,missing_now=0;uint32_t missing_values=0;
    float amp_median[MAX_CARRIERS]{};
    void reset(const char* why="AMPLITUDE_REFERENCE_REQUIRED");void startCalibration();void add(const Frame& f);void idle(uint64_t now);
private:
    CausalConfirmation confirmation;unsigned winning_scale=0;
    unsigned active_bins[BAND]{};bool layout_ht=true;
    float reservoir[BAND][CAL_SAMPLES]{};unsigned cal_frames=0,cal_kept=0;uint32_t rng=123456789;
    uint64_t cal_valid_us=0;
    float normalized[BAND][MAX_WINDOW]{},cv[MAX_WINDOW]{};uint32_t sequences[MAX_WINDOW]{};
    unsigned head=0,filled=0,since_hop=0;
    uint64_t last_us=0,rate_start=0,signature=0;unsigned rate_frames=0;
    uint32_t tx_boot=0,last_seq=0,last_drops=0;
    bool pending=false,pending_valid=false;uint64_t pending_slot=0,pending_us=0,grid_origin=0,last_committed_us=0;
    float pending_distance=0,pending_cv=0,pending_shape[BAND]{};uint32_t pending_seq=0;
    float impulses[15]{};unsigned impulse_count=0,impulse_head=0;Biquad lowpass;bool lowpass_ready=false;
    unsigned capacity()const{return std::max<unsigned>(config.window,100u);}
    bool physical(unsigned bin)const{return layout_ht?((bin>=1&&bin<=28)||(bin>=36&&bin<=63)):((bin>=2&&bin<=26)||(bin>=38&&bin<=63));}
    unsigned index(unsigned back)const{return (head+capacity()-back)%capacity();}
    void clearContinuity();void fail(const char* why);void selectBand();
    void append(bool valid,const float* shape,float value,uint32_t seq);
    void evaluate();void commit();void buildBank();void scaleBank();ScaleResult scale(unsigned window);float legacy(unsigned window,bool& valid,bool diagnostics);
    static float unsortedQuantile(float* a,unsigned n,float q);static float median(float* a,unsigned n);static float quantile(const float* sorted,unsigned n,float q);
};
