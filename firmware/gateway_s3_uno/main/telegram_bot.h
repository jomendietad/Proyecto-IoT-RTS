#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "str_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Niveles de prioridad de envio en la cola de Telegram. */
typedef enum {
    TG_PRIO_NORMAL = 0,
    TG_PRIO_URGENT = 1,
} tg_prio_t;

/** @brief Callback para consultar las tareas registradas del Gateway S3 al generar /metricas. */
typedef void (*tg_get_s3_tasks_cb_t)(const TaskHandle_t **out_handles,
                                     const char *const **out_names,
                                     size_t *out_count);

/**
 * @brief Inicializa colas TX/RX, valida credenciales y crea task_tg_tx y task_tg_rx en Core 1.
 */
esp_err_t telegram_bot_init(str_metrics_ctx_t *metrics,
                            tg_get_s3_tasks_cb_t tasks_cb,
                            TaskHandle_t *out_tx_task,
                            TaskHandle_t *out_rx_task);

/**
 * @brief Notifica al cliente Telegram si el enlace Wi-Fi esta disponible.
 */
void telegram_bot_set_wifi_online(bool online);

/**
 * @brief Encola un mensaje de texto hacia el chat autorizado con la prioridad indicada.
 */
esp_err_t telegram_bot_send(tg_prio_t prio, const char *text);

/**
 * @brief Encola el resumen con el Inline Keyboard interactivo del comando /panel.
 */
esp_err_t telegram_bot_send_panel(void);

#ifdef __cplusplus
}
#endif
