/**
 * @file
 * @ingroup     app
 *
 * @brief       Blink Node application example
 *
 * @author Geovane Fedrecheski <geovane.fedrecheski@inria.fr>
 *
 * @copyright Inria, 2024
 */
#include <nrf.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "bl_device.h"
#include "bl_radio.h"
#include "bl_timer_hf.h"
#include "blink.h"
#include "packet.h"

//=========================== defines ==========================================

#define BLINK_APP_TIMER_DEV 1

typedef struct {
    bool sent_one;
    uint32_t chunk_count;
} node_vars_t;

//=========================== variables ========================================

node_vars_t node_vars = { 0 };

uint8_t payload[] = { 0xF0, 0xF0, 0xF0, 0xF0, 0xF0 };
uint8_t payload_len = 5;

uint8_t payload_status[] = { 0x90, 0x38, 0xD7, 0xD4, 0x01, 0x57, 0x48, 0x60, 0x1E, 0x00 }; // to answer for 0x80
uint8_t payload_status_len = 1+8+1;

uint8_t payload_fake_start_ack[] = { 0x93, 0x38, 0xD7, 0xD4, 0x01, 0x57, 0x48, 0x60, 0x1E }; // to ack for 0x84
uint8_t payload_fake_start_ack_len = 1+8;

uint8_t payload_fake_chunck_ack[] = { 0x94, 0x38, 0xD7, 0xD4, 0x01, 0x57, 0x48, 0x60, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x01 }; // to ack for 0x85
uint8_t payload_fake_chunck_ack_len = 1+8+4+1;

extern schedule_t schedule_minuscule, schedule_tiny, schedule_small, schedule_huge, schedule_only_beacons, schedule_only_beacons_optimized_scan;

//=========================== prototypes =======================================

static void blink_event_callback(bl_event_t event, bl_event_data_t event_data);

//=========================== main =============================================

int main(void)
{
    printf("Hello Blink Node %016llX\n", bl_device_id());
    bl_timer_hf_init(BLINK_APP_TIMER_DEV);

    blink_init(BLINK_NODE, &schedule_minuscule, &blink_event_callback);

    while (1) {
        __SEV();
        __WFE();
        __WFE();

        // if (blink_node_is_connected() && !node_vars.sent_one) {
        //     blink_node_tx_payload(payload, payload_len);
        //     node_vars.sent_one = true;

        //     // // sleep for 500 ms
        //     // bl_timer_hf_delay_ms(BLINK_APP_TIMER_DEV, 1000);
        // }
    }
}

//=========================== callbacks ========================================

static void blink_event_callback(bl_event_t event, bl_event_data_t event_data) {
    switch (event) {
        case BLINK_NEW_PACKET:
            printf("New packet (%d B): ", event_data.data.new_packet.length);
            for (int i = 0; i < event_data.data.new_packet.length; i++) {
                printf("%02X ", event_data.data.new_packet.packet[i]);
            }
            printf("\n");

            uint8_t *payload = event_data.data.new_packet.packet + sizeof(bl_packet_header_t);
            if (payload[0] == 0x80) {
                printf("Received status query\n");
                blink_node_tx_payload(payload_status, payload_status_len);
            } else if (payload[0] == 0x84) {
                printf("Received start packet\n");
                node_vars.chunk_count = 0;
                blink_node_tx_payload(payload_fake_start_ack, payload_fake_start_ack_len);
            } else if (payload[0] == 0x85) {
                printf("Received chunck packet\n");
                node_vars.chunk_count++;
                memcpy(payload_fake_chunck_ack + (payload_fake_chunck_ack_len - 4 - 1), &node_vars.chunk_count, 4);
                blink_node_tx_payload(payload_fake_chunck_ack, payload_fake_chunck_ack_len);
            }
            break;
        case BLINK_CONNECTED: {
            uint64_t gateway_id = event_data.data.gateway_info.gateway_id;
            printf("Connected to gateway %016llX\n", gateway_id);
            break;
        }
        case BLINK_DISCONNECTED: {
            uint64_t gateway_id = event_data.data.gateway_info.gateway_id;
            printf("Disconnected from gateway %016llX, reason: %u\n", gateway_id, event_data.tag);
            break;
        }
        case BLINK_ERROR:
            printf("Error\n");
            break;
        default:
            break;
    }
}
