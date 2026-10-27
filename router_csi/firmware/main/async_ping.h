#pragma once
#include <cstdint>
namespace AsyncPing {
struct Stats {
    uint32_t sent=0,replies=0,timeouts=0,send_errors=0,unmatched=0,invalid=0,skipped=0,pending=0,max_pending=0;
    uint32_t backpressure=0,error_heap_free=0,error_heap_largest=0;
    uint32_t last_rtt_us=0,rtt_p95_us=0,send_p50_us=0,send_p95_us=0,send_max_us=0;
    int error=0;bool running=false;
};
bool start(uint32_t ipv4,unsigned hz,unsigned timeout_ms);
bool stop();bool running();uint32_t replies();Stats stats();
}
