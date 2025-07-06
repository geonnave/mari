/**
 * @file
 * @ingroup     app
 *
 * @brief       Mira Gateway application (radio side)
 *
 * @author Geovane Fedrecheski <geovane.fedrecheski@inria.fr>
 * @author Alexandre Abadie <alexandre.abadie@inria.fr>
 *
 * @copyright Inria, 2025-now
 */
#include <nrf.h>
#include <stdio.h>
#include <stdbool.h>

#include "mr_device.h"
#include "mr_radio.h"
#include "mr_timer_hf.h"
#include "mr_gpio.h"
#include "scheduler.h"
#include "packet.h"
#include "mira.h"

#include "uart.h"
#include "hdlc.h"

//=========================== defines ==========================================

#define MR_UART_INDEX    (0)        ///< Index of UART peripheral to use
#define MR_UART_BAUDRATE (1000000)  ///< Baudrate of UART peripheral
// #define MR_UART_BAUDRATE   (115200)  ///< Baudrate of UART peripheral
#define BUFFER_MAX_BYTES   (255U)  ///< Max bytes in UART receive buffer
#define MIRA_APP_TIMER_DEV 1

typedef struct {
    uint8_t length;                    ///< Length of the radio packet
    uint8_t buffer[BUFFER_MAX_BYTES];  ///< Buffer containing the radio packet
} gateway_packet_t;

typedef struct {
    gateway_packet_t packet;
    gateway_packet_t mira_uart_frame;
    bool             mira_uart_frame_ready;
    gateway_packet_t hdlc_frame;
    bool             hdlc_frame_ready;
    gateway_packet_t uart_frame_payload;
    bool             uart_frame_ready;
} gateway_vars_t;

//=========================== variables ========================================

gateway_vars_t gateway_vars = { 0 };

// UART RX and TX pins
static const mr_gpio_t mr_uart_tx = { .port = 1, .pin = 1 };
static const mr_gpio_t mr_uart_rx = { .port = 1, .pin = 0 };

extern schedule_t schedule_minuscule, schedule_tiny, schedule_huge;
schedule_t       *schedule_app = &schedule_huge;

//=========================== prototypes =======================================

void mira_event_callback(mr_event_t event, mr_event_data_t event_data);
void uart_rx_callback(uint8_t data);

//=========================== main =============================================

int main(void) {
    printf("Hello Mira Gateway Net Core %016llX\n", mr_device_id());
    mr_timer_hf_init(MIRA_APP_TIMER_DEV);

    mira_init(MIRA_GATEWAY, MIRA_NET_ID_DEFAULT, schedule_app, &mira_event_callback);

    mr_timer_hf_set_periodic_us(MIRA_APP_TIMER_DEV, 2, mr_scheduler_get_duration_us(), &mira_event_loop);

    mr_uart_init(MR_UART_INDEX, &mr_uart_rx, &mr_uart_tx, MR_UART_BAUDRATE, &uart_rx_callback);

    while (1) {
        __SEV();
        __WFE();
        __WFE();

        if (gateway_vars.uart_frame_ready) {
            gateway_vars.uart_frame_ready = false;
            // printf("Received payload: ");
            // for (size_t i = 0; i < gateway_vars.uart_frame_payload.length; i++) {
            //     printf("%02X ", gateway_vars.uart_frame_payload.buffer[i]);
            // }
            // printf("\n");

            uint8_t _uart_type = gateway_vars.uart_frame_payload.buffer[0];  // just ignore for now
            if (_uart_type != 0x01) {
                printf("Invalid UART packet type: %02X\n", _uart_type);
                continue;
            }

            uint8_t *mira_frame     = gateway_vars.uart_frame_payload.buffer + 1;
            uint8_t  mira_frame_len = gateway_vars.uart_frame_payload.length - 1;

            mr_packet_header_t *header = (mr_packet_header_t *)mira_frame;
            header->src                = mr_device_id();

            mira_tx(mira_frame, mira_frame_len);
        }

        if (gateway_vars.mira_uart_frame_ready) {
            gateway_vars.mira_uart_frame_ready = false;
            gateway_vars.hdlc_frame.length     = db_hdlc_encode(gateway_vars.mira_uart_frame.buffer, gateway_vars.mira_uart_frame.length, gateway_vars.hdlc_frame.buffer);
            mr_uart_write(MR_UART_INDEX, gateway_vars.hdlc_frame.buffer, gateway_vars.hdlc_frame.length);
        }
    }
}

//=========================== callbacks ========================================

// TODO: move decoding logic out of isr context
void uart_rx_callback(uint8_t data) {
    // printf("Received: %02X - %c\n", data, data);

    db_hdlc_state_t state = db_hdlc_rx_byte(data);
    if (state == DB_HDLC_STATE_READY) {
        gateway_vars.uart_frame_payload.length = db_hdlc_decode(gateway_vars.uart_frame_payload.buffer);
        gateway_vars.uart_frame_ready          = true;
    }
}

void mira_event_callback(mr_event_t event, mr_event_data_t event_data) {
    switch (event) {
        case MIRA_NEW_PACKET:
        {
            gateway_vars.mira_uart_frame.buffer[0] = MIRA_EDGE_DATA;
            memcpy(gateway_vars.mira_uart_frame.buffer + 1, event_data.data.new_packet.header, sizeof(mr_packet_header_t));
            memcpy(gateway_vars.mira_uart_frame.buffer + 1 + sizeof(mr_packet_header_t), event_data.data.new_packet.payload, event_data.data.new_packet.payload_len);
            gateway_vars.mira_uart_frame.length = sizeof(mr_packet_header_t) + event_data.data.new_packet.payload_len;
            gateway_vars.mira_uart_frame_ready  = true;
            break;
        }
        case MIRA_KEEPALIVE:
        {
            gateway_vars.mira_uart_frame.buffer[0] = MIRA_EDGE_KEEPALIVE;
            memcpy(gateway_vars.mira_uart_frame.buffer + 1, &event_data.data.node_info.node_id, sizeof(uint64_t));
            gateway_vars.mira_uart_frame.length = 1 + sizeof(uint64_t);
            gateway_vars.mira_uart_frame_ready  = true;
            break;
        }
        case MIRA_NODE_JOINED:
            puts("#");
            gateway_vars.mira_uart_frame.buffer[0] = MIRA_EDGE_NODE_JOINED;
            memcpy(gateway_vars.mira_uart_frame.buffer + 1, &event_data.data.node_info.node_id, sizeof(uint64_t));
            gateway_vars.mira_uart_frame.length = 1 + sizeof(uint64_t);
            gateway_vars.mira_uart_frame_ready  = true;
            break;
        case MIRA_NODE_LEFT:
            puts("0");
            gateway_vars.mira_uart_frame.buffer[0] = MIRA_EDGE_NODE_LEFT;
            memcpy(gateway_vars.mira_uart_frame.buffer + 1, &event_data.data.node_info.node_id, sizeof(uint64_t));
            gateway_vars.mira_uart_frame.length = 1 + sizeof(uint64_t);
            gateway_vars.mira_uart_frame_ready  = true;
            break;
        case MIRA_ERROR:
            printf("Error, reason: %u\n", event_data.tag);
            break;
        default:
            break;
    }
}

//=========================== private ========================================
