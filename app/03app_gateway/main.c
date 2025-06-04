/**
 * @file
 * @ingroup     app
 *
 * @brief       Mira Gateway application example
 *
 * @author Geovane Fedrecheski <geovane.fedrecheski@inria.fr>
 *
 * @copyright Inria, 2025
 */
#include <nrf.h>
#include <stdio.h>

#include "mr_device.h"
#include "mr_radio.h"
#include "mr_timer_hf.h"
#include "mira.h"
#include "packet.h"

//=========================== defines ==========================================

#define MIRA_APP_TIMER_DEV 1

#define DATA_LEN 4

typedef struct {
    bool dummy;
} gateway_vars_t;

typedef struct {
    uint32_t n_downlink;  ///< Number of packets sent to nodes (as unicast)
    uint32_t n_uplink;    ///< Number of packets received from nodes
} stats_vars_t;

//=========================== variables ========================================

gateway_vars_t gateway_vars = { 0 };

stats_vars_t stats_vars = { 0 };

uint8_t packet[MIRA_PACKET_MAX_SIZE] = { 0 };
uint8_t payload[]                    = { 0xFA, 0xFA, 0xFA, 0xFA, 0xFA };
uint8_t payload_len                  = 5;

extern schedule_t schedule_minuscule, schedule_tiny, schedule_huge;

schedule_t *schedule_app = &schedule_huge;

//=========================== prototypes =======================================

void mira_event_callback(mr_event_t event, mr_event_data_t event_data);
void tx_to_all_connected(void);
void stats_register(uint8_t type);
void _debug_print_stats(void);
void _debug_print_schedule(void);

//=========================== main =============================================

int main(void) {
    printf("Hello Mira Gateway %016llX\n", mr_device_id());
    mr_timer_hf_init(MIRA_APP_TIMER_DEV);

    mr_timer_hf_set_periodic_us(MIRA_APP_TIMER_DEV, 0, 1000 * 750, &tx_to_all_connected);

    mr_timer_hf_set_periodic_us(MIRA_APP_TIMER_DEV, 1, 1000 * 1005, &_debug_print_stats);

    mira_init(MIRA_GATEWAY, 10, schedule_app, &mira_event_callback);

    while (1) {
        __SEV();
        __WFE();
        __WFE();

        mira_event_loop();
    }
}

//=========================== callbacks ========================================

void mira_event_callback(mr_event_t event, mr_event_data_t event_data) {
    (void)event_data;
    uint32_t now_ts_s = mr_timer_hf_now(MIRA_APP_TIMER_DEV) / 1000 / 1000;
    switch (event) {
        case MIRA_NEW_PACKET:
        {
            stats_register('U');
            // mira_packet_t packet = event_data.data.new_packet;
            // printf("RX %u B: src=%016llX dst=%016llX (rssi %d) payload=", packet.len, packet.header->src, packet.header->dst, mr_radio_rssi());
            // for (int i = 0; i < packet.payload_len; i++) {
            //     printf("%02X ", packet.payload[i]);
            // }
            // printf("\n");
            break;
        }
        case MIRA_NODE_JOINED:
            printf("%d New node joined: %016llX  (%d nodes connected)\n", now_ts_s, event_data.data.node_info.node_id, mira_gateway_count_nodes());
            // uint64_t joined_nodes[MIRA_MAX_NODES] = { 0 };
            // uint8_t joined_nodes_len = mira_gateway_get_nodes(joined_nodes);
            // printf("Number of connected nodes: %d\n", joined_nodes_len);
            //  TODO: send list of joined_nodes to Edge Gateway via UART
            break;
        case MIRA_NODE_LEFT:
            printf("%d Node left: %016llX, reason: %u  (%d nodes connected)\n", now_ts_s, event_data.data.node_info.node_id, event_data.tag, mira_gateway_count_nodes());
            break;
        case MIRA_ERROR:
            printf("Error, reason: %u\n", event_data.tag);
            break;
        default:
            break;
    }
}

//=========================== private ========================================

void tx_to_all_connected(void) {
    uint64_t nodes[MIRA_MAX_NODES] = { 0 };
    uint8_t  nodes_len             = mira_gateway_get_nodes(nodes);
    for (int i = 0; i < nodes_len; i++) {
        // printf("Enqueing TX to node %d: %016llX\n", i, nodes[i]);
        payload[0]         = i;
        uint8_t packet_len = mr_build_packet_data(packet, nodes[i], payload, payload_len);
        mira_tx(packet, packet_len);
        stats_register('D');
    }
}

void stats_register(uint8_t type) {
    if (type == 'D') {
        stats_vars.n_downlink++;
    } else if (type == 'U') {
        stats_vars.n_uplink++;
    }
}

void _debug_print_stats(void) {
    uint32_t now_ts_ms = mr_timer_hf_now(MIRA_APP_TIMER_DEV) / 1000;
    uint32_t now_ts_s  = now_ts_ms / 1000;
    // calculate success rate
    float rate = (float)stats_vars.n_uplink / (float)stats_vars.n_downlink * 100.0;
    printf("ts = %d.%d Success = %.2f%%: %u downlink packets, %u uplink packets\n", now_ts_s, now_ts_ms, rate, stats_vars.n_downlink, stats_vars.n_uplink);
}

void _debug_print_schedule(void) {
    uint8_t schedule_len = schedule_app->n_cells;
    printf("Schedule cells: ");
    for (int i = 0; i < schedule_len; i++) {
        cell_t cell = schedule_app->cells[i];
        if (cell.type == SLOT_TYPE_UPLINK) {
            printf("%d-U-%016llX ", i, cell.assigned_node_id);
        } else if (cell.type == SLOT_TYPE_DOWNLINK) {
            printf("%d-D ", i);
        } else if (cell.type == SLOT_TYPE_BEACON) {
            printf("%d-B ", i);
        } else if (cell.type == SLOT_TYPE_SHARED_UPLINK) {
            printf("%d-S ", i);
        }
    }
    printf("\n");
}
