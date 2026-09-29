#pragma once
#include "radio_config.h"
#include <cmath>
#include <algorithm>
#include <cstring>

struct ActivityFSM {
    bool active=false;
    unsigned enter=0, exit=0;
    void clear() { active=false; enter=exit=0; }
    void update(float score, const Config& c) {
        if (!active) { enter=score>=c.enter_score?enter+1:0; if(enter>=c.enter_hops){active=true;enter=0;} }
        else { exit=score<c.exit_score?exit+1:0; if(exit>=c.exit_hops){active=false;exit=0;} }
    }
};
struct Biquad {
    float b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    void configure(float fs,float fc,float q) {
        float w=2*3.14159265359f*fc/fs,cs=std::cos(w),alpha=std::sin(w)/(2*q),a0=1+alpha;
        b0=(1-cs)/(2*a0);b1=(1-cs)/a0;b2=b0;a1=-2*cs/a0;a2=(1-alpha)/a0;z1=z2=0;
    }
    float add(float x) {float y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
    void seed(float x) {z1=(1-b0)*x;z2=(b2-a2)*x;}
};
struct CarrierFeature { float mad=0,iqr=0,sd=0,derivative=0,z=0; };
class RatDetector {
public:
    static constexpr unsigned CAL_SAMPLES=Experiment::amplitude_reservoir, MAX_WINDOW=Experiment::max_window;
    Config config;
    ActivityFSM fsm;
    const char* state="INVALID";
    const char* reason="NO_CSI";
    float effective_fs=0, loss_fraction=0, score=0, filter_fs=0;
    unsigned selected=0,votes=0,top[32]{}, top_count=0,reference_id=0;
    uint32_t seq0=0,seq1=0;
    bool emit=false, calibrated=false, calibrating=false, measurement_valid=false;
    const char* last_recovery="NONE";
    uint32_t recovery_count=0;
    float calibration_progress=0;
    CarrierFeature features[MAX_CARRIERS]{};
    bool mask[MAX_CARRIERS]{};
    bool window_valid[MAX_CARRIERS]{};
    unsigned usable=0,required_carriers=0,missing_now=0;
    uint32_t missing_values=0;
    float amp_median[MAX_CARRIERS]{};
    void reset(const char* why="AMPLITUDE_REFERENCE_REQUIRED");
    void startCalibration();
    void add(const Frame& f);
    void idle(uint64_t now);
private:
    float reservoir[MAX_CARRIERS][CAL_SAMPLES]{};
    float ring[MAX_CARRIERS][MAX_WINDOW]{};
    float impulses[MAX_CARRIERS][15]{};
    float previous_filtered[MAX_CARRIERS]{},current_filtered[MAX_CARRIERS]{};
    uint32_t seq_ring[MAX_WINDOW]{};
    uint64_t next_grid_us=0,previous_filter_us=0;
    Biquad low1[MAX_CARRIERS],low2[MAX_CARRIERS];
    unsigned finite_count[MAX_CARRIERS]{},cal_frames=0,cal_kept=0;
    unsigned carriers=0,head=0,filled=0,since_hop=0,impulse_head=0,impulse_count=0;
    uint64_t last_us=0,rate_start=0,cal_start=0,cal_valid_us=0;
    unsigned rate_frames=0,rate_missing=0;
    uint32_t last_seq=0,tx_boot=0,last_drops=0;
    uint32_t rng=123456789;
    uint64_t signature=0;
    bool amplitude_ready=false;
    unsigned carrier_head[MAX_CARRIERS]{},carrier_count[MAX_CARRIERS]{};
    unsigned valid_run[MAX_CARRIERS]{};
    bool filter_ready[MAX_CARRIERS]{};
    void clearContinuity();
    static float median(float* a,unsigned n);
    static float deviation(float* a,unsigned n,float center);
    void establishAmplitude();
    void provisionalAmplitude(const Frame& f);
    void windowFeatures();
    void fail(const char* why);
};
