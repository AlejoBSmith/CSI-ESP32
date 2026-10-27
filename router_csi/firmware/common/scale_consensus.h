#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

// Temporal windows overlap. This is scale-space agreement, never a probability.
struct ScaleConsensus {
    static constexpr unsigned MAX_SCALES=8,MAX_CARRIERS=56;
    static float fuse(const float* const* z,const bool* valid,unsigned count,unsigned carriers,unsigned quorum,unsigned top,
                      unsigned& winning_first,unsigned& supporting,bool& measurement_valid){
        float best=0,values[MAX_CARRIERS];winning_first=0;supporting=0;measurement_valid=false;
        if(!quorum||quorum>count||!carriers)return 0;
        for(unsigned first=0;first+quorum<=count;++first){
            bool usable=true;for(unsigned j=first;j<first+quorum;++j)usable=usable&&valid[j];
            if(!usable)continue;
            measurement_valid=true;
            for(unsigned k=0;k<carriers;++k){float low=z[first][k],high=low;
                for(unsigned j=first+1;j<first+quorum;++j){low=std::min(low,z[j][k]);high=std::max(high,z[j][k]);}
                values[k]=std::min(high,2*low); // Preserve the stronger response only to the extent its neighbors corroborate it.
            }
            unsigned n=std::min(top,carriers);if(!n)continue;
            if(n<carriers)std::nth_element(values,values+n,values+carriers,std::greater<float>());
            std::nth_element(values,values+n/2,values+n);float candidate=values[n/2];
            if(!(n&1))candidate=(candidate+*std::max_element(values,values+n/2))*.5f;
            if(candidate>best){best=candidate;winning_first=first;supporting=quorum;}
        }
        return best;
    }
};
// Welford covariance on valid lag pairs. Float32 uses the S3 FPU without
// cancellation from subtracting large nearly equal raw moments.
struct StableLagCorrelation {
    unsigned count=0;float mx=0,my=0,vx=0,vy=0,cov=0;
    void add(float x,float y,float inverse=0){
        ++count;if(inverse<=0)inverse=1.f/count;
        float dx=x-mx,dy=y-my;mx+=dx*inverse;my+=dy*inverse;
        vx+=dx*(x-mx);vy+=dy*(y-my);cov+=dx*(y-my);
    }
    float correlation()const{
        float den=std::sqrt(std::max(0.f,vx)*std::max(0.f,vy));
        return den>1e-12f?std::clamp(cov/den,-1.f,1.f):0;
    }
};
struct CausalConfirmation {
    float history[7]{};unsigned head=0;uint64_t previous_us=0;
    void clear(){std::fill_n(history,7,0.f);head=0;previous_us=0;}
    float add(float value,unsigned required,uint64_t now,uint64_t expected,float threshold,float& progress){
        required=std::clamp(required,1u,4u);unsigned length=2*required-1;
        if(previous_us&&(now<=previous_us||now-previous_us>expected*18/10))clear();
        previous_us=now;history[head]=value;head=(head+1)%length;
        float scratch[7];std::copy_n(history,length,scratch);unsigned votes=0;for(unsigned i=0;i<length;++i)votes+=scratch[i]>=threshold;
        progress=std::min(1.f,float(votes)/required);
        std::nth_element(scratch,scratch+length/2,scratch+length);return scratch[length/2];
    }
};
