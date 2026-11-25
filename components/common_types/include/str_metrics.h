#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Acumulador de métricas STR por software para cada nodo. */
typedef struct {
    const char *node_tag;
    uint32_t max_crit_latency_us;
    uint64_t jitter_sum_us;
    uint32_t jitter_count;
    uint16_t deadline_misses;
    portMUX_TYPE lock;
} str_metrics_ctx_t;

/**
 * @brief Configura (o reconfigura) el Task Watchdog Timer global.
 */
esp_err_t str_twdt_init(uint32_t timeout_ms);

/**
 * @brief Inicializa el contexto de métricas de tiempo real.
 */
void str_metrics_init(str_metrics_ctx_t *ctx, const char *node_tag);

/**
 * @brief Registra una medición en µs, actualiza máximos/misses e imprime línea [STR_METRIC].
 */
void str_metrics_record(str_metrics_ctx_t *ctx,
                        const char *metric_name,
                        uint32_t value_us,
                        uint32_t deadline_us,
                        bool is_jitter);

/**
 * @brief Obtiene una instantánea atómica de los contadores STR acumulados.
 */
void str_metrics_snapshot(str_metrics_ctx_t *ctx,
                          uint32_t *out_max_lat_us,
                          uint32_t *out_avg_jitter_us,
                          uint16_t *out_misses);

/**
 * @brief Imprime estado de salud: uptime, heap libre/mínimo, watermarks de stack y %CPU.
 */
void str_metrics_log_health(str_metrics_ctx_t *ctx,
                            const TaskHandle_t *handles,
                            const char *const *names,
                            size_t count);

#ifdef __cplusplus
}
#endif
