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

#include "mr_device.h"
#include "mr_radio.h"
#include "mr_timer_hf.h"
#include "mr_gpio.h"
#include "scheduler.h"
#include "packet.h"
#include "mira.h"

#include "uart.h"

//=========================== defines ==========================================

#define MR_UART_INDEX      (0)     ///< Index of UART peripheral to use
#define BUFFER_MAX_BYTES   (255U)  ///< Max bytes in UART receive buffer
#define MIRA_APP_TIMER_DEV 1

typedef struct {
    uint8_t length;                    ///< Length of the radio packet
    uint8_t buffer[BUFFER_MAX_BYTES];  ///< Buffer containing the radio packet
} gateway_packet_t;

typedef struct {
    gateway_packet_t packet;
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

    mr_uart_init(MR_UART_INDEX, &mr_uart_rx, &mr_uart_tx, 1000000, &uart_rx_callback);

    while (1) {
        __SEV();
        __WFE();
        __WFE();
    }
}

//=========================== callbacks ========================================

void uart_rx_callback(uint8_t data) {
    printf("Received: %c\n", data);
}

void mira_event_callback(mr_event_t event, mr_event_data_t event_data) {
    (void)event_data;
    uint32_t now_ts_s = mr_timer_hf_now(MIRA_APP_TIMER_DEV) / 1000 / 1000;

    // just send some dummy data
    gateway_vars.packet.buffer[0] = 0xFE;
    gateway_vars.packet.length    = 1;
    mr_uart_write(MR_UART_INDEX, gateway_vars.packet.buffer, gateway_vars.packet.length);

    switch (event) {
        case MIRA_NEW_PACKET:
        {
            break;
        }
        case MIRA_NODE_JOINED:
            printf("%d New node joined: %016llX  (%d nodes connected)\n", now_ts_s, event_data.data.node_info.node_id, mira_gateway_count_nodes());
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
