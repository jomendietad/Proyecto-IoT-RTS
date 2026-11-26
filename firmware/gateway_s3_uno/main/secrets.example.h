#pragma once

/*
 * Plantilla de credenciales locales para el Gateway ESP32-S3 (versionada en Git).
 *
 * INSTRUCCIONES:
 *   1. Copia este archivo como "secrets.h" en esta misma carpeta:
 *        cp firmware/gateway_s3_uno/main/secrets.example.h firmware/gateway_s3_uno/main/secrets.h
 *   2. Reemplaza los valores con tus credenciales reales de Wi-Fi y Telegram.
 *   3. "secrets.h" esta listado en .gitignore para nunca subirse a GitHub.
 */

/* Red Wi-Fi primaria (2.4 GHz) */
#define WIFI_PRIMARY_SSID           "TU_WIFI_PRIMARIA"
#define WIFI_PRIMARY_PASS           "TU_CLAVE_PRIMARIA"

/* Red Wi-Fi de respaldo / hotspot movil (dejar "" si no se usa) */
#define WIFI_BACKUP_SSID            ""
#define WIFI_BACKUP_PASS            ""

/* Token generado por @BotFather (formato: "123456789:ABCdef...") */
#define TELEGRAM_BOT_TOKEN          "000000000:REEMPLAZA_CON_TU_TOKEN_DE_BOTFATHER"

/*
 * Identificador numerico del chat o grupo autorizado (ej. -1001234567890LL o -4123456789LL).
 * Usa sufijo LL para soportar IDs negativos de grupos/supergrupos de 64 bits.
 */
#define TELEGRAM_AUTHORIZED_CHAT_ID (0LL)
