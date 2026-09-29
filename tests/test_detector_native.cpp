#include "detector.h"
#include "activity_led.h"
#include <iostream>
#include <memory>
#include <vector>
#include <stdexcept>
static void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
int main(){try{
    Config old;old.version=2;old.window=800;old.hop=400;old.channel=6;
    require(upgradeConfig(old,sizeof(Config))&&old.version==3&&old.hop==62&&old.channel==6,"NVS migration");
    PeakScale peak;peak.observe(10);require(peak.peak==10,"LED initial peak");peak.observe(5);
    require(activityBrightness(5,"ACTIVE",false,false,true,500000,peak.peak)==511,"LED falling response");
    peak.observe(20);require(activityBrightness(10,"ACTIVE",false,false,true,500000,peak.peak)==511,"LED remap at new maximum");
    peak.observe(NAN);require(peak.peak==20,"LED ignores invalid");
    require(activityBrightness(20,"INVALID",false,false,true,500000,peak.peak)==0,"No stale LED activity");
    auto d=std::make_unique<RatDetector>();Frame f{};
    f.meta.flags=2;f.meta.gain=1;f.meta.csi_len=128;f.meta.channel=11;f.meta.sig_mode=1;f.meta.tx_boot=1;
    uint64_t clock=1000000;unsigned seq=0;bool cal=false,gap=false,drop=false,boot=false;
    unsigned provisional=0,after_active=0,returned=0,intervals=0,invalid=0;bool short_gap=false;uint64_t previous=0;
    for(float t=0;t<95;){
        unsigned dt=t>=35&&t<40?3333:t>=45&&t<48?10000:2000;
        t+=dt/1e6f;clock+=dt;
        if(t>=10&&!cal){d->startCalibration();cal=true;}
        if(t>=55&&!gap){clock+=2000000;gap=true;d->idle(clock);require(d->calibrated,"NO_CSI erased amplitude");}
        if(t>=60&&!drop){++f.meta.queue_drops;drop=true;}
        if(t>=65&&!boot){++f.meta.tx_boot;seq=0;boot=true;}
        if(t>=33&&!short_gap){clock+=28000;short_gap=true;}
        bool movement=t<85;
        for(unsigned k=0;k<64;++k){f.iq[2*k]=0;f.iq[2*k+1]=int8_t(30+(movement?8*std::sin(12.56637f*t):0));}
        if(t>70&&t<75)std::fill_n(f.iq,128,0);
        f.meta.tx_seq=++seq;f.meta.rx_us=clock;d->add(f);
        if(t>33&&t<33.1)require(d->measurement_valid,"Short radio jitter discarded the entire window");
        require(std::string(d->reason)!="NEEDS_CALIBRATION","Permanent calibration lock");
        if(t>3&&t<9&&d->emit){require(d->measurement_valid&&!d->calibrated&&d->score>4,"Provisional signal missing");++provisional;}
        if(t>27&&t<34&&d->emit){require(d->calibrated&&d->score>4,"Activity learned away during amplitude calibration");++after_active;}
        if(t>28&&t<33&&d->emit){if(previous){require(clock-previous>=246000&&clock-previous<=252000,"Hop not about 250 ms");++intervals;}previous=clock;}
        if(t>46.5&&t<47.5){require(!d->measurement_valid,"Low RF rate should remain invalid");++invalid;}
        if(t>27)require(d->calibrated&&d->reference_id==1,"Transient fault erased reference");
        if(((t>41&&t<44)||(t>50&&t<54)||(t>57&&t<59)||(t>62&&t<64)||(t>67&&t<69)||(t>78&&t<81))&&d->emit){require(d->measurement_valid,"No automatic recovery");++returned;}
        if(t>90&&d->emit)require(std::string(d->state)=="CLEAR"&&d->score<.1,"Constant input did not return CLEAR");
    }
    require(provisional>10&&after_active>10&&returned>30&&intervals>10&&invalid>0,"Insufficient test coverage");
    require(d->recovery_count>3,"Fault recovery diagnostics missing");
    std::cout<<"PASS: provisional output, persistent activity, 248 ms hops, recovery after rate changes/gaps/queue loss/TX restart, LED peak scaling\n";
    return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
