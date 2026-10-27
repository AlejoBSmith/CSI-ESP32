#pragma once
#include <algorithm>
#include <cstdint>
struct ArrivalTiming {
    uint64_t previous=0;uint32_t values[256]{};unsigned count=0,head=0;
    uint64_t intervals=0,gaps=0;uint32_t maximum=0;
    void observe(uint64_t us){
        if(previous&&us>previous){uint32_t dt=uint32_t(std::min<uint64_t>(us-previous,UINT32_MAX));
            values[head]=dt;head=(head+1)%256;count=std::min(256u,count+1);
            ++intervals;if(dt>50000)++gaps;maximum=std::max(maximum,dt);
        }previous=us;
    }
    uint32_t percentile(unsigned percent) const {
        if(!count){return 0;}
        uint32_t copy[256];std::copy_n(values,count,copy);std::sort(copy,copy+count);
        return copy[(count-1)*std::min(100u,percent)/100];
    }
};
