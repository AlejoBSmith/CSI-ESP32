#include "async_ping.h"
#include "experiment_config.h"
#include "esp_heap_caps.h"
#include "probe_protocol.h"
#include "router_timing.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
namespace AsyncPing {
static std::atomic<bool> run{false},done{true};
static std::atomic<uint32_t> sent{0},received{0},missed{0},send_errors{0},unmatched{0},invalid{0},skipped{0},pending{0},max_pending{0},last_rtt{0};
static std::atomic<int> last_error{0};
static std::atomic<uint32_t> backpressure{0},error_heap_free{0},error_heap_largest{0};
static portMUX_TYPE guard=portMUX_INITIALIZER_UNLOCKED;
static ArrivalTiming sends;static uint32_t rtts[256]{};static unsigned rtt_count=0,rtt_head=0;
static uint32_t destination;static unsigned frequency,timeout_ms;static int socket_fd=-1;
static void loop(void*){
    ProbeProtocol::Ledger ledger;ProbeProtocol::Cadence cadence;uint64_t now=esp_timer_get_time();cadence.reset(frequency,now);
    uint16_t id=uint16_t(esp_random());uint32_t nonce=esp_random(),sequence=0,skips=0;
    sockaddr_in to{};to.sin_family=AF_INET;to.sin_addr.s_addr=destination;
    while(run){
        now=esp_timer_get_time();missed+=ledger.expire(now,uint64_t(timeout_ms)*1000);
        if(cadence.due(now)){
            skipped+=cadence.skipped-skips;skips=cadence.skipped;uint32_t seq=++sequence;
            if(ledger.active>=RouterDefaults::max_pending_ping){++backpressure;}
            else if(ledger.add(seq,now)){
                uint8_t packet[16];ProbeProtocol::echo(packet,id,seq,nonce);
                int n=sendto(socket_fd,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&to),sizeof(to));
                if(n==sizeof(packet)){++sent;portENTER_CRITICAL(&guard);sends.observe(now);portEXIT_CRITICAL(&guard);}
                else{
                    ledger.cancel(seq);++send_errors;last_error=errno;
                    error_heap_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
                    error_heap_largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
                    if(errno==ENOMEM||errno==ENOBUFS||errno==EAGAIN||errno==EWOULDBLOCK){++backpressure;cadence.next=esp_timer_get_time()+RouterDefaults::ping_backoff_us;}
                }
            }else ++send_errors;
        }
        for(unsigned i=0;i<32;++i){
            uint8_t packet[256];sockaddr_in from{};socklen_t size=sizeof(from);
            int n=recvfrom(socket_fd,packet,sizeof(packet),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&from),&size);
            if(n<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)last_error=errno;break;}
            if(from.sin_addr.s_addr!=destination)continue;
            uint32_t seq=0,rtt=0;
            if(!ProbeProtocol::reply(packet,size_t(n),id,nonce,seq)){++invalid;continue;}
            now=esp_timer_get_time();
            if(ledger.finish(seq,now,rtt)){
                ++received;last_rtt=rtt;portENTER_CRITICAL(&guard);rtts[rtt_head]=rtt;rtt_head=(rtt_head+1)%256;rtt_count=std::min(256u,rtt_count+1);portEXIT_CRITICAL(&guard);
            }else ++unmatched;
        }
        pending=ledger.active;max_pending=std::max<uint32_t>(max_pending.load(),ledger.active);
        now=esp_timer_get_time();uint64_t delay=cadence.next>now?std::min<uint64_t>(2000,cadence.next-now):1;
        timeval wait{};wait.tv_usec=long(delay);fd_set readset;FD_ZERO(&readset);FD_SET(socket_fd,&readset);
        int result=select(socket_fd+1,&readset,nullptr,nullptr,&wait);if(result<0&&errno!=EINTR)last_error=errno;
    }
    close(socket_fd);socket_fd=-1;pending=0;done=true;vTaskDelete(nullptr);
}
bool start(uint32_t ipv4,unsigned hz,unsigned timeout){
    if(!done||hz<10||hz>500)return false;
    destination=ipv4;frequency=hz;timeout_ms=timeout;
    socket_fd=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);if(socket_fd<0){last_error=errno;return false;}
    int flags=fcntl(socket_fd,F_GETFL,0);
    if(flags<0||fcntl(socket_fd,F_SETFL,flags|O_NONBLOCK)<0){last_error=errno;close(socket_fd);socket_fd=-1;return false;}
    int tos=0;setsockopt(socket_fd,IPPROTO_IP,IP_TOS,&tos,sizeof(tos));
    last_error=0;portENTER_CRITICAL(&guard);sends=ArrivalTiming{};rtt_count=rtt_head=0;portEXIT_CRITICAL(&guard);
    run=true;done=false;
    if(xTaskCreatePinnedToCore(loop,"icmp_async",6144,nullptr,5,nullptr,1)!=pdPASS){run=false;done=true;close(socket_fd);socket_fd=-1;last_error=ENOMEM;return false;}
    return true;
}
bool stop(){run=false;for(unsigned i=0;i<200&&!done;++i)vTaskDelay(pdMS_TO_TICKS(1));return done;}
bool running(){return run&&!done;}
uint32_t replies(){return received.load();}
Stats stats(){
    Stats s;s.sent=sent;s.replies=received;s.timeouts=missed;s.send_errors=send_errors;s.unmatched=unmatched;s.invalid=invalid;s.skipped=skipped;s.pending=pending;s.max_pending=max_pending;s.last_rtt_us=last_rtt;s.error=last_error;s.running=running();s.backpressure=backpressure;s.error_heap_free=error_heap_free;s.error_heap_largest=error_heap_largest;
    ArrivalTiming copy;uint32_t values[256];unsigned n;
    portENTER_CRITICAL(&guard);copy=sends;n=rtt_count;std::copy_n(rtts,n,values);portEXIT_CRITICAL(&guard);
    s.send_p50_us=copy.percentile(50);s.send_p95_us=copy.percentile(95);s.send_max_us=copy.maximum;
    if(n){std::sort(values,values+n);s.rtt_p95_us=values[(n-1)*95/100];}return s;
}
}
