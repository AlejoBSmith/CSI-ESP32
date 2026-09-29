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
#include "esp_now.h"
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
static RawMeta last_meta{};
static uint64_t dsp_total_us=0,dsp_frames=0;
static uint32_t dsp_max_us=0;
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
    FIELD(channel);FIELD(tx_hz);FIELD(window);FIELD(hop);FIELD(top_k);FIELD(enter_hops);FIELD(exit_hops);FIELD(hampel_window);FIELD(hampel_sigma);FIELD(cutoff);FIELD(min_fs);FIELD(enter_score);FIELD(exit_score);FIELD(profile);FIELD(coverage_fraction);
#undef FIELD
    return j;
}
static bool valid(const Config& c){return c.channel>=1&&c.channel<=11&&(c.tx_hz==100||c.tx_hz==250||c.tx_hz==500||c.tx_hz==800)&&c.window>=64&&c.window<=RatDetector::MAX_WINDOW&&c.hop>0&&c.hop<=c.window&&c.top_k>0&&c.top_k<=32&&c.enter_hops>0&&c.enter_hops<=20&&c.exit_hops>0&&c.exit_hops<=20&&c.hampel_window>=3&&c.hampel_window<=15&&(c.hampel_window&1)&&std::isfinite(c.enter_score)&&std::isfinite(c.exit_score)&&c.enter_score>c.exit_score&&c.exit_score>=0&&std::isfinite(c.min_fs)&&c.min_fs>=25&&c.min_fs<=1000&&c.cutoff>0&&c.cutoff<c.min_fs/2&&std::isfinite(c.hampel_sigma)&&c.hampel_sigma>0&&c.profile<=1&&std::isfinite(c.coverage_fraction)&&c.coverage_fraction>=.1f&&c.coverage_fraction<=1;}
static void status(){
    if(!telemetry_active())return;
    auto*j=cJSON_CreateObject();number(j,"v",2);number(j,"node",RAT_NODE_ID);number(j,"boot",boot_id);
    cJSON_AddStringToObject(j,"device_id",device_id);number(j,"led_brightness",led_level/1023.0);
    number(j,"calibration_wait_s",calibration_due?std::max<int64_t>(0,int64_t(calibration_due)-esp_timer_get_time()+999999)/1000000:0);
    cJSON_AddStringToObject(j,"role",RAT_TX?"TX":"RX");cJSON_AddStringToObject(j,"mode","MODE_RAT_EDGE");
    cJSON_AddStringToObject(j,"state",RAT_TX?(enabled?"TRANSMITTING":"STOPPED"):detector->state);
    cJSON_AddStringToObject(j,"reason",RAT_TX?"TX_ONLY":detector->reason);
    number(j,"rx_us",esp_timer_get_time());number(j,"rx_count",rx_count);number(j,"rejected",rejected);
    number(j,"queue_drops",queue_drops);number(j,"telemetry_drops",telemetry_drops);
    number(j,"tx_count",tx_count);number(j,"tx_errors",tx_errors);number(j,"commanded_hz",tx_hz);
    number(j,"effective_fs",detector->effective_fs);number(j,"packet_loss",detector->loss_fraction);
    number(j,"score",detector->score);cJSON_AddBoolToObject(j,"measurement_valid",detector->measurement_valid);
    cJSON_AddBoolToObject(j,"amplitude_calibrated",detector->calibrated);number(j,"calibration_progress",detector->calibration_progress);
    cJSON_AddStringToObject(j,"score_units","relative_amplitude_percent");number(j,"led_peak",led_scale.peak);
    cJSON_AddStringToObject(j,"last_recovery",detector->last_recovery);number(j,"recovery_count",detector->recovery_count);number(j,"selected",detector->selected);number(j,"votes",detector->votes);
    number(j,"usable_carriers",detector->usable);number(j,"required_carriers",detector->required_carriers);
    number(j,"missing_values",detector->missing_values);number(j,"missing_now",detector->missing_now);
    number(j,"seq0",detector->seq0);number(j,"seq1",detector->seq1);number(j,"tx_boot",last_meta.tx_boot);
    number(j,"reference_id",detector->reference_id);number(j,"rssi",last_meta.rssi);number(j,"noise_floor",last_meta.noise_floor);
    number(j,"gain",last_meta.gain);number(j,"csi_len",last_meta.csi_len);
    number(j,"dsp_mean_us",dsp_frames?double(dsp_total_us)/dsp_frames:0);number(j,"dsp_max_us",dsp_max_us);
    number(j,"queue_pending",uxQueueMessagesWaiting(frames));
    char elf_hash[65];const auto* app=esp_app_get_description();for(unsigned i=0;i<32;++i)snprintf(elf_hash+i*2,3,"%02x",app->app_elf_sha256[i]);cJSON_AddStringToObject(j,"elf_sha256",elf_hash);
    cJSON_AddStringToObject(j,"idf",esp_get_idf_version());cJSON_AddStringToObject(j,"commit",RAT_COMMIT);
    cJSON_AddStringToObject(j,"source_sha256",RAT_SOURCE_HASH);cJSON_AddStringToObject(j,"esp_csi_commit","8633d67152db2808f141cc1595970aa9cf406045");
    cJSON_AddStringToObject(j,"model","ESP32-S3");cJSON_AddStringToObject(j,"gain_component","0.1.4");
    cJSON_AddStringToObject(j,"board","Seeed Studio XIAO ESP32S3");cJSON_AddStringToObject(j,"transport","USB Serial/JTAG");
    cJSON_AddStringToObject(j,"tx_mac","1a:00:00:00:00:00");
    int8_t power=0;esp_wifi_get_max_tx_power(&power);number(j,"tx_power_quarter_dbm",power);
    cJSON_AddItemToObject(j,"config",configuration());
    auto*tuning=cJSON_AddObjectToObject(j,"tuning");
#define TUNE(k) number(tuning,#k,Experiment::k)
    TUNE(output_hz);TUNE(amplitude_presence);TUNE(amplitude_cal_us);
    TUNE(rate_interval_us);TUNE(rate_change_fraction);
    TUNE(max_sample_gap_us);TUNE(max_interpolation_gap_us);TUNE(no_csi_timeout_us);TUNE(hampel_min_scale);
    TUNE(amplitude_epsilon);TUNE(feature_epsilon);TUNE(gain_warmup_frames);
    TUNE(max_window);TUNE(amplitude_reservoir);
    TUNE(rx_queue_frames);TUNE(telemetry_queue_messages);TUNE(worker_yield_us);
#undef TUNE
    auto* top=cJSON_AddArrayToObject(j,"top");for(unsigned i=0;i<detector->top_count;++i)cJSON_AddItemToArray(top,cJSON_CreateNumber(detector->top[i]));
    send_json(2,j);
}
// IDF 5.5 supplies the action body. All offsets are centralized and checked;
// never borrow the last ESP-NOW sequence for an unrelated CSI frame.
static bool parse_probe_sequence(const wifi_csi_info_t* info,Probe& probe){
    const uint8_t*p=info->payload;
    constexpr size_t offset=15;
    if(!p||info->payload_len<offset+sizeof(Probe)||info->rx_ctrl.sig_len<24+offset+sizeof(Probe)+4)return false;
    if(p[0]!=127||p[1]!=0x18||p[2]!=0xfe||p[3]!=0x34||p[8]!=221||p[9]!=5+sizeof(Probe)||p[10]!=0x18||p[11]!=0xfe||p[12]!=0x34||p[13]!=4||((p[14]&15)!=1&&(p[14]&15)!=2))return false;
    memcpy(&probe,p+offset,sizeof(probe));return probe.magic==RAT_MAGIC;
}
static void csi_callback(void*,wifi_csi_info_t*info){
    if(!enabled||!info||!info->buf)return;
    if(memcmp(info->mac,TX_MAC,6)){++rejected;return;}
    Probe probe{};
    if(info->len>MAX_CSI_BYTES||!parse_probe_sequence(info,probe)){++rejected;return;}
    Frame f;auto&m=f.meta;const auto&r=info->rx_ctrl;
    m.node=RAT_NODE_ID;m.flags=info->first_word_invalid?1:0;m.csi_len=info->len;
    m.tx_seq=probe.seq;m.tx_boot=probe.boot;m.rx_boot=boot_id;m.rx_us=esp_timer_get_time();m.radio_us=r.timestamp;
    m.rssi=r.rssi;m.noise_floor=r.noise_floor;m.channel=r.channel;m.bandwidth=r.cwb;m.sig_mode=r.sig_mode;m.mcs=r.mcs;m.stbc=r.stbc;m.rate=r.rate;
    esp_csi_gain_ctrl_get_rx_gain(&r,&m.agc,&m.fft);
    memcpy(m.source,info->mac,6);memcpy(f.iq,info->buf,info->len);
    ++rx_count;
    if(xQueueSend(frames,&f,0)!=pdTRUE)++queue_drops;
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
    auto*j=cJSON_CreateObject();number(j,"node",RAT_NODE_ID);number(j,"boot",boot_id);number(j,"rx_us",last_meta.rx_us);number(j,"seq0",detector->seq0);number(j,"seq1",detector->seq1);
    number(j,"reference_id",detector->reference_id);number(j,"effective_fs",detector->effective_fs);number(j,"filter_fs",detector->filter_fs);number(j,"score",detector->score);cJSON_AddBoolToObject(j,"measurement_valid",detector->measurement_valid);
    cJSON_AddBoolToObject(j,"amplitude_calibrated",detector->calibrated);number(j,"calibration_progress",detector->calibration_progress);
    cJSON_AddStringToObject(j,"score_units","relative_amplitude_percent");number(j,"led_peak",led_scale.peak);
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
    Config c;ActivityFSM f;f.update(4,c);bool ok=!f.active;f.update(4,c);ok&=f.active;f.update(2,c);f.update(2,c);ok&=f.active;f.update(2,c);ok&=!f.active;
    Biquad b1,b2;b1.configure(250,10,.5411961f);b2.configure(250,10,1.306563f);float y=0;for(int i=0;i<2000;++i)y=b2.add(b1.add(1));ok&=std::fabs(y-1)<1e-4;
    uint8_t payload[27]={127,0x18,0xfe,0x34,0,0,0,0,221,17,0x18,0xfe,0x34,4,1};
    Probe expected{RAT_MAGIC,1234,5678},parsed{};memcpy(payload+15,&expected,sizeof(expected));
    wifi_csi_info_t info{};info.payload=payload;info.payload_len=sizeof(payload);info.rx_ctrl.sig_len=55;
    ok&=parse_probe_sequence(&info,parsed)&&parsed.seq==1234&&parsed.boot==5678;
    info.payload_len=20;ok&=!parse_probe_sequence(&info,parsed);
    void* memory=heap_caps_malloc(sizeof(RatDetector),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    bool cal_ok=false,active_seen=false,clear_seen=false;
    if(memory){
        auto* test=new(memory)RatDetector;test->startCalibration();Frame frame{};
        frame.meta.node=RAT_NODE_ID;frame.meta.flags=2;frame.meta.gain=1;frame.meta.csi_len=128;frame.meta.tx_boot=99;frame.meta.sig_mode=1;frame.meta.mcs=0;frame.meta.channel=test->config.channel;
        // Virtual 500 Hz RF, decimated to 250 Hz. No samples enter the live
        // detector or raw stream; this tests the embedded numerical pipeline.
        for(unsigned i=0;i<46000;++i){
            float seconds=i/500.f;bool movement=seconds>1&&seconds<78;
            for(unsigned k=0;k<64;++k){frame.iq[2*k]=0;frame.iq[2*k+1]=int8_t(30+(movement?8*std::sin(4*3.14159265f*seconds):0));}
            if(movement&&i%20==0)frame.iq[5]=0; // Repeated zero in one calibrated carrier.
            frame.meta.tx_seq=i;frame.meta.rx_us=1000000+uint64_t(i)*2000;test->add(frame);
            if(seconds>69&&seconds<74)ok&=!strcmp(test->state,"ACTIVE")&&test->usable>=test->required_carriers;
            cal_ok|=test->calibrated;active_seen|=test->fsm.active;
            if(seconds>85&&!strcmp(test->state,"CLEAR"))clear_seen=true;
            if(i%500==0)vTaskDelay(1);
        }
        // A failed frame must never request retransmission of stale features.
        ++frame.meta.tx_seq;frame.meta.rx_us+=2000;++frame.meta.queue_drops;
        test->add(frame);ok&=!test->emit&&!strcmp(test->state,"INVALID");
        ++frame.meta.tx_seq;frame.meta.rx_us+=2000;frame.meta.flags=0;
        test->add(frame);ok&=!test->emit&&!strcmp(test->state,"INVALID");
        test->idle(frame.meta.rx_us+3000000);ok&=!strcmp(test->state,"INVALID")&&!test->emit;
        test->~RatDetector();free(memory);
    }
    auto*j=cJSON_CreateObject();cJSON_AddStringToObject(j,"kind","SELFTEST");
    cJSON_AddBoolToObject(j,"fsm_filter_parser",ok);cJSON_AddBoolToObject(j,"calibration",cal_ok);cJSON_AddBoolToObject(j,"active",active_seen);cJSON_AddBoolToObject(j,"clear",clear_seen);
    cJSON_AddStringToObject(j,"result",ok&&cal_ok&&active_seen&&clear_seen?"PASS":"FAIL");cJSON_AddStringToObject(j,"evidence","synthetic numerical test; not RF or rat validation");send_json(5,j);
}
static void handle_command(const Command& c){
    telemetry_until=esp_timer_get_time()+Experiment::telemetry_lease_us;
    auto*j=cJSON_Parse(c.line);if(!j){message("ERROR","Invalid JSON");return;}
    auto*cmd=cJSON_GetObjectItem(j,"cmd");const char*s=cJSON_IsString(cmd)?cmd->valuestring:"";
    if(!strcmp(s,"status"))status();
    else if(!strcmp(s,"calibrate")){if(RAT_TX)message("ERROR","Calibration requires RX");else{calibration_due=0;detector->startCalibration();message("OK","Amplitude reference started; no activity baseline is learned");}}
    else if(!strcmp(s,"calibrate_after_delay")){if(RAT_TX)message("ERROR","Calibration requires RX");else{request_calibration();message("OK","Calibration begins in 20 seconds");}}
    else if(!strcmp(s,"cancel_calibration")){calibration_due=0;detector->reset("CALIBRATION_CANCELLED");message("OK","Calibration cancelled");}
    else if(!strcmp(s,"selftest")){bool was_enabled=enabled;calibration_due=0;enabled=false;detector->reset("SELFTEST_MAINTENANCE");selftest();xQueueReset(frames);detector->reset();enabled=was_enabled;}
    else if(!strcmp(s,"raw")){raw_enabled=cJSON_IsTrue(cJSON_GetObjectItem(j,"enabled"));message("OK",raw_enabled?"raw enabled":"raw disabled");}
    else if(!strcmp(s,"stop")){calibration_due=0;enabled=false;detector->reset("STOPPED");message("OK","Stopped");}
    else if(!strcmp(s,"start")){xQueueReset(frames);enabled=true;if(!RAT_TX)request_calibration();message("OK","Started");}
    else if(!strcmp(s,"config")||!strcmp(s,"defaults")){
        Config next=!strcmp(s,"defaults")?Config{}:config;bool ok=true;
        cJSON* item=nullptr;cJSON_ArrayForEach(item,j){if(!strcmp(item->string,"cmd"))continue;if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)){ok=false;break;}const double v=item->valuedouble;bool found=false;
#define UINT_FIELD(k) if(!strcmp(item->string,#k)){found=true;if(v<0||v>100000||v!=floor(v))ok=false;else next.k=(uint32_t)v;}
#define FLOAT_FIELD(k) if(!strcmp(item->string,#k)){found=true;next.k=v;}
            UINT_FIELD(channel);UINT_FIELD(tx_hz);UINT_FIELD(window);UINT_FIELD(hop);UINT_FIELD(top_k);UINT_FIELD(enter_hops);UINT_FIELD(exit_hops);UINT_FIELD(hampel_window);UINT_FIELD(profile);
            FLOAT_FIELD(hampel_sigma);FLOAT_FIELD(cutoff);FLOAT_FIELD(min_fs);FLOAT_FIELD(enter_score);FLOAT_FIELD(exit_score);FLOAT_FIELD(coverage_fraction);
#undef UINT_FIELD
#undef FLOAT_FIELD
            if(!found)ok=false;
        }
        if(!ok||!valid(next))message("ERROR","Invalid config; unchanged");
        else{
            bool reboot=next.channel!=config.channel||next.profile!=config.profile;
            nvs_handle_t n;if(nvs_open("rat-csi",NVS_READWRITE,&n)==ESP_OK){esp_err_t e=nvs_set_blob(n,"config",&next,sizeof(next));if(e==ESP_OK)e=nvs_commit(n);nvs_close(n);if(e!=ESP_OK){message("ERROR","NVS save failed");cJSON_Delete(j);return;}}
            else{message("ERROR","NVS open failed");cJSON_Delete(j);return;}
            calibration_due=0;config=next;tx_hz=next.tx_hz;detector->config=next;detector->reset("CONFIG_CHANGED");message("OK",reboot?"Saved; rebooting for radio change":"Saved; recalibrate RX");
            if(reboot){enabled=false;vTaskDelay(pdMS_TO_TICKS(300));esp_restart();}
        }
    }
    else if(!strcmp(s,"scan")){
        calibration_due=0;enabled=false;detector->reset("SCAN_MAINTENANCE");esp_wifi_set_promiscuous(false);
        wifi_scan_config_t scan{};scan.show_hidden=true;scan.scan_type=WIFI_SCAN_TYPE_PASSIVE;scan.scan_time.passive=120;
        if(esp_wifi_scan_start(&scan,true)==ESP_OK){uint16_t n=32;wifi_ap_record_t aps[32];esp_wifi_scan_get_ap_records(&n,aps);auto*r=cJSON_CreateObject();auto*a=cJSON_AddArrayToObject(r,"aps");for(int i=0;i<n;++i){auto*b=cJSON_CreateObject();number(b,"channel",aps[i].primary);number(b,"rssi",aps[i].rssi);cJSON_AddItemToArray(a,b);}send_json(7,r);}else message("ERROR","Scan failed");
        esp_wifi_set_channel(config.channel,config.profile?WIFI_SECOND_CHAN_BELOW:WIFI_SECOND_CHAN_NONE);esp_wifi_set_promiscuous(!RAT_TX);xQueueReset(frames);enabled=true;
    }
    else message("ERROR","Commands: status, config, defaults, calibrate, calibrate_after_delay, cancel_calibration, raw, stop, start, scan, selftest");
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
    led_level=activityBrightness(detector->score,detector->state,calibration_due!=0,RAT_TX,enabled,now,led_scale.peak);
    ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,led_level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
}
static void worker_task(void*){
    Frame f;Command c;uint64_t last_status=0,last_yield=0;unsigned previous_ref=0;
    while(true){
        while(xQueueReceive(commands,&c,0)==pdTRUE)handle_command(c);
        if(!RAT_TX&&xQueueReceive(frames,&f,pdMS_TO_TICKS(10))==pdTRUE){
            if(gain_frames<Experiment::gain_warmup_frames)esp_csi_gain_ctrl_record_rx_gain(f.meta.agc,f.meta.fft);
            else if(gain_frames==Experiment::gain_warmup_frames){uint8_t a;int8_t b;esp_csi_gain_ctrl_get_rx_gain_baseline(&a,&b);}
            if(gain_frames++>=Experiment::gain_warmup_frames){float gain=1;esp_err_t err=esp_csi_gain_ctrl_get_gain_compensation(&gain,f.meta.agc,f.meta.fft);f.meta.gain=gain;if(err==ESP_OK&&std::isfinite(gain)&&gain>0)f.meta.flags|=2;}else f.meta.gain=1;
            f.meta.queue_drops=queue_drops;f.meta.telemetry_drops=telemetry_drops;last_meta=f.meta;
            if(raw_enabled)send_message(1,&f,sizeof(RawMeta)+f.meta.csi_len);
            if(enabled){uint64_t began=esp_timer_get_time();detector->add(f);if(detector->emit&&detector->selected)feature_event();if(detector->reference_id!=previous_ref){led_scale.reset();calibration_event();previous_ref=detector->reference_id;}uint32_t elapsed=esp_timer_get_time()-began;dsp_total_us+=elapsed;++dsp_frames;dsp_max_us=std::max(dsp_max_us,elapsed);}
        }else vTaskDelay(1);
        uint64_t now=esp_timer_get_time();update_indicator(now);if(now-last_status>1000000){last_status=now;if(!RAT_TX)detector->idle(now);status();}
        // A burst must not starve IDLE1/watchdog while draining the RX queue.
        if(now-last_yield>=Experiment::worker_yield_us){vTaskDelay(1);last_yield=esp_timer_get_time();}
    }
}
static void tx_task(void*){
    const uint8_t broadcast[6]={255,255,255,255,255,255};int64_t next=esp_timer_get_time();
    while(true){if(enabled){Probe p{RAT_MAGIC,tx_seq++,boot_id};if(esp_now_send(broadcast,(uint8_t*)&p,sizeof(p))!=ESP_OK)++tx_errors;else ++tx_count;}
        next+=1000000/tx_hz;int64_t wait=next-esp_timer_get_time();if(wait>0)usleep(wait);else{next=esp_timer_get_time();vTaskDelay(1);}
    }
}
extern "C" void app_main(){
    ESP_ERROR_CHECK(nvs_flash_init()); // Preserve unrelated WiFi credentials; never erase NVS automatically.
    nvs_handle_t n;if(nvs_open("rat-csi",NVS_READONLY,&n)==ESP_OK){Config saved;size_t size=sizeof(saved);if(nvs_get_blob(n,"config",&saved,&size)==ESP_OK&&upgradeConfig(saved,size)&&valid(saved))config=saved;nvs_close(n);}
    boot_id=esp_random();tx_hz=config.tx_hz;
    void* memory=heap_caps_malloc(sizeof(RatDetector),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(memory);detector=new(memory)RatDetector;detector->config=config;
    auto* frame_storage=(uint8_t*)heap_caps_malloc(Experiment::rx_queue_frames*sizeof(Frame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(frame_storage);
    frames=xQueueCreateStatic(Experiment::rx_queue_frames,sizeof(Frame),frame_storage,&frame_queue_control);telemetry=xQueueCreate(Experiment::telemetry_queue_messages,sizeof(Message));commands=xQueueCreate(8,sizeof(Command));assert(frames&&telemetry&&commands);
    usb_serial_jtag_driver_config_t usb{};usb.rx_buffer_size=4096;usb.tx_buffer_size=16384;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    uint8_t mac[6];ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    snprintf(device_id,sizeof(device_id),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    init_indicator();

    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&wifi));ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA,config.profile?WIFI_BW_HT40:WIFI_BW_HT20));
    if(RAT_TX)ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA,TX_MAC));
    ESP_ERROR_CHECK(esp_wifi_start());ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));ESP_ERROR_CHECK(esp_wifi_set_channel(config.channel,config.profile?WIFI_SECOND_CHAN_BELOW:WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());esp_now_peer_info_t peer{};memset(peer.peer_addr,255,6);peer.channel=config.channel;peer.ifidx=WIFI_IF_STA;ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    esp_now_rate_config_t rate{};rate.phymode=config.profile?WIFI_PHY_MODE_HT40:WIFI_PHY_MODE_HT20;rate.rate=WIFI_PHY_RATE_MCS0_LGI;ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr,&rate));
    if(!RAT_TX){ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));wifi_csi_config_t c{};c.lltf_en=c.htltf_en=c.stbc_htltf2_en=c.ltf_merge_en=c.channel_filter_en=true;c.manu_scale=false;c.shift=0;
        ESP_ERROR_CHECK(esp_wifi_set_csi_config(&c));ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_callback,nullptr));ESP_ERROR_CHECK(esp_wifi_set_csi(true));}
    if(!RAT_TX&&Experiment::auto_calibrate)request_calibration();
    xTaskCreatePinnedToCore(telemetry_task,"usb",4096,nullptr,7,nullptr,1);
    xTaskCreatePinnedToCore(command_task,"commands",4096,nullptr,7,nullptr,1);
    xTaskCreatePinnedToCore(worker_task,"dsp",24576,nullptr,6,nullptr,1);
    if(RAT_TX)xTaskCreatePinnedToCore(tx_task,"tx",4096,nullptr,10,nullptr,1);
}
