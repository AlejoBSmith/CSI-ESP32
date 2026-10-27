#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Own implementation. Sequence Control identifies MPDUs, not ICMP requests.
struct WifiFrameKey {
    uint16_t control=0;
    uint8_t tid=16;
    bool retry=false;
};
inline bool dataFrameKey(const uint8_t* h,unsigned length,WifiFrameKey& key){
    if(!h||length<24||(h[0]&0x0c)!=0x08||(h[0]&0x40))return false;
    unsigned header=24+((h[1]&3)==3?6:0);
    if(h[0]&0x80){if(length<header+2)return false;key.tid=h[header]&15;}
    else key.tid=16;
    key.control=uint16_t(h[22])|(uint16_t(h[23])<<8);key.retry=(h[1]&8)!=0;
    return true;
}
class WifiDuplicates {
    struct Entry {uint64_t time=0;uint16_t seq=0;uint8_t tid=0;bool valid=false;};
    Entry entries[128]{};unsigned next=0;uint32_t session=0;
public:
    void reset(uint32_t s){for(auto& e:entries)e.valid=false;next=0;session=s;}
    bool contains(const WifiFrameKey& key,uint64_t now,uint32_t s){
        if(s!=session)reset(s);
        for(const auto& e:entries)if(e.valid&&e.seq==key.control&&e.tid==key.tid&&now>=e.time&&now-e.time<2000000)return true;
        return false;
    }
    void remember(const WifiFrameKey& key,uint64_t now){entries[next]={now,key.control,key.tid,true};next=(next+1)%128;}
};
// Expand the hardware RX timestamp through its 32-bit wrap. Reject late packets.
class RadioClock {
    bool ready=false;uint32_t previous=0;uint64_t expanded=0,last_wall=0;
public:
    bool observe(uint32_t radio,uint64_t wall,uint64_t& result){
        if(!ready||wall-last_wall>2000000){ready=true;previous=radio;expanded=wall;last_wall=wall;result=expanded;return true;}
        int32_t advance=int32_t(radio-previous);
        if(advance<=0)return false;
        previous=radio;expanded+=uint32_t(advance);last_wall=wall;result=expanded;return true;
    }
};
