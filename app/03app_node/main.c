/**
 * @file
 * @ingroup     app
 *
 * @brief       Mari Node application example
 *
 * @author Geovane Fedrecheski <geovane.fedrecheski@inria.fr>
 *
 * @copyright Inria, 2025
 */
#include <nrf.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "mr_gpio.h"
#include "mr_device.h"
#include "mr_radio.h"
#include "mr_timer_hf.h"
#include "mari.h"
#include "packet.h"

#include "board.h"

//=========================== defines ==========================================

#define MARI_APP_TIMER_DEV 1

typedef struct {
    mr_event_t      event;
    mr_event_data_t event_data;
    bool            event_ready;
} node_vars_t;

//=========================== variables ========================================

node_vars_t node_vars = { 0 };

uint8_t payload[]   = { 0xF0, 0xF0, 0xF0, 0xF0, 0xF0 };
uint8_t payload_len = 5;

extern schedule_t schedule_minuscule, schedule_tiny, schedule_huge;

schedule_t *schedule_app = &schedule_huge;

//=========================== prototypes =======================================

static void mari_event_callback(mr_event_t event, mr_event_data_t event_data);
static void tx_if_connected(void);

//=========================== main =============================================

int main(void) {
    printf("Hello Mari Node %016llX\n", mr_device_id());
    mr_timer_hf_init(MARI_APP_TIMER_DEV);

    // mr_timer_hf_set_periodic_us(MARI_APP_TIMER_DEV, 0, 1000 * 500, &tx_if_connected);
    mr_timer_hf_set_oneshot_us(MARI_APP_TIMER_DEV, 0, 0, &tx_if_connected);  // do not tx

    board_init();

    mari_init(MARI_NODE, 1707, schedule_app, &mari_event_callback);

    while (1) {
        __SEV();
        __WFE();
        __WFE();

        if (node_vars.event_ready) {
            node_vars.event_ready = false;

            mr_event_t      event      = node_vars.event;
            mr_event_data_t event_data = node_vars.event_data;

            switch (event) {
                case MARI_NEW_PACKET:
                {
                    mari_packet_t packet = event_data.data.new_packet;
                    printf("RX %u B: src=%016llX dst=%016llX (rssi %d) payload=", packet.len, packet.header->src, packet.header->dst, mr_radio_rssi());
                    for (int i = 0; i < packet.payload_len; i++) {
                        printf("%02X ", packet.payload[i]);
                    }
                    printf("\n");
                    mari_node_tx_payload(payload, payload_len);
                    break;
                }
                case MARI_CONNECTED:
                {
                    uint64_t gateway_id = event_data.data.gateway_info.gateway_id;
                    printf("Connected to gateway %016llX\n", gateway_id);
                    if (gateway_id == 0xCEA467E20BACC0AB) {
                        board_set_mari_status(GREEN);
                    } else {
                        board_set_mari_status(OTHER);
                    }
                    break;
                }
                case MARI_DISCONNECTED:
                {
                    uint64_t gateway_id = event_data.data.gateway_info.gateway_id;
                    printf("Disconnected from gateway %016llX, reason: %u\n", gateway_id, event_data.tag);
                    board_set_mari_status(RED);
                    break;
                }
                case MARI_ERROR:
                    printf("Error\n");
                    break;
                default:
                    break;
            }
        }

        mari_event_loop();
    }
}

//=========================== callbacks ========================================

static void mari_event_callback(mr_event_t event, mr_event_data_t event_data) {
    memcpy(&node_vars.event, &event, sizeof(mr_event_t));
    memcpy(&node_vars.event_data, &event_data, sizeof(mr_event_data_t));
    node_vars.event_ready = true;
}

//=========================== private =========================================

static void tx_if_connected(void) {
    if (mari_node_is_connected()) {
        mari_node_tx_payload(payload, payload_len);
    }
}
