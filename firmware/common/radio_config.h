#pragma once
#include <cstdint>
#include "experiment_config.h"
constexpr uint32_t RAT_MAGIC = 0x52415431;
constexpr unsigned MAX_CSI_BYTES = 612;
constexpr unsigned MAX_CARRIERS = MAX_CSI_BYTES/2;
constexpr uint8_t TX_MAC[6] = {0x1a,0,0,0,0,0};
constexpr uint32_t SERIAL_BAUD = 3000000;
enum RadioProfile { RADIO_PROFILE_RAT_HT20, RADIO_PROFILE_ESPR_GENERIC };
enum DetectorMode { MODE_RAT_EDGE, MODE_WIHUNTER_REPLICATION, DETECTOR_LEGACY };

#ifdef _MSC_VER
#define RAT_PACKED
#pragma pack(push,1)
#else
#define RAT_PACKED __attribute__((packed))
#endif
struct RAT_PACKED Probe { uint32_t magic, seq, boot; };
// Framed USB stream: header, payload, CRC32(header+payload). All little-endian.
struct RAT_PACKED WireHeader {
    uint32_t magic = 0x31544152; // bytes RAT1
    uint8_t version = 1, type = 0;
    uint16_t length = 0;
};
struct RAT_PACKED RawMeta {
    uint8_t node, flags;
    uint16_t csi_len;
    uint32_t tx_seq, tx_boot, rx_boot;
    uint64_t rx_us;
    uint32_t radio_us, queue_drops, telemetry_drops;
    int8_t rssi, noise_floor;
    uint8_t channel, bandwidth, sig_mode, mcs, stbc, rate;
    uint8_t agc;
    int8_t fft;
    float gain;
    uint8_t source[6];
};
struct Frame { RawMeta meta{}; int8_t iq[MAX_CSI_BYTES]{}; };
#ifdef _MSC_VER
#pragma pack(pop)
#endif
#undef RAT_PACKED
static_assert(sizeof(RawMeta)==56,"USB raw metadata layout changed");
