#include "mesh_protocol.h"

#include <string.h>

void mesh_build_device_uuid(iot_node_role_t role, const uint8_t mac[6], uint8_t out_uuid[16])
{
    if (!out_uuid) {
        return;
    }
    memset(out_uuid, 0, 16);
    out_uuid[0] = MESH_PROJECT_MAGIC_0;
    out_uuid[1] = MESH_PROJECT_MAGIC_1;
    out_uuid[2] = MESH_PROTO_VERSION;
    out_uuid[3] = (uint8_t)role;
    if (mac) {
        memcpy(&out_uuid[4], mac, 6);
    }
}

uint8_t mesh_calc_hops(uint8_t tx_ttl, uint8_t rx_ttl)
{
    return (tx_ttl >= rx_ttl) ? (uint8_t)(tx_ttl - rx_ttl) : 0U;
}

bool mesh_dedup_check(mesh_dedup_table_t *tbl, uint16_t src_addr, uint8_t opcode, uint8_t seq_num)
{
    if (!tbl) {
        return true;
    }

    for (size_t i = 0; i < MESH_DEDUP_SLOTS; i++) {
        mesh_dedup_entry_t *e = &tbl->entries[i];
        if (e->used && e->src_addr == src_addr && e->opcode == opcode) {
            if (e->seq_num == seq_num) {
                return false; /* Duplicado */
            }
            e->seq_num = seq_num;
            return true;
        }
    }

    mesh_dedup_entry_t *slot = &tbl->entries[tbl->next_slot % MESH_DEDUP_SLOTS];
    slot->src_addr = src_addr;
    slot->opcode = opcode;
    slot->seq_num = seq_num;
    slot->used = true;
    tbl->next_slot = (uint8_t)((tbl->next_slot + 1U) % MESH_DEDUP_SLOTS);
    return true;
}
