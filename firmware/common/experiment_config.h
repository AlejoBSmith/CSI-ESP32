#pragma once
#include <cstdint>
#include <cstddef>

// PARAMETROS DEL EXPERIMENTO: editar aqui, al principio de este archivo.
// Config puede cambiarse por JSON sin recompilar. Los valores guardados en NVS
// prevalecen sobre estos defaults. {"cmd":"defaults"} aplica y guarda estos valores.
// Tras cambiar el DSP hay que calibrar de nuevo; canal/perfil reinician la placa.
struct Config {
    uint32_t version = 3;           // Esquema NVS: no editar.
    uint32_t channel = 11;          // Mismo canal en TX, RX1 y RX2 (1..11).
    uint32_t tx_hz = 500;           // TX: 100, 250, 500 u 800 paquetes/s.
    uint32_t window = 125;          // Muestras filtradas por ventana (~0.5 s a 250 Hz).
    uint32_t hop = 62;             // Separacion entre resultados (~0.248 s).
    uint32_t top_k = 5;             // Mediana de las K subportadoras con mayor variacion relativa.
    uint32_t enter_hops = 2;        // Ventanas consecutivas para entrar en ACTIVE.
    uint32_t exit_hops = 3;         // Ventanas consecutivas para volver a CLEAR.
    uint32_t hampel_window = 7;     // Impar, 3..15, a la tasa CSI nativa.
    float hampel_sigma = 5;         // Umbral Hampel en desviaciones robustas.
    float cutoff = 10;              // Hz: Butterworth de orden 4.
    float min_fs = 250;             // Minima tasa CSI recibida admisible, Hz.
    float enter_score = 4;              // Umbral de entrada (porcentaje de variacion, no probabilidad).
    float exit_score = 2.5f;            // Umbral de salida; menor que enter_score.
    uint32_t profile = 0;           // 0=HT20 detector; 1=HT40 captura experimental.
    float coverage_fraction = .80f; // Fraccion de subportadoras utilizables (0.1..1).
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
constexpr float output_hz = 250;              // Maxima tasa del anillo filtrado.
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
constexpr uint64_t max_interpolation_gap_us = 50000; // Upper bound; also capped at half a cutoff period.
constexpr unsigned rx_queue_frames = 256;
constexpr unsigned telemetry_queue_messages = 128;
constexpr uint64_t worker_yield_us = 20000;
}
// v3 changes score units and defaults. Keep radio/quality choices but migrate
// the old 800/400 timing and z-score thresholds exactly once.
inline bool upgradeConfig(Config& value,size_t stored_size){
    if(value.version==1&&stored_size==offsetof(Config,coverage_fraction)){
        value.version=2;value.coverage_fraction=Config{}.coverage_fraction;
        stored_size=sizeof(Config);
    }
    if(value.version==2&&stored_size==sizeof(Config)){
        value.version=3;value.window=Config{}.window;value.hop=Config{}.hop;
        value.enter_score=Config{}.enter_score;value.exit_score=Config{}.exit_score;
    }
    return value.version==3&&stored_size==sizeof(Config);
}
