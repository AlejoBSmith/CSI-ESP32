#include "radio_config.h"
#include "detector.h"
#include "build_version.h"
#include "activity_led.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <atomic>
#include <unistd.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "router_link.h"
#include "router_timing.h"
#include "csi_quality.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_csi_gain_ctrl.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#ifndef RAT_TX
#define RAT_TX 0
#endif
static Config config;
static ArrivalTiming arrivals;
static WifiDuplicates duplicate_frames;static RadioClock radio_clock;
static std::atomic<uint32_t> csi_callbacks{0},hardware_errors{0},duplicate_drops{0},malformed_drops{0},stale_timestamps{0};
// No PC is needed for sensing. USB telemetry is leased by host commands.
static std::atomic<uint64_t> telemetry_until{0};
static bool telemetry_active(){return usb_serial_jtag_is_connected() && uint64_t(esp_timer_get_time())<telemetry_until.load();}
static bool serial_write(const void* data,size_t size){
    const auto* p=static_cast<const uint8_t*>(data);
    const int64_t deadline=esp_timer_get_time()+30000;
    while(size && telemetry_active() && esp_timer_get_time()<deadline){
        int n=usb_serial_jtag_write_bytes(p,size,pdMS_TO_TICKS(5));
        if(n>0){p+=n;size-=n;}else vTaskDelay(1);
    }
    return size==0; // A partial frame is rejected by the host CRC/resync decoder.
}
static int serial_read(void* data,size_t size){
    return usb_serial_jtag_read_bytes(data,size,pdMS_TO_TICKS(100));
}
static uint64_t calibration_due=0;
static char device_id[18]{};
static uint8_t local_mac[6]{};
static unsigned led_level=0;
static PeakScale led_scale;
static uint32_t boot_id;
static QueueHandle_t frames,telemetry,commands;
static StaticQueue_t frame_queue_control;
static RatDetector* detector;
static void request_calibration(){
    detector->reset("AMPLITUDE_REFERENCE_REQUIRED");led_scale.reset();
    calibration_due=esp_timer_get_time()+Experiment::calibration_delay_us;
}
static std::atomic<uint32_t> queue_drops{0},telemetry_drops{0},rx_count{0},rejected{0},tx_count{0},tx_errors{0};
static std::atomic<bool> enabled{true},raw_enabled{false};
static std::atomic<unsigned> tx_hz{500};
static std::atomic<uint32_t> tx_seq{0};
static uint32_t gain_frames=0;
static esp_err_t gain_lock_error=ESP_OK;
static RawMeta last_meta{};
static uint64_t dsp_total_us=0,dsp_frames=0;
static uint32_t dsp_max_us=0,core_max_us=0;static uint64_t core_total_us=0;
struct Message { uint8_t type; uint16_t length; uint8_t* data; };
struct Command {char line[512];};

