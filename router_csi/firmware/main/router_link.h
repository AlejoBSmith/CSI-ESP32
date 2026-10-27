#pragma once
#include "esp_wifi.h"
#include "cJSON.h"
#include <cstdint>
namespace RouterLink {
void init();
void tick(bool enabled);
bool configure(const cJSON* command,const char*& error);
bool configureTraffic(const cJSON* command,const char*& error);
bool forget();
bool ready();
bool useHT();
bool accept(const wifi_csi_info_t* info);
uint32_t session();
const char* reason();
void appendStatus(cJSON* root);
}
