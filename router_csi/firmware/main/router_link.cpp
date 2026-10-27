#include "router_link.h"
#include "async_ping.h"
#include "experiment_config.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#include "lwip/ip4.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
namespace RouterLink {
struct Settings {
    uint32_t version=1;
    char ssid[33]{},password[64]{},target[16]{};
    Settings(){snprintf(ssid,sizeof(ssid),"%s",RouterDefaults::ssid);snprintf(target,sizeof(target),"%s",RouterDefaults::target);}
    uint32_t ping_hz=RouterDefaults::ping_hz;
};
static Settings settings;
static bool legacy_only=false;static bool asynchronous=RouterDefaults::asynchronous_ping,traffic_changed=false;static int64_t next_probe_attempt=0;
static std::atomic<bool> ht_acquisition{true};
static std::atomic<bool> online{false};
static std::atomic<uint32_t> generation{0},last_disconnect{0},ping_replies{0},ping_timeouts{0},rtt_ms{0};
static std::atomic<uint32_t> ping_completed_sent{0};
static std::atomic<bool> associated{false};
static uint64_t rate_started=0;static uint32_t rate_replies=0;static float reply_hz=0;
static portMUX_TYPE guard=portMUX_INITIALIZER_UNLOCKED;
static uint8_t bssid[6]{};static char local_ip[16]="0.0.0.0";
static int64_t next_connect=0;static bool reconfigure=false;static std::atomic<bool> connecting{false};static int64_t connect_started=0;static esp_err_t connect_error=ESP_OK;
static esp_ping_handle_t ping=nullptr;
static bool arp_keepalive=RouterDefaults::arp_keepalive;
static std::atomic<bool> neighbor_busy{false},neighbor_known{false};
static std::atomic<uint32_t> neighbor_unknown{0},neighbor_checks{0},arp_requests{0},arp_errors{0};
static uint32_t neighbor_target=0;static bool neighbor_refresh=false;
static uint64_t next_neighbor_check=0,next_neighbor_refresh=0;
// All ARP table access is confined to the lwIP TCP/IP thread.
static void inspect_neighbor(void*){
    ip4_addr_t target{};target.addr=neighbor_target;auto* netif=ip4_route(&target);
    bool known=false;
    if(netif&&netif_is_up(netif)){
        ip4_addr_t hop=ip4_addr_netcmp(&target,netif_ip4_addr(netif),netif_ip4_netmask(netif))?target:*netif_ip4_gw(netif);
        if(!ip4_addr_isany_val(hop)){
            eth_addr* mac=nullptr;const ip4_addr_t* found=nullptr;
            known=etharp_find_addr(netif,&hop,&mac,&found)>=0;
            if(neighbor_refresh){if(etharp_request(netif,&hop)==ERR_OK)++arp_requests;else ++arp_errors;}
        }
    }
    neighbor_known=known;++neighbor_checks;if(!known)++neighbor_unknown;neighbor_busy=false;
}
static void maintain_neighbor(uint64_t now){
    if(now<next_neighbor_check||neighbor_busy.exchange(true))return;
    next_neighbor_check=now+1000000;neighbor_target=inet_addr(settings.target);
    neighbor_refresh=arp_keepalive&&now>=next_neighbor_refresh;
    if(neighbor_refresh)next_neighbor_refresh=now+RouterDefaults::arp_refresh_us;
    if(tcpip_try_callback(inspect_neighbor,nullptr)!=ERR_OK){neighbor_busy=false;++arp_errors;}
}
static bool configured(){return settings.ssid[0]&&std::strlen(settings.password)>=8;}
static bool valid(const Settings& s){
    return s.version==1&&s.ssid[32]==0&&s.password[63]==0&&s.target[15]==0&&
        std::strlen(s.ssid)>0&&std::strlen(s.password)>=8&&s.ping_hz>=10&&s.ping_hz<=500&&
        inet_addr(s.target)!=IPADDR_NONE&&inet_addr(s.target)!=0;
}
static void success(esp_ping_handle_t h,void*){
    uint32_t ms=0;
    esp_ping_get_profile(h,ESP_PING_PROF_TIMEGAP,&ms,sizeof(ms));
    rtt_ms=ms;++ping_completed_sent;++ping_replies;
}
static void timeout(esp_ping_handle_t h,void*){
    (void)h;++ping_completed_sent;++ping_timeouts;
}
static void stopPing(){
    if(ping){esp_ping_stop(ping);esp_ping_delete_session(ping);ping=nullptr;}
    AsyncPing::stop();

}
static void event(void*,esp_event_base_t base,int32_t id,void* data){
    if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_CONNECTED){associated=true;}
    if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){
        online=false;associated=false;connecting=false;last_disconnect=static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
    }
    if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){
        wifi_ap_record_t ap{};
        if(esp_wifi_sta_get_ap_info(&ap)!=ESP_OK)return;
        auto* got=static_cast<ip_event_got_ip_t*>(data);
        portENTER_CRITICAL(&guard);
        memcpy(bssid,ap.bssid,6);snprintf(local_ip,sizeof(local_ip),IPSTR,IP2STR(&got->ip_info.ip));
        ht_acquisition=!legacy_only&&ap.phy_11n;++generation;online=true;
        portEXIT_CRITICAL(&guard);
    }
}
void init(){
    nvs_handle_t h;
    if(nvs_open("rat-router-net",NVS_READONLY,&h)==ESP_OK){
        Settings saved;size_t size=sizeof(saved);
        if(nvs_get_blob(h,"settings",&saved,&size)==ESP_OK&&size==sizeof(saved)&&valid(saved))settings=saved;
        uint8_t legacy=0;nvs_get_u8(h,"legacy_only",&legacy);legacy_only=legacy!=0;
        uint8_t mode=asynchronous;nvs_get_u8(h,"probe_mode",&mode);asynchronous=mode!=0;
        uint8_t arp=arp_keepalive;nvs_get_u8(h,"arp_keepalive",&arp);arp_keepalive=arp!=0;
        uint32_t hz=settings.ping_hz;if(nvs_get_u32(h,"traffic_hz",&hz)==ESP_OK&&hz>=10&&hz<=500)settings.ping_hz=hz;
        nvs_close(h);
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,event,nullptr));
    reconfigure=configured();
}
bool configure(const cJSON* j,const char*& error){
    Settings next=settings;bool next_legacy=legacy_only;
    if(auto* item=cJSON_GetObjectItemCaseSensitive(j,"legacy_only")){
        if(!cJSON_IsBool(item)){error="legacy_only must be boolean";return false;}
        next_legacy=cJSON_IsTrue(item);
    }
    auto copy=[&](const char* key,char* out,size_t cap){
        const auto* item=cJSON_GetObjectItemCaseSensitive(j,key);
        if(!item)return true;
        if(!cJSON_IsString(item)||!item->valuestring||std::strlen(item->valuestring)>=cap)return false;
        std::strcpy(out,item->valuestring);return true;
    };
    if(!copy("ssid",next.ssid,sizeof(next.ssid))||!copy("password",next.password,sizeof(next.password))||!copy("target",next.target,sizeof(next.target))){error="SSID, password or target length/type invalid";return false;}
    const auto* hz=cJSON_GetObjectItemCaseSensitive(j,"ping_hz");
    if(hz){if(!cJSON_IsNumber(hz)||hz->valuedouble<10||hz->valuedouble>500||std::floor(hz->valuedouble)!=hz->valuedouble){error="ping_hz must be an integer in 10..500";return false;}next.ping_hz=unsigned(hz->valuedouble);}
    if(!valid(next)){error="Use SSID, WPA2 password 8..63 bytes, IPv4 target and 10..500 pings/s";return false;}
    nvs_handle_t h;esp_err_t e=nvs_open("rat-router-net",NVS_READWRITE,&h);
    if(e==ESP_OK){e=nvs_set_blob(h,"settings",&next,sizeof(next));if(e==ESP_OK)e=nvs_set_u8(h,"legacy_only",next_legacy);if(e==ESP_OK)e=nvs_set_u32(h,"traffic_hz",next.ping_hz);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
    if(e!=ESP_OK){error="Cannot save device-local Wi-Fi configuration";return false;}
    settings=next;legacy_only=next_legacy;reconfigure=true;return true;
}
bool configureTraffic(const cJSON* j,const char*& error){
    bool mode=asynchronous,arp=arp_keepalive;unsigned hz=settings.ping_hz;bool persist=true;
    if(auto* p=cJSON_GetObjectItemCaseSensitive(j,"arp_keepalive")){if(!cJSON_IsBool(p)){error="arp_keepalive must be boolean";return false;}arp=cJSON_IsTrue(p);}
    if(auto* p=cJSON_GetObjectItemCaseSensitive(j,"mode")){
        if(!cJSON_IsString(p)||!p->valuestring||(strcmp(p->valuestring,"async")&&strcmp(p->valuestring,"sequential"))){error="mode must be async or sequential";return false;}
        mode=!strcmp(p->valuestring,"async");
    }
    if(auto* p=cJSON_GetObjectItemCaseSensitive(j,"ping_hz")){
        if(!cJSON_IsNumber(p)||p->valuedouble<10||p->valuedouble>500||std::floor(p->valuedouble)!=p->valuedouble){error="ping_hz must be 10..500";return false;}hz=unsigned(p->valuedouble);
    }
    if(auto* p=cJSON_GetObjectItemCaseSensitive(j,"persist")){if(!cJSON_IsBool(p)){error="persist must be boolean";return false;}persist=cJSON_IsTrue(p);}
    if(persist){
        nvs_handle_t h;esp_err_t e=nvs_open("rat-router-net",NVS_READWRITE,&h);
        if(e==ESP_OK){e=nvs_set_u8(h,"probe_mode",mode);if(e==ESP_OK)e=nvs_set_u32(h,"traffic_hz",hz);if(e==ESP_OK)e=nvs_set_u8(h,"arp_keepalive",arp);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
        if(e!=ESP_OK){error="Cannot persist traffic settings";return false;}
    }
    traffic_changed=traffic_changed||mode!=asynchronous||hz!=settings.ping_hz;asynchronous=mode;settings.ping_hz=hz;if(arp!=arp_keepalive)next_neighbor_refresh=0;arp_keepalive=arp;return true;
}
bool forget(){
    nvs_handle_t h;esp_err_t e=nvs_open("rat-router-net",NVS_READWRITE,&h);
    if(e==ESP_OK){e=nvs_erase_all(h);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
    if(e!=ESP_OK)return false;
    stopPing();online=false;associated=false;esp_wifi_disconnect();settings=Settings{};legacy_only=false;reconfigure=false;connecting=false;return true;
}
void tick(bool enabled){
    int64_t now=esp_timer_get_time();
    if(!rate_started)rate_started=now;
    if(now-rate_started>=1000000){unsigned replies=ping_replies.load()+AsyncPing::replies();reply_hz=(replies-rate_replies)*1e6f/(now-rate_started);rate_replies=replies;rate_started=now;}
    if(traffic_changed){stopPing();traffic_changed=false;next_probe_attempt=now;}
    if(reconfigure){
        stopPing();online=false;associated=false;esp_wifi_disconnect();
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|(legacy_only?0:WIFI_PROTOCOL_11N)));
        wifi_config_t cfg{};memcpy(cfg.sta.ssid,settings.ssid,std::strlen(settings.ssid));
        memcpy(cfg.sta.password,settings.password,std::strlen(settings.password));
        cfg.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&cfg));
        reconfigure=false;connecting=false;connect_started=0;next_connect=now+500000;
    }
    if(!online){
        stopPing();
        // The driver owns scan/authentication until it emits a terminal event.
        // Calling connect repeatedly while that operation is in flight is invalid.
        if(connecting&&!associated&&connect_started&&now-connect_started>30000000){
            esp_wifi_disconnect();connecting=false;next_connect=now+RouterDefaults::reconnect_delay_ms*1000;
        }
        if(configured()&&!associated&&!connecting&&now>=next_connect){
            connect_error=esp_wifi_connect();
            connecting=connect_error==ESP_OK||connect_error==ESP_ERR_WIFI_CONN;
            connect_started=now;next_connect=now+RouterDefaults::reconnect_delay_ms*1000;
        }
        return;
    }
    connecting=false;maintain_neighbor(now);
    if(!enabled){stopPing();return;}
    if(asynchronous){
        if(!AsyncPing::running()&&now>=next_probe_attempt){AsyncPing::start(inet_addr(settings.target),settings.ping_hz,RouterDefaults::ping_timeout_ms);next_probe_attempt=now+1000000;}
        return;
    }
    if(!ping){
        esp_ping_config_t cfg=ESP_PING_DEFAULT_CONFIG();
        cfg.count=0;cfg.interval_ms=1000/settings.ping_hz;cfg.timeout_ms=RouterDefaults::ping_timeout_ms;
        cfg.data_size=8;cfg.task_stack_size=4096;
        cfg.target_addr.type=ESP_IPADDR_TYPE_V4;cfg.target_addr.u_addr.ip4.addr=inet_addr(settings.target);
        esp_ping_callbacks_t cb{};cb.on_ping_success=success;cb.on_ping_timeout=timeout;
        if(esp_ping_new_session(&cfg,&cb,&ping)==ESP_OK)esp_ping_start(ping);
        else ping=nullptr;
    }
}
bool ready(){return online.load();}
bool useHT(){return ht_acquisition.load();}
bool accept(const wifi_csi_info_t* info){
    bool match;portENTER_CRITICAL(&guard);match=online&&memcmp(info->mac,bssid,6)==0;portEXIT_CRITICAL(&guard);return match;
}
uint32_t session(){return generation.load();}
const char* reason(){return !configured()?"NEEDS_WIFI_CONFIG":online?"CONNECTED":associated?"WAITING_DHCP":connecting?"WIFI_CONNECTING":"WIFI_DISCONNECTED";}
void appendStatus(cJSON* j){
    auto* w=cJSON_AddObjectToObject(j,"router");auto a=AsyncPing::stats();
    cJSON_AddBoolToObject(w,"arp_keepalive",arp_keepalive);cJSON_AddBoolToObject(w,"neighbor_resolved",neighbor_known);
    cJSON_AddNumberToObject(w,"neighbor_checks",neighbor_checks);cJSON_AddNumberToObject(w,"neighbor_unknown",neighbor_unknown);cJSON_AddNumberToObject(w,"arp_requests",arp_requests);cJSON_AddNumberToObject(w,"arp_errors",arp_errors);
    cJSON_AddStringToObject(w,"probe_mode",asynchronous?"async":"sequential");
    cJSON_AddNumberToObject(w,"ping_sent",ping_completed_sent.load()+a.sent);
    cJSON_AddNumberToObject(w,"backpressure_skips",a.backpressure);cJSON_AddNumberToObject(w,"error_heap_free",a.error_heap_free);cJSON_AddNumberToObject(w,"error_heap_largest",a.error_heap_largest);
    cJSON_AddNumberToObject(w,"ping_send_errors",a.send_errors);cJSON_AddNumberToObject(w,"schedule_skips",a.skipped);
    cJSON_AddNumberToObject(w,"pending_requests",a.pending);cJSON_AddNumberToObject(w,"max_pending_requests",a.max_pending);
    if(asynchronous){cJSON_AddNumberToObject(w,"send_interval_p50_us",a.send_p50_us);cJSON_AddNumberToObject(w,"send_interval_p95_us",a.send_p95_us);cJSON_AddNumberToObject(w,"send_interval_max_us",a.send_max_us);}
    else{cJSON_AddNullToObject(w,"send_interval_p50_us");cJSON_AddNullToObject(w,"send_interval_p95_us");cJSON_AddNullToObject(w,"send_interval_max_us");}
    if(asynchronous)cJSON_AddNumberToObject(w,"rtt_p95_us",a.rtt_p95_us);else cJSON_AddNullToObject(w,"rtt_p95_us");cJSON_AddNumberToObject(w,"probe_socket_error",a.error);
    cJSON_AddNumberToObject(w,"unmatched_replies",a.unmatched);cJSON_AddNumberToObject(w,"invalid_replies",a.invalid);
    cJSON_AddStringToObject(w,"ssid",settings.ssid);cJSON_AddStringToObject(w,"target",settings.target);
    cJSON_AddBoolToObject(w,"legacy_only",legacy_only);
    cJSON_AddStringToObject(w,"connection",reason());cJSON_AddBoolToObject(w,"configured",configured());cJSON_AddBoolToObject(w,"connected",online);
    char mac[18],ip[16];portENTER_CRITICAL(&guard);
    snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5]);memcpy(ip,local_ip,sizeof(ip));portEXIT_CRITICAL(&guard);
    cJSON_AddStringToObject(w,"bssid",mac);cJSON_AddStringToObject(w,"local_ip",ip);
    cJSON_AddNumberToObject(w,"last_disconnect_reason",last_disconnect.load());
    cJSON_AddStringToObject(w,"connect_error",esp_err_to_name(connect_error));
    cJSON_AddNumberToObject(w,"session",generation.load());cJSON_AddNumberToObject(w,"ping_hz_requested",settings.ping_hz);
    cJSON_AddNumberToObject(w,"ping_interval_ms",asynchronous?1000.0/settings.ping_hz:double(1000/settings.ping_hz));
    cJSON_AddNumberToObject(w,"ping_sent_completed",ping_completed_sent.load()+a.replies+a.timeouts);
    cJSON_AddNumberToObject(w,"ping_replies",ping_replies.load()+a.replies);cJSON_AddNumberToObject(w,"ping_timeouts",ping_timeouts.load()+a.timeouts);
    if(ping_replies.load()+a.replies)cJSON_AddNumberToObject(w,"last_rtt_ms",asynchronous?a.last_rtt_us/1000.0:rtt_ms.load());else cJSON_AddNullToObject(w,"last_rtt_ms");
    cJSON_AddNumberToObject(w,"ping_reply_hz",reply_hz);
    const unsigned received=ping_replies.load()+a.replies,missed=ping_timeouts.load()+a.timeouts;
    if(received+missed)cJSON_AddNumberToObject(w,"ping_loss_fraction",double(missed)/(received+missed));else cJSON_AddNullToObject(w,"ping_loss_fraction");
    cJSON_AddStringToObject(w,"csi_scope","AP-source frames, not one-to-one matched ICMP replies");
}
}