static uint32_t crc32(uint32_t crc,const uint8_t* p,size_t len){
    crc=~crc;while(len--){crc^=*p++;for(int i=0;i<8;++i)crc=(crc>>1)^((0-(crc&1))&0xedb88320);}return ~crc;
}
static void send_message(uint8_t type,const void* data,size_t len){
    if(!telemetry_active())return;
    if(len>60000){++telemetry_drops;return;}
    Message m{type,(uint16_t)len,(uint8_t*)heap_caps_malloc(len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)};
    if(!m.data){++telemetry_drops;return;}
    memcpy(m.data,data,len);
    if(xQueueSend(telemetry,&m,0)!=pdTRUE){free(m.data);++telemetry_drops;}
}
static void send_json(uint8_t type,cJSON* j){char* s=cJSON_PrintUnformatted(j);if(s){send_message(type,s,strlen(s));free(s);}cJSON_Delete(j);}
static void number(cJSON* j,const char* k,double v){if(std::isfinite(v))cJSON_AddNumberToObject(j,k,v);else cJSON_AddNullToObject(j,k);}
static void message(const char* kind,const char* text){auto*j=cJSON_CreateObject();cJSON_AddStringToObject(j,"kind",kind);cJSON_AddStringToObject(j,"message",text);send_json(5,j);}
static cJSON* configuration(){
    auto*j=cJSON_CreateObject();
#define FIELD(k) number(j,#k,config.k)
    FIELD(channel);FIELD(tx_hz);FIELD(window);FIELD(hop);FIELD(top_k);FIELD(enter_hops);FIELD(exit_hops);FIELD(hampel_window);FIELD(hampel_sigma);FIELD(cutoff);FIELD(min_fs);FIELD(enter_score);FIELD(exit_score);FIELD(profile);FIELD(coverage_fraction);FIELD(fast_window);FIELD(detector_kind);FIELD(carrier_mode);FIELD(fast_weight);FIELD(scale_count);FIELD(consensus_scales);FIELD(confirm_hops);
#undef FIELD
    return j;
}
static bool valid(const Config& c){return c.channel>=1&&c.channel<=13&&(c.tx_hz>=10&&c.tx_hz<=500)&&c.window>=16&&c.window<=RatDetector::MAX_WINDOW&&c.fast_window>=16&&c.fast_window<=c.window&&c.hop>0&&c.hop<=c.window&&c.detector_kind<=2&&c.scale_count>=2&&c.scale_count<=RatDetector::MAX_SCALES&&c.consensus_scales>=2&&c.consensus_scales<=c.scale_count&&c.confirm_hops>=1&&c.confirm_hops<=4&&(c.detector_kind!=2||c.window>=c.fast_window+c.scale_count-1)&&c.carrier_mode<=2&&std::isfinite(c.fast_weight)&&c.fast_weight>=0&&c.fast_weight<=1&&c.top_k>0&&c.top_k<=32&&c.enter_hops>0&&c.enter_hops<=20&&c.exit_hops>0&&c.exit_hops<=20&&c.hampel_window>=3&&c.hampel_window<=15&&(c.hampel_window&1)&&std::isfinite(c.enter_score)&&std::isfinite(c.exit_score)&&c.enter_score>c.exit_score&&c.exit_score>=0&&std::isfinite(c.min_fs)&&c.min_fs>=25&&c.min_fs<=1000&&c.cutoff>=0&&c.cutoff<std::min(c.min_fs,Experiment::output_hz)/2&&std::isfinite(c.hampel_sigma)&&c.hampel_sigma>0&&c.profile==0&&std::isfinite(c.coverage_fraction)&&c.coverage_fraction>=.1f&&c.coverage_fraction<=1;}
static void bank_fields(cJSON* j){
    bool bank=config.detector_kind==2;
    number(j,"bank_raw",bank?detector->bank_raw:0);number(j,"bank_valid",bank?detector->bank_valid:0);
    number(j,"bank_total",bank?detector->bank_total:0);number(j,"bank_support",bank?detector->bank_support:0);
    number(j,"bank_dominant_ms",bank?detector->bank_dominant_ms:0);number(j,"confirmation_progress",bank?detector->confirmation_progress:0);
    cJSON_AddBoolToObject(j,"bank_confirmed",bank&&detector->bank_confirmed);
    auto* scales=cJSON_AddArrayToObject(j,"scales");
    if(bank)for(unsigned i=0;i<detector->bank_total;++i){auto* item=cJSON_CreateObject();auto& r=detector->scales[i];
        number(item,"window",detector->scale_windows[i]);number(item,"period_ms",detector->scale_windows[i]*1000.f/Experiment::output_hz);
        number(item,"score",r.score);cJSON_AddBoolToObject(item,"valid",r.valid);number(item,"coverage",r.coverage);number(item,"coherence",r.coherence);cJSON_AddItemToArray(scales,item);
    }
}
static void status(){
    if(!telemetry_active())return;
    auto*j=cJSON_CreateObject();number(j,"v",2);number(j,"node",RAT_NODE_ID);number(j,"boot",boot_id);
    cJSON_AddStringToObject(j,"device_id",device_id);number(j,"led_brightness",led_level/1023.0);
    number(j,"calibration_wait_s",calibration_due?std::max<int64_t>(0,int64_t(calibration_due)-esp_timer_get_time()+999999)/1000000:0);
    cJSON_AddStringToObject(j,"role",RAT_TX?"TX":"RX");cJSON_AddStringToObject(j,"mode","MODE_RAT_EDGE");
    cJSON_AddStringToObject(j,"acquisition","router_ping");RouterLink::appendStatus(j);
    if(!RouterLink::ready()){cJSON_AddStringToObject(j,"connection_reason",RouterLink::reason());}
    cJSON_AddStringToObject(j,"state",!RouterLink::ready()?"INVALID":detector->state);
    cJSON_AddStringToObject(j,"reason",!RouterLink::ready()?RouterLink::reason():detector->reason);
    number(j,"rx_us",esp_timer_get_time());number(j,"rx_count",rx_count);number(j,"rejected",rejected);
    number(j,"csi_callbacks",csi_callbacks);number(j,"hardware_errors",hardware_errors);number(j,"duplicate_drops",duplicate_drops);number(j,"malformed_drops",malformed_drops);number(j,"stale_timestamps",stale_timestamps);
    number(j,"queue_drops",queue_drops);number(j,"telemetry_drops",telemetry_drops);
    number(j,"tx_count",tx_count);number(j,"tx_errors",tx_errors);number(j,"commanded_hz",tx_hz);
    number(j,"effective_fs",detector->effective_fs);cJSON_AddNullToObject(j,"packet_loss"); // AP receive counter cannot measure RF loss.
    number(j,"csi_interval_p50_us",arrivals.percentile(50));number(j,"csi_interval_p95_us",arrivals.percentile(95));
    number(j,"csi_interval_max_us",arrivals.maximum);number(j,"csi_gaps_over_50ms",arrivals.gaps);
    cJSON_AddStringToObject(j,"sequence_kind","local_ap_csi_counter");
    number(j,"score",detector->score);cJSON_AddBoolToObject(j,"measurement_valid",detector->measurement_valid&&RouterLink::ready());
    cJSON_AddBoolToObject(j,"amplitude_calibrated",detector->calibrated);number(j,"calibration_progress",detector->calibration_progress);
    cJSON_AddStringToObject(j,"score_units",config.detector_kind?"coherent_subcarrier_iqr_percent":"coherent_shape_variation_percent");
    cJSON_AddStringToObject(j,"algorithm",config.detector_kind==2?"multicarrier_iqr_acf_scale_bank_v3":config.detector_kind?"multicarrier_iqr_acf_dualscale_v2":"fixed_band_spatial_cv_acf_v1");
    number(j,"score_fast",detector->fast.score);number(j,"score_slow",detector->slow.score);number(j,"coherence_fast",detector->fast.coherence);number(j,"coherence_slow",detector->slow.coherence);
    number(j,"coverage_fast",detector->fast.coverage);number(j,"coverage_slow",detector->slow.coverage);
    cJSON_AddBoolToObject(j,"fast_valid",detector->fast.valid);cJSON_AddBoolToObject(j,"slow_valid",detector->slow.valid);
    number(j,"fast_weight_applied",detector->fast_weight_applied);cJSON_AddStringToObject(j,"fusion_source",detector->fusion_source);
    number(j,"score_legacy",detector->legacy_score);cJSON_AddBoolToObject(j,"legacy_valid",detector->legacy_valid);bank_fields(j);

    number(j,"turbulence",detector->turbulence);number(j,"turbulence_relative_iqr",detector->turbulence_relative_iqr);
    number(j,"temporal_autocorr",detector->temporal_autocorr);number(j,"coherence_floor",detector->coherence_floor);
    number(j,"shape_displacement",detector->shape_displacement);number(j,"window_coverage",detector->window_coverage);
    number(j,"admitted_count",detector->admitted_count);number(j,"excess_packets",detector->excess_packets);number(j,"shifted_packets",detector->shifted_packets);number(j,"missing_slots",detector->missing_slots);number(j,"led_peak",led_scale.peak);
    cJSON_AddStringToObject(j,"last_recovery",detector->last_recovery);number(j,"recovery_count",detector->recovery_count);number(j,"selected",detector->selected);number(j,"votes",detector->votes);
    number(j,"usable_carriers",detector->usable);number(j,"required_carriers",detector->required_carriers);
    number(j,"missing_values",detector->missing_values);number(j,"missing_now",detector->missing_now);
    number(j,"seq0",detector->seq0);number(j,"seq1",detector->seq1);number(j,"tx_boot",last_meta.tx_boot);
    number(j,"reference_id",detector->reference_id);number(j,"rssi",last_meta.rssi);number(j,"noise_floor",last_meta.noise_floor);
    number(j,"gain",last_meta.gain);number(j,"csi_len",last_meta.csi_len);
    cJSON_AddBoolToObject(j,"csi_manual_scale",true);number(j,"csi_shift",RouterDefaults::csi_shift);
    cJSON_AddBoolToObject(j,"ampdu_rx",RouterDefaults::ampdu_rx);
    cJSON_AddBoolToObject(j,"rx_gain_fixed",esp_csi_gain_ctrl_get_gain_status()==RX_GAIN_FORCE);
    cJSON_AddStringToObject(j,"rx_gain_error",esp_err_to_name(gain_lock_error));
    number(j,"dsp_mean_us",dsp_frames?double(dsp_total_us)/dsp_frames:0);number(j,"dsp_max_us",dsp_max_us);number(j,"dsp_frames",dsp_frames);number(j,"core_mean_us",dsp_frames?double(core_total_us)/dsp_frames:0);number(j,"core_max_us",core_max_us);number(j,"dsp_stack_free",uxTaskGetStackHighWaterMark(nullptr));
    number(j,"queue_pending",uxQueueMessagesWaiting(frames));
    number(j,"heap_internal_free",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));number(j,"heap_internal_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    char elf_hash[65];const auto* app=esp_app_get_description();for(unsigned i=0;i<32;++i)snprintf(elf_hash+i*2,3,"%02x",app->app_elf_sha256[i]);cJSON_AddStringToObject(j,"elf_sha256",elf_hash);
    cJSON_AddStringToObject(j,"idf",esp_get_idf_version());cJSON_AddStringToObject(j,"commit",RAT_COMMIT);
    cJSON_AddStringToObject(j,"source_sha256",RAT_SOURCE_HASH);cJSON_AddStringToObject(j,"esp_csi_commit","8633d67152db2808f141cc1595970aa9cf406045");
    cJSON_AddStringToObject(j,"model","ESP32-S3");cJSON_AddStringToObject(j,"gain_component","0.1.4");
    cJSON_AddStringToObject(j,"board","Seeed Studio XIAO ESP32S3");cJSON_AddStringToObject(j,"transport","USB Serial/JTAG");
    cJSON_AddStringToObject(j,"csi_layout",(last_meta.flags&4)?"HTLTF_HT20":"LLTF_LEGACY20");
    int8_t power=0;esp_wifi_get_max_tx_power(&power);number(j,"tx_power_quarter_dbm",power);
    cJSON_AddItemToObject(j,"config",configuration());
    auto*tuning=cJSON_AddObjectToObject(j,"tuning");
#define TUNE(k) number(tuning,#k,Experiment::k)
    TUNE(output_hz);TUNE(amplitude_presence);TUNE(amplitude_cal_us);
    TUNE(rate_interval_us);TUNE(rate_change_fraction);
    TUNE(autocorr_noise_sigma);TUNE(shape_lag_us);TUNE(sample_coverage_fraction);TUNE(max_sample_gap_us);TUNE(max_interpolation_gap_us);TUNE(no_csi_timeout_us);TUNE(hampel_min_scale);
    TUNE(amplitude_epsilon);TUNE(feature_epsilon);TUNE(gain_warmup_frames);
    TUNE(max_window);TUNE(amplitude_reservoir);
    TUNE(rx_queue_frames);TUNE(telemetry_queue_messages);TUNE(worker_yield_us);
#undef TUNE
    auto* top=cJSON_AddArrayToObject(j,"top");for(unsigned i=0;i<detector->top_count;++i)cJSON_AddItemToArray(top,cJSON_CreateNumber(detector->top[i]));
    send_json(2,j);
}
// Use only the first HT-LTF of HT20 frames; LLTF may change with AP TX mapping.
static void csi_callback(void*,wifi_csi_info_t*info){
    if(!enabled||!info||!info->buf)return;
    ++csi_callbacks;
    if(info->rx_ctrl.rx_state){++hardware_errors;++rejected;return;}
    if(!RouterLink::accept(info)||memcmp(info->dmac,local_mac,6)||!info->hdr||
        (info->hdr[0]&0x0c)!=0x08||(info->hdr[0]&0x40)||info->rx_ctrl.cwb||
        (RouterLink::useHT()?(info->rx_ctrl.sig_mode!=1||info->len<256):(info->rx_ctrl.sig_mode!=0||info->len!=128))){++rejected;return;}
    WifiFrameKey key;
    if(!dataFrameKey(info->hdr,info->rx_ctrl.sig_len,key)){++malformed_drops;++rejected;return;}
    uint64_t now=esp_timer_get_time();const uint32_t session=RouterLink::session();
    if(duplicate_frames.contains(key,now,session)){++duplicate_drops;++rejected;return;}
    uint64_t rx_us=0;if(!radio_clock.observe(info->rx_ctrl.timestamp,now,rx_us)){++stale_timestamps;++rejected;return;}
    Frame f{};auto&m=f.meta;const auto&r=info->rx_ctrl;
    const bool ht=r.sig_mode==1;
    m.node=RAT_NODE_ID;m.flags=ht?4:(info->first_word_invalid?1:0);m.csi_len=128;
    m.tx_seq=++tx_seq;m.tx_boot=RouterLink::session();m.rx_boot=boot_id;
    m.rx_us=rx_us;m.radio_us=r.timestamp;
    m.rssi=r.rssi;m.noise_floor=r.noise_floor;m.channel=r.channel;m.bandwidth=r.cwb;
    m.sig_mode=r.sig_mode;m.mcs=r.mcs;m.stbc=r.stbc;m.rate=r.rate;
    esp_csi_gain_ctrl_get_rx_gain(&r,&m.agc,&m.fft);
    memcpy(m.source,info->mac,6);memcpy(f.iq,info->buf+(ht?128:0),128);++rx_count;
    f.phy_length=r.sig_len;f.frame_control=info->hdr?info->hdr[0]:0xff;
    f.phy_flags=r.aggregation|(r.smoothing<<1)|(r.not_sounding<<2)|(r.ant<<3);
    if(xQueueSend(frames,&f,0)!=pdTRUE)++queue_drops;else duplicate_frames.remember(key,now);
}
static void telemetry_task(void*){
    Message m;while(true)if(xQueueReceive(telemetry,&m,portMAX_DELAY)==pdTRUE){
        WireHeader h;h.type=m.type;h.length=m.length;uint32_t crc=crc32(0,(uint8_t*)&h,sizeof(h));crc=crc32(crc,m.data,m.length);
        if(telemetry_active()){
            if(!serial_write(&h,sizeof(h)) || !serial_write(m.data,m.length) || !serial_write(&crc,4))++telemetry_drops;
        }
        free(m.data);
    }
}
static void command_task(void*){
    Command c{};unsigned used=0;bool overflow=false;
    while(true){uint8_t ch;if(serial_read(&ch,1)!=1)continue;
        if(ch=='\n'){if(!overflow&&used){c.line[used]=0;xQueueSend(commands,&c,0);}used=0;overflow=false;}
        else if(ch!='\r'){if(used<sizeof(c.line)-1)c.line[used++]=ch;else overflow=true;}
    }
}
static void feature_event(){
    if(!telemetry_active())return;
    auto*j=cJSON_CreateObject();number(j,"node",RAT_NODE_ID);number(j,"boot",boot_id);number(j,"rx_us",detector->result_us);number(j,"seq0",detector->seq0);number(j,"seq1",detector->seq1);
    number(j,"reference_id",detector->reference_id);number(j,"effective_fs",detector->effective_fs);number(j,"filter_fs",detector->filter_fs);number(j,"score",detector->score);cJSON_AddBoolToObject(j,"measurement_valid",detector->measurement_valid&&RouterLink::ready());
    cJSON_AddBoolToObject(j,"amplitude_calibrated",detector->calibrated);number(j,"calibration_progress",detector->calibration_progress);
    cJSON_AddStringToObject(j,"score_units",config.detector_kind?"coherent_subcarrier_iqr_percent":"coherent_shape_variation_percent");
    cJSON_AddStringToObject(j,"algorithm",config.detector_kind==2?"multicarrier_iqr_acf_scale_bank_v3":config.detector_kind?"multicarrier_iqr_acf_dualscale_v2":"fixed_band_spatial_cv_acf_v1");
    number(j,"score_fast",detector->fast.score);number(j,"score_slow",detector->slow.score);number(j,"coherence_fast",detector->fast.coherence);number(j,"coherence_slow",detector->slow.coherence);
    number(j,"coverage_fast",detector->fast.coverage);number(j,"coverage_slow",detector->slow.coverage);
    cJSON_AddBoolToObject(j,"fast_valid",detector->fast.valid);cJSON_AddBoolToObject(j,"slow_valid",detector->slow.valid);
    number(j,"fast_weight_applied",detector->fast_weight_applied);cJSON_AddStringToObject(j,"fusion_source",detector->fusion_source);
    number(j,"score_legacy",detector->legacy_score);cJSON_AddBoolToObject(j,"legacy_valid",detector->legacy_valid);bank_fields(j);

    number(j,"turbulence",detector->turbulence);number(j,"turbulence_relative_iqr",detector->turbulence_relative_iqr);
    number(j,"temporal_autocorr",detector->temporal_autocorr);number(j,"coherence_floor",detector->coherence_floor);
    number(j,"shape_displacement",detector->shape_displacement);number(j,"window_coverage",detector->window_coverage);
    number(j,"admitted_count",detector->admitted_count);number(j,"excess_packets",detector->excess_packets);number(j,"shifted_packets",detector->shifted_packets);number(j,"missing_slots",detector->missing_slots);number(j,"led_peak",led_scale.peak);
    cJSON_AddStringToObject(j,"last_recovery",detector->last_recovery);number(j,"recovery_count",detector->recovery_count);
    number(j,"usable_carriers",detector->usable);number(j,"required_carriers",detector->required_carriers);
    cJSON_AddStringToObject(j,"state",detector->state);cJSON_AddStringToObject(j,"reason",detector->reason);
    auto*a=cJSON_AddArrayToObject(j,"carriers");
    for(unsigned k=0;k<MAX_CARRIERS;++k)if(raw_enabled&&detector->window_valid[k]){
        auto*b=cJSON_CreateArray();const auto&f=detector->features[k];
        float values[]={float(k),f.mad,f.iqr,f.sd,f.derivative,f.z};for(float v:values)cJSON_AddItemToArray(b,std::isfinite(v)?cJSON_CreateNumber(v):cJSON_CreateNull());cJSON_AddItemToArray(a,b);
    }
    send_json(3,j);
}
static void calibration_event(){
    if(!telemetry_active())return;
    auto*j=cJSON_CreateObject();number(j,"node",RAT_NODE_ID);number(j,"boot",boot_id);number(j,"reference_id",detector->reference_id);
    number(j,"feature_epsilon",Experiment::feature_epsilon);
    number(j,"filter_fs",detector->filter_fs);cJSON_AddItemToObject(j,"config",configuration());auto*a=cJSON_AddArrayToObject(j,"carriers");
    for(unsigned k=0;k<MAX_CARRIERS;++k)if(detector->mask[k]){auto*b=cJSON_CreateArray();float values[]={float(k),detector->amp_median[k]};for(float v:values)cJSON_AddItemToArray(b,cJSON_CreateNumber(v));cJSON_AddItemToArray(a,b);}
    send_json(4,j);
}
static void selftest(){
    Biquad filter;filter.configure(100,10,.7071067f);filter.seed(1);
    bool ok=std::fabs(filter.add(1)-1)<1e-4f;
    auto*j=cJSON_CreateObject();cJSON_AddStringToObject(j,"kind","SELFTEST");
    cJSON_AddStringToObject(j,"result",ok?"PASS":"FAIL");
    cJSON_AddStringToObject(j,"evidence","filter numerical check only; use native tests for full DSP");send_json(5,j);
}
// Optional synthetic benchmark: a separate detector on a lower-priority task.
// It never publishes synthetic data as measurements or controls the real LED.
static std::atomic<bool> benchmark_running{false};static Config benchmark_config;
static void benchmark_task(void*){
    void* memory=heap_caps_malloc(sizeof(RatDetector),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!memory){message("ERROR","Cannot allocate independent benchmark detector");benchmark_running=false;vTaskDelete(nullptr);return;}
    auto* test=new(memory)RatDetector;test->config=benchmark_config;test->calibrated=true;
    Frame f{};f.meta.flags=4;f.meta.channel=test->config.channel;f.meta.csi_len=128;f.meta.tx_boot=1;
    unsigned count=std::min<unsigned>(2000,test->config.window+400),valid=0,steady_count=0,eval_count=0;uint64_t elapsed_sum=0,steady_sum=0,eval_sum=0,begin=esp_timer_get_time();uint32_t maximum=0,queue_before=queue_drops,usb_before=telemetry_drops;float peak=0;
    for(unsigned i=0;i<count;++i){
        float motion=12*std::sin(float(i)*.062831853f);
        for(unsigned k=0;k<64;++k){f.iq[2*k]=0;f.iq[2*k+1]=int8_t(std::round(40+motion*(k<32?1:-1)));}
        f.meta.rx_us=1000000+uint64_t(i)*10000;f.meta.tx_seq=i+1;
        uint64_t started=esp_timer_get_time();test->add(f);uint32_t elapsed=esp_timer_get_time()-started;
        elapsed_sum+=elapsed;maximum=std::max(maximum,elapsed);valid+=test->emit&&test->measurement_valid;peak=std::max(peak,test->score);if(i>=test->config.window){steady_sum+=elapsed;++steady_count;if(test->emit){eval_sum+=elapsed;++eval_count;}}
        if(i%8==0)vTaskDelay(1);
    }
    auto* j=cJSON_CreateObject();cJSON_AddStringToObject(j,"kind","DSP_BENCHMARK");
    number(j,"frames",count);number(j,"window",test->config.window);number(j,"scales",test->bank_total);number(j,"valid_outputs",valid);number(j,"peak_score",peak);
    number(j,"mean_call_us",double(elapsed_sum)/count);number(j,"steady_mean_call_us",steady_count?double(steady_sum)/steady_count:0);number(j,"mean_evaluation_us",eval_count?double(eval_sum)/eval_count:0);number(j,"max_call_us",maximum);number(j,"wall_us",esp_timer_get_time()-begin);number(j,"stack_free_bytes",uxTaskGetStackHighWaterMark(nullptr));
    number(j,"real_queue_drops",queue_drops);number(j,"real_usb_drops",telemetry_drops);number(j,"queue_drops_delta",queue_drops.load()-queue_before);number(j,"usb_drops_delta",telemetry_drops.load()-usb_before);cJSON_AddStringToObject(j,"timing","Elapsed call time includes preemption; independent synthetic detector, low priority");
    test->~RatDetector();free(memory);send_json(5,j);benchmark_running=false;vTaskDelete(nullptr);
}
static void handle_command(const Command& c){
    telemetry_until=esp_timer_get_time()+Experiment::telemetry_lease_us;
    auto*j=cJSON_Parse(c.line);if(!j){message("ERROR","Invalid JSON");return;}
    auto*cmd=cJSON_GetObjectItem(j,"cmd");const char*s=cJSON_IsString(cmd)?cmd->valuestring:"";
    if(!strcmp(s,"status"))status();
    else if(!strcmp(s,"wifi")||!strcmp(s,"router_config")){
        const char* error=nullptr;
        if(RouterLink::configure(j,error)){
            xQueueReset(frames);gain_frames=0;request_calibration();message("OK","Router settings saved on device; reconnecting");
        }else message("ERROR",error);
    }
    else if(!strcmp(s,"traffic")){
        const char* error=nullptr;if(RouterLink::configureTraffic(j,error))message("OK","Traffic settings applied; WiFi and detector reference preserved");else message("ERROR",error);
    }
    else if(!strcmp(s,"wifi_forget")){
        if(RouterLink::forget()){calibration_due=0;detector->reset("NEEDS_WIFI_CONFIG");message("OK","Router credentials removed from this variant");}
        else message("ERROR","Cannot remove router credentials");
    }
    else if(!strcmp(s,"rx_gain")){
        auto* fixed=cJSON_GetObjectItem(j,"fixed");
        if(!cJSON_IsBool(fixed)){message("ERROR","rx_gain requires boolean fixed");}
        else{
            uint8_t a=0;int8_t b=0;gain_lock_error=ESP_OK;
            if(cJSON_IsTrue(fixed))gain_lock_error=esp_csi_gain_ctrl_get_rx_gain_baseline(&a,&b);
            if(gain_lock_error==ESP_OK)gain_lock_error=esp_csi_gain_ctrl_set_rx_force_gain(a,b);
            if(gain_lock_error==ESP_OK){request_calibration();message("OK","RX gain mode changed; amplitude reference scheduled");}
            else message("ERROR",esp_err_to_name(gain_lock_error));
        }
    }
    else if(!strcmp(s,"calibrate")){if(RAT_TX)message("ERROR","Calibration requires RX");else{calibration_due=0;detector->startCalibration();message("OK","Amplitude reference started; no activity baseline is learned");}}
    else if(!strcmp(s,"calibrate_after_delay")){if(RAT_TX)message("ERROR","Calibration requires RX");else{request_calibration();message("OK","Calibration begins in 20 seconds");}}
    else if(!strcmp(s,"cancel_calibration")){calibration_due=0;detector->reset("CALIBRATION_CANCELLED");message("OK","Calibration cancelled");}
    else if(!strcmp(s,"benchmark")){
        if(benchmark_running.exchange(true))message("ERROR","Benchmark already running");
        else{benchmark_config=config;if(xTaskCreatePinnedToCore(benchmark_task,"dsp_benchmark",24576,nullptr,1,nullptr,1)!=pdPASS){benchmark_running=false;message("ERROR","Cannot start benchmark task");}else message("OK","Independent synthetic benchmark started; sensing remains active");}
    }
    else if(!strcmp(s,"selftest"))selftest(); // This numerical filter check does not interrupt live sensing.
    else if(!strcmp(s,"raw")){raw_enabled=cJSON_IsTrue(cJSON_GetObjectItem(j,"enabled"));message("OK",raw_enabled?"raw enabled":"raw disabled");}
    else if(!strcmp(s,"stop")){calibration_due=0;enabled=false;detector->reset("STOPPED");message("OK","Stopped");}
    else if(!strcmp(s,"start")){xQueueReset(frames);enabled=true;if(!RAT_TX)request_calibration();message("OK","Started");}
    else if(!strcmp(s,"config")||!strcmp(s,"defaults")){
        Config next=!strcmp(s,"defaults")?Config{}:config;bool ok=true;
        cJSON* item=nullptr;cJSON_ArrayForEach(item,j){if(!strcmp(item->string,"cmd"))continue;if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)){ok=false;break;}const double v=item->valuedouble;bool found=false;
#define UINT_FIELD(k) if(!strcmp(item->string,#k)){found=true;if(v<0||v>100000||v!=floor(v))ok=false;else next.k=(uint32_t)v;}
#define FLOAT_FIELD(k) if(!strcmp(item->string,#k)){found=true;next.k=v;}
            UINT_FIELD(channel);UINT_FIELD(tx_hz);UINT_FIELD(window);UINT_FIELD(hop);UINT_FIELD(top_k);UINT_FIELD(enter_hops);UINT_FIELD(exit_hops);UINT_FIELD(hampel_window);UINT_FIELD(profile);UINT_FIELD(fast_window);UINT_FIELD(detector_kind);UINT_FIELD(carrier_mode);UINT_FIELD(scale_count);UINT_FIELD(consensus_scales);UINT_FIELD(confirm_hops);
            FLOAT_FIELD(hampel_sigma);FLOAT_FIELD(cutoff);FLOAT_FIELD(min_fs);FLOAT_FIELD(enter_score);FLOAT_FIELD(exit_score);FLOAT_FIELD(coverage_fraction);FLOAT_FIELD(fast_weight);
#undef UINT_FIELD
#undef FLOAT_FIELD
            if(!found)ok=false;
        }
        if(!ok||!valid(next))message("ERROR","Invalid config; unchanged");
        else{
            bool reboot=false; // AP selects the channel; this is an RX-only HT20 build.
            nvs_handle_t n;if(nvs_open("rat-router-dsp",NVS_READWRITE,&n)==ESP_OK){esp_err_t e=nvs_set_blob(n,"config",&next,sizeof(next));if(e==ESP_OK)e=nvs_commit(n);nvs_close(n);if(e!=ESP_OK){message("ERROR","NVS save failed");cJSON_Delete(j);return;}}
            else{message("ERROR","NVS open failed");cJSON_Delete(j);return;}
            calibration_due=0;config=next;tx_hz=next.tx_hz;detector->config=next;detector->reset("CONFIG_CHANGED");request_calibration();message("OK","Saved; amplitude reference scheduled in 20 seconds");
            if(reboot){enabled=false;vTaskDelay(pdMS_TO_TICKS(300));esp_restart();}
        }
    }
    else if(!strcmp(s,"scan"))message("ERROR","Use the AP panel for channel selection; live scans are disabled during sensing");
    else message("ERROR","Commands: status, wifi, router_config, wifi_forget, config, defaults, calibrate, calibrate_after_delay, cancel_calibration, raw, stop, start, selftest");
    cJSON_Delete(j);
}
static void init_indicator(){
    gpio_config_t button{};button.pin_bit_mask=1ULL<<Experiment::button_gpio;
    button.mode=GPIO_MODE_INPUT;button.pull_up_en=GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    ledc_timer_config_t timer{};timer.speed_mode=LEDC_LOW_SPEED_MODE;
    timer.duty_resolution=LEDC_TIMER_10_BIT;timer.timer_num=LEDC_TIMER_0;
    timer.freq_hz=5000;timer.clk_cfg=LEDC_AUTO_CLK;ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t led{};led.gpio_num=Experiment::led_gpio;led.speed_mode=LEDC_LOW_SPEED_MODE;
    led.channel=LEDC_CHANNEL_0;led.timer_sel=LEDC_TIMER_0;led.flags.output_invert=1;
    ESP_ERROR_CHECK(ledc_channel_config(&led));
}
static void update_indicator(uint64_t now){
    static uint64_t last=0;if(now-last<20000)return;last=now;
    static uint64_t pressed=0;static bool handled=false;
    if(!RAT_TX && enabled && gpio_get_level(gpio_num_t(Experiment::button_gpio))==0){
        if(!pressed)pressed=now;
        if(!handled && now-pressed>=Experiment::button_hold_us){request_calibration();handled=true;}
    }else{pressed=0;handled=false;}
    if(calibration_due && now>=calibration_due){calibration_due=0;detector->startCalibration();}
    if(detector->measurement_valid)led_scale.observe(detector->score);
    led_level=activityBrightness(detector->score,RouterLink::ready()?detector->state:"INVALID",calibration_due!=0,RAT_TX,enabled,now,led_scale.peak);
    ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,led_level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
}
static void worker_task(void*){
    Frame f;Command c;uint64_t last_status=0,last_yield=0;unsigned previous_ref=0;
    while(true){
        while(xQueueReceive(commands,&c,0)==pdTRUE)handle_command(c);
        RouterLink::tick(enabled);
        if(xQueueReceive(frames,&f,pdMS_TO_TICKS(10))==pdTRUE){
            arrivals.observe(f.meta.rx_us);
            if(last_meta.csi_len&&memcmp(last_meta.source,f.meta.source,6)){
                gain_frames=0;request_calibration(); // A different AP is a different amplitude environment.
            }
            if(detector->config.channel!=f.meta.channel){config.channel=f.meta.channel;detector->config.channel=f.meta.channel;}
            if(gain_frames<Experiment::gain_warmup_frames)esp_csi_gain_ctrl_record_rx_gain(f.meta.agc,f.meta.fft);
            else if(gain_frames==Experiment::gain_warmup_frames){
                uint8_t a;int8_t b;gain_lock_error=esp_csi_gain_ctrl_get_rx_gain_baseline(&a,&b);
                if(gain_lock_error==ESP_OK&&RouterDefaults::fixed_rx_gain)gain_lock_error=esp_csi_gain_ctrl_set_rx_force_gain(a,b);
            }
            if(gain_frames++>=Experiment::gain_warmup_frames){float gain=1;esp_err_t err=esp_csi_gain_ctrl_get_gain_compensation(&gain,f.meta.agc,f.meta.fft);f.meta.gain=gain;if(err==ESP_OK&&std::isfinite(gain)&&gain>0)f.meta.flags|=2;}else f.meta.gain=1;
            f.meta.queue_drops=queue_drops;f.meta.telemetry_drops=telemetry_drops;last_meta=f.meta;
            if(raw_enabled){
                send_message(1,&f,sizeof(RawMeta)+f.meta.csi_len);
                if(f.meta.tx_seq%10==0){auto*j=cJSON_CreateObject();cJSON_AddStringToObject(j,"kind","PHY_SAMPLE");number(j,"seq",f.meta.tx_seq);number(j,"frame_control",f.frame_control);number(j,"phy_length",f.phy_length);number(j,"phy_flags",f.phy_flags);send_json(7,j);}
            }
            if(enabled){uint64_t began=esp_timer_get_time();detector->add(f);uint32_t core_elapsed=esp_timer_get_time()-began;core_total_us+=core_elapsed;core_max_us=std::max(core_max_us,core_elapsed);if(detector->emit&&detector->selected)feature_event();if(detector->reference_id!=previous_ref){led_scale.reset();calibration_event();previous_ref=detector->reference_id;}uint32_t elapsed=esp_timer_get_time()-began;dsp_total_us+=elapsed;++dsp_frames;dsp_max_us=std::max(dsp_max_us,elapsed);}
        }else vTaskDelay(1);
        uint64_t now=esp_timer_get_time();update_indicator(now);if(now-last_status>1000000){last_status=now;if(!RAT_TX)detector->idle(now);status();}
        // A burst must not starve IDLE1/watchdog while draining the RX queue.
        if(now-last_yield>=Experiment::worker_yield_us){vTaskDelay(1);last_yield=esp_timer_get_time();}
    }
}
extern "C" void app_main(){
    ESP_ERROR_CHECK(nvs_flash_init()); // Preserve unrelated WiFi credentials; never erase NVS automatically.
    nvs_handle_t n;if(nvs_open("rat-router-dsp",NVS_READONLY,&n)==ESP_OK){Config saved;size_t size=sizeof(saved);if(nvs_get_blob(n,"config",&saved,&size)==ESP_OK&&upgradeConfig(saved,size)&&valid(saved))config=saved;nvs_close(n);}
    boot_id=esp_random();tx_hz=config.tx_hz;
    void* memory=heap_caps_malloc(sizeof(RatDetector),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(memory);detector=new(memory)RatDetector;detector->config=config;
    auto* frame_storage=(uint8_t*)heap_caps_malloc(Experiment::rx_queue_frames*sizeof(Frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(frame_storage);
    frames=xQueueCreateStatic(Experiment::rx_queue_frames,sizeof(Frame),frame_storage,&frame_queue_control);telemetry=xQueueCreate(Experiment::telemetry_queue_messages,sizeof(Message));commands=xQueueCreate(8,sizeof(Command));assert(frames&&telemetry&&commands);
    usb_serial_jtag_driver_config_t usb{};usb.rx_buffer_size=4096;usb.tx_buffer_size=16384;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    uint8_t mac[6];ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));memcpy(local_mac,mac,6);
    snprintf(device_id,sizeof(device_id),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    init_indicator();

    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
    assert(esp_netif_create_default_wifi_sta());
    wifi_init_config_t wifi=WIFI_INIT_CONFIG_DEFAULT();wifi.ampdu_rx_enable=RouterDefaults::ampdu_rx;ESP_ERROR_CHECK(esp_wifi_init(&wifi));ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA,WIFI_BW_HT20));
    RouterLink::init();
    ESP_ERROR_CHECK(esp_wifi_start());ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(80));
    wifi_csi_config_t c{};c.lltf_en=c.htltf_en=true;c.stbc_htltf2_en=false;
    c.ltf_merge_en=false;c.channel_filter_en=true;
    // Fixed CSI scaling is required for the AGC/FFT compensation to be meaningful.
    // Auto scaling adds an unreported, packet-dependent gain.
    c.manu_scale=true;c.shift=RouterDefaults::csi_shift;
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&c));ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_callback,nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    if(!RAT_TX&&Experiment::auto_calibrate)request_calibration();
    xTaskCreatePinnedToCore(telemetry_task,"usb",4096,nullptr,7,nullptr,1);
    xTaskCreatePinnedToCore(command_task,"commands",4096,nullptr,7,nullptr,1);
    xTaskCreatePinnedToCore(worker_task,"dsp",24576,nullptr,6,nullptr,1);

}
