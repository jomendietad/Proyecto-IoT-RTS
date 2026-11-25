#include "ble_protocol.h"

bool ble_dedup_check(ble_dedup_table_t *tbl, uint16_t src_addr, uint8_t opcode, uint8_t seq_num)
{
    if (!tbl) {
        return true;
    }

    for (size_t i = 0; i < BLE_DEDUP_SLOTS; i++) {
        ble_dedup_entry_t *e = &tbl->entries[i];
        if (e->used && e->src_addr == src_addr && e->opcode == opcode) {
            if (e->seq_num == seq_num) {
                return false; /* Duplicado */
            }
            e->seq_num = seq_num;
            return true;
        }
    }

    ble_dedup_entry_t *slot = &tbl->entries[tbl->next_slot % BLE_DEDUP_SLOTS];
    slot->src_addr = src_addr;
    slot->opcode = opcode;
    slot->seq_num = seq_num;
    slot->used = true;
    tbl->next_slot = (uint8_t)((tbl->next_slot + 1U) % BLE_DEDUP_SLOTS);
    return true;
}
