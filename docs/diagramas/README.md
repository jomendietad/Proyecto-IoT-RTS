# Diagramas, Protocolos y Máquinas de Estados (`docs/diagramas/`)

Contiene la especificación funcional y de comunicaciones entre las 4 placas y Telegram:

- [`protocolo_ble_gatt.md`](protocolo_ble_gatt.md): arquitectura BLE GATT en estrella (NimBLE), nombres GAP, servicio `0xFF10` (`0xFF11` Notify / `0xFF12` Write), reconexión automática del Maestro S3 y layout binario byte a byte de los opcodes `0xC0–0xCE`.
- [`fsm_y_telegram.md`](fsm_y_telegram.md): FSM de 3 estados de la alarma tipo carro, cerradura *toggle* y acoplamiento `N1 → S3 → N2`, reglas de alertas por sobresalto térmico (`|ΔT| >= 2.0 °C` y `T >= T_high`), comandos y `/panel` *Inline Keyboard* de Telegram, más failover dual Wi-Fi.
