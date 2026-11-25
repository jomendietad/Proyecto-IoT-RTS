#include "str_metrics.h"

#include <inttypes.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

static const char *TAG = "str_metrics";

esp_err_t str_twdt_init(uint32_t timeout_ms)
{
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = (1U << 0),
        .trigger_panic = true,
    };

    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&cfg);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TWDT configurado a %" PRIu32 " ms (panic=on)", timeout_ms);
    } else {
        ESP_LOGW(TAG, "Error configurando TWDT: %s", esp_err_to_name(err));
    }
    return err;
}

void str_metrics_init(str_metrics_ctx_t *ctx, const char *node_tag)
{
    if (!ctx) {
        return;
    }
    ctx->node_tag = node_tag ? node_tag : "NODE";
    ctx->max_crit_latency_us = 0;
    ctx->jitter_sum_us = 0;
    ctx->jitter_count = 0;
    ctx->deadline_misses = 0;
    portMUX_INITIALIZE(&ctx->lock);
}

void str_metrics_record(str_metrics_ctx_t *ctx,
                        const char *metric_name,
                        uint32_t value_us,
                        uint32_t deadline_us,
                        bool is_jitter)
{
    if (!ctx || !metric_name) {
        return;
    }

    const unsigned int miss = (deadline_us > 0U && value_us > deadline_us) ? 1U : 0U;

    portENTER_CRITICAL(&ctx->lock);
    if (is_jitter) {
        ctx->jitter_sum_us += value_us;
        ctx->jitter_count++;
    } else if (value_us > ctx->max_crit_latency_us) {
        ctx->max_crit_latency_us = value_us;
    }
    if (miss) {
        ctx->deadline_misses++;
    }
    portEXIT_CRITICAL(&ctx->lock);

    ESP_LOGI(TAG, "[STR_METRIC] node=%s metric=%s value_us=%" PRIu32 " deadline_us=%" PRIu32 " miss=%u",
             ctx->node_tag, metric_name, value_us, deadline_us, miss);
}

void str_metrics_snapshot(str_metrics_ctx_t *ctx,
                          uint32_t *out_max_lat_us,
                          uint32_t *out_avg_jitter_us,
                          uint16_t *out_misses)
{
    if (!ctx) {
        return;
    }
    portENTER_CRITICAL(&ctx->lock);
    if (out_max_lat_us) {
        *out_max_lat_us = ctx->max_crit_latency_us;
    }
    if (out_avg_jitter_us) {
        *out_avg_jitter_us = (ctx->jitter_count > 0U)
                                 ? (uint32_t)(ctx->jitter_sum_us / ctx->jitter_count)
                                 : 0U;
    }
    if (out_misses) {
        *out_misses = ctx->deadline_misses;
    }
    portEXIT_CRITICAL(&ctx->lock);
}

void str_metrics_log_health(str_metrics_ctx_t *ctx,
                            const TaskHandle_t *handles,
                            const char *const *names,
                            size_t count)
{
    uint32_t max_lat = 0;
    uint32_t avg_jit = 0;
    uint16_t misses = 0;
    str_metrics_snapshot(ctx, &max_lat, &avg_jit, &misses);

    const uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    const uint32_t free_heap = esp_get_free_heap_size();
    const uint32_t min_heap = esp_get_minimum_free_heap_size();

    ESP_LOGI(TAG,
             "[STR_HEALTH] node=%s uptime_s=%" PRIu32 " free_heap=%" PRIu32 " min_heap=%" PRIu32
             " max_lat_us=%" PRIu32 " avg_jit_us=%" PRIu32 " misses=%u",
             ctx ? ctx->node_tag : "NODE", uptime_s, free_heap, min_heap, max_lat, avg_jit, misses);

    if (handles && names) {
        for (size_t i = 0; i < count; i++) {
            if (handles[i] != NULL) {
                const UBaseType_t words = uxTaskGetStackHighWaterMark(handles[i]);
                ESP_LOGI(TAG, "[STR_STACK] node=%s task=%s min_free_bytes=%u",
                         ctx ? ctx->node_tag : "NODE",
                         names[i],
                         (unsigned int)(words * sizeof(StackType_t)));
            }
        }
    }

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
    char *stats_buf = malloc(1024);
    if (stats_buf) {
        vTaskGetRunTimeStats(stats_buf);
        ESP_LOGI(TAG, "[STR_CPU]\n%s", stats_buf);
        free(stats_buf);
    }
#endif
}
