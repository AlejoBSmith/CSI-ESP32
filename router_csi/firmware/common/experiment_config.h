#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>

// PARAMETROS DEL EXPERIMENTO: editar aqui, al principio de este archivo.
// Config puede cambiarse por JSON sin recompilar. Los valores guardados en NVS
// prevalecen sobre estos defaults. {"cmd":"defaults"} aplica y guarda estos valores.
// Cambiar el DSP programa otra referencia de amplitud; el AP determina el canal.
struct Config {
    uint32_t version = 7;           // Esquema NVS: no editar.
    uint32_t channel = 6;          // Informativo: se adopta el canal real del AP (1..13).
    uint32_t tx_hz = 100;           // Campo reservado; usar ping_hz para las solicitudes al AP.
    uint32_t window = 200;          // Ventana lenta: 200 slots = 2 s.
    uint32_t hop = 10;             // Salida cada 100 ms; no altera los 100 slots/s.
    uint32_t top_k = 12;           // Mediana de las K subportadoras con mayor variacion coherente.
    uint32_t enter_hops = 2;        // Ventanas consecutivas para entrar en ACTIVE.
    uint32_t exit_hops = 3;         // Ventanas consecutivas para volver a CLEAR.
    uint32_t hampel_window = 7;     // Impar, 3..15, a la tasa CSI nativa.
    float hampel_sigma = 5;         // Umbral Hampel en desviaciones robustas.
    float cutoff = 0;              // Hz: low-pass opcional sobre CV; 0 lo desactiva.
    float min_fs = 50;             // Minima tasa CSI real admisible; independiente del anillo.
    float enter_score = 4;              // Umbral de entrada (score de variacion de forma coherente; no probabilidad).
    float exit_score = 2.5f;            // Umbral de salida; menor que enter_score.
    uint32_t profile = 0;           // Solo 0 (HT20/LLTF) admitido en esta variante.
    float coverage_fraction = .80f; // Fraccion de subportadoras utilizables (0.1..1).
    // Campos v6 anexados: mantener el prefijo para migrar NVS v4/v5.
    uint32_t fast_window = 25;     // Ventana rapida: 250 ms. Debe ser <= window.
    uint32_t detector_kind = 2;    // 2 = banco; 1 = fusion dual anterior; 0 = CV fijo.
    uint32_t carrier_mode = 1;     // 0 = 12 promedios fijos; 1 = todas utiles; 2 = todas con media vecina.
    float fast_weight = .65f;      // Peso de la rapida cuando no la respalda la lenta (0..1).

    // Banco logaritmico entre fast_window y window; 100 slots equivalen a 1 s.
    uint32_t scale_count = 7;      // 2..8 periodos; se calculan todos con el mismo timestamp.
    uint32_t consensus_scales = 2; // Ventanas vecinas que deben apoyar las mismas subportadoras.
    uint32_t confirm_hops = 2;     // Mediana causal de 2*K-1 salidas: K=2 elimina picos de una salida.
};

// Ajustes que requieren recompilar y cargar firmware. Unidades explicitas.
namespace Experiment {
// Standalone user interface. BOOT is active-low; the user LED is GPIO21.
constexpr bool auto_calibrate = true;
constexpr uint64_t calibration_delay_us = 20000000;
constexpr uint64_t button_hold_us = 1500000;
constexpr float led_full_scale = 1.f;        // Minimum LED peak (relative amplitude %); higher peaks remap PWM.
constexpr int led_gpio = 21;
constexpr int button_gpio = 0;
constexpr uint64_t telemetry_lease_us = 5000000; // Renewed only by PC commands.
constexpr float output_hz = 100;              // Maxima tasa del anillo filtrado.
constexpr float amplitude_presence = .95f;    // Presencia positiva durante seleccion.
constexpr uint64_t amplitude_cal_us = 15000000;
constexpr uint64_t rate_interval_us = 1000000;
constexpr float rate_change_fraction = .20f;
constexpr uint64_t max_sample_gap_us = 250000;
constexpr uint64_t no_csi_timeout_us = 2000000;
constexpr float hampel_min_scale = 1e-4f;
constexpr float amplitude_epsilon = 1e-6f;
constexpr float feature_epsilon = 1e-6f;
constexpr unsigned gain_warmup_frames = 100;
constexpr unsigned max_window = 1600;         // Capacidad de memoria, no ventana activa.
constexpr unsigned amplitude_reservoir = 512;
constexpr float sample_coverage_fraction=.70f; // Fraccion temporal real minima en cada ventana.
constexpr uint64_t max_interpolation_gap_us = 0; // No se interpolan muestras del detector.
constexpr uint64_t calibration_max_gap_us = 50000;
constexpr float autocorr_noise_sigma=2.f; // Margen de incertidumbre ACF, sin aprender actividad.
constexpr uint64_t shape_lag_us=100000; // Diagnostico de desplazamiento de forma a 100 ms.
constexpr unsigned rx_queue_frames = 256;
constexpr unsigned telemetry_queue_messages = 128;
constexpr uint64_t worker_yield_us = 20000;
}
// Router settings live in a separate NVS namespace. No ESP-NOW config migration.
inline bool upgradeConfig(Config& value,size_t stored_size){
    const size_t v5_size=offsetof(Config,fast_window),v6_size=offsetof(Config,scale_count);
    if(stored_size==v5_size&&(value.version==4||value.version==5)){
        if(value.version==4){if(value.window==50)value.window=100;if(value.cutoff==10)value.cutoff=0;value.top_k=12;}
        if(value.window==100)value.window=200;
        if(value.hop==25)value.hop=10;
        value.fast_window=std::min(value.fast_window,value.window);
        value.detector_kind=value.window>=value.fast_window+value.scale_count-1?2:1;
        value.version=7;return true;
    }
    if(stored_size==v6_size&&value.version==6){
        if(value.detector_kind==1&&value.window>=value.fast_window+value.scale_count-1)value.detector_kind=2;
        value.version=7;return true;
    }
    return stored_size==sizeof(Config)&&value.version==7;
}
namespace RouterDefaults {
constexpr bool arp_keepalive=false; // Experimental: no mejoro de forma sostenida con este AP.
constexpr uint64_t arp_refresh_us=30000000; // Una solicitud ARP cada 30 s; diagnostico cada 1 s.
constexpr unsigned max_pending_ping=8; // Limite de presion sobre buffers de red.
constexpr uint64_t ping_backoff_us=20000; // Pausa de envio tras ENOMEM/ENOBUFS/EAGAIN; RX continua.
constexpr bool asynchronous_ping=false; // Conservador: async queda disponible para comparar, sin ganancia sostenida demostrada.
constexpr bool ampdu_rx=false;          // Paquetes individuales; no se necesita agregacion para 100 pings/s.
constexpr bool fixed_rx_gain=false;     // Evita saltos AGC entre paquetes; comprobar cobertura con el AP.
constexpr unsigned csi_shift=1;         // Escala CSI fija; evita autoescala no compensada por paquete.
constexpr const char* ssid="CSI_LAB";
constexpr const char* target="192.168.50.1";
constexpr unsigned ping_hz=100;          // Solicitudes/s, no garantia de tasa CSI.
constexpr unsigned ping_timeout_ms=100;
constexpr unsigned reconnect_delay_ms=3000;
}
