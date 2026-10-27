#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace ProbeProtocol {
inline uint16_t read16(const uint8_t* p){return uint16_t(p[0])<<8|p[1];}
inline uint32_t read32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
inline void write16(uint8_t* p,uint16_t v){p[0]=uint8_t(v>>8);p[1]=uint8_t(v);}
inline void write32(uint8_t* p,uint32_t v){p[0]=uint8_t(v>>24);p[1]=uint8_t(v>>16);p[2]=uint8_t(v>>8);p[3]=uint8_t(v);}
inline uint16_t checksum(const uint8_t* p,size_t n){uint32_t s=0;while(n>1){s+=read16(p);p+=2;n-=2;}if(n)s+=uint16_t(*p)<<8;while(s>>16)s=(s&65535)+(s>>16);return uint16_t(~s);}
inline void echo(uint8_t (&p)[16],uint16_t id,uint32_t seq,uint32_t nonce){
    std::memset(p,0,sizeof(p));p[0]=8;write16(p+4,id);write16(p+6,uint16_t(seq));write32(p+8,nonce);write32(p+12,seq);write16(p+2,checksum(p,sizeof(p)));
}
inline bool reply(const uint8_t* ip,size_t n,uint16_t id,uint32_t nonce,uint32_t& seq){
    if(n<36||ip[0]>>4!=4||ip[9]!=1)return false;
    size_t ihl=(ip[0]&15)*4,total=read16(ip+2);
    if(ihl<20||total>n||total<ihl+16||(read16(ip+6)&0x3fff))return false;
    const auto* p=ip+ihl;
    if(p[0]!=0||p[1]!=0||read16(p+4)!=id||read32(p+8)!=nonce||checksum(p,total-ihl)!=0)return false;
    seq=read32(p+12);return read16(p+6)==uint16_t(seq);
}
struct Cadence {
    uint64_t next=0,last=0;uint32_t period=10000,skipped=0;
    void reset(unsigned hz,uint64_t now){period=1000000/hz;next=now;last=0;skipped=0;}
    bool due(uint64_t now){
        if(now<next)return false;
        if(last&&now-last<period/2){next=last+period;return false;}
        uint64_t late=now-next;skipped+=uint32_t(late/period);last=now;
        next=late>=period/2?now+period:next+period;return true;
    }
};
struct Ledger {
    static constexpr unsigned CAPACITY=64;
    struct Request {uint32_t seq=0;uint64_t sent=0;bool used=false;};Request requests[CAPACITY]{};unsigned active=0;
    void clear(){for(auto& r:requests)r.used=false;active=0;}
    bool add(uint32_t seq,uint64_t now){auto& r=requests[seq%CAPACITY];if(r.used)return false;r={seq,now,true};++active;return true;}
    bool finish(uint32_t seq,uint64_t now,uint32_t& rtt){auto& r=requests[seq%CAPACITY];if(!r.used||r.seq!=seq||now<r.sent)return false;rtt=uint32_t(now-r.sent);r.used=false;--active;return true;}
    void cancel(uint32_t seq){auto& r=requests[seq%CAPACITY];if(r.used&&r.seq==seq){r.used=false;--active;}}
    unsigned expire(uint64_t now,uint64_t timeout){unsigned n=0;for(auto& r:requests)if(r.used&&now-r.sent>=timeout){r.used=false;--active;++n;}return n;}
};
}
