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
#include "mr_ipc.h"
#include "scheduler.h"
#include "mira.h"
#include "packet.h"

//=========================== defines ==========================================

typedef struct {
    mr_ipc_req_t ipc_req;
} gateway_vars_t;

//=========================== variables ========================================

gateway_vars_t gateway_vars = { 0 };

extern schedule_t schedule_minuscule, schedule_tiny, schedule_huge;
schedule_t       *schedule_app = &schedule_huge;

extern volatile __attribute__((section(".shared_data"))) mr_ipc_shared_data_t ipc_shared_data;

//=========================== prototypes =======================================

void mira_event_callback(mr_event_t event, mr_event_data_t event_data);

//=========================== main =============================================

int main(void) {
    printf("Hello Mira Gateway Net Core %016llX\n", mr_device_id());

    // TODO: communicate with the application core via IPC

    // Network core must remain on
    ipc_shared_data.net_ready = true;

    while (1) {
        __SEV();
        __WFE();
        __WFE();

        if (gateway_vars.ipc_req != MR_IPC_REQ_NONE) {
            ipc_shared_data.net_ack = false;
            switch (gateway_vars.ipc_req) {
                // Mira node functions
                case MR_IPC_MIRA_INIT_REQ:
                    mira_init(MIRA_GATEWAY, MIRA_NET_ID_DEFAULT, schedule_app, &mira_event_callback);
                    break;
                case MR_IPC_MIRA_NODE_TX_REQ:
                    while (!mira_node_is_connected()) {}
                    mira_tx((uint8_t *)ipc_shared_data.tx_pdu.buffer, ipc_shared_data.tx_pdu.length);
                    break;
                default:
                    break;
            }
            ipc_shared_data.net_ack = true;
            gateway_vars.ipc_req    = MR_IPC_REQ_NONE;
        }
    }
}

//=========================== callbacks ========================================

void mira_event_callback(mr_event_t event, mr_event_data_t event_data) {
    switch (event) {
        case MIRA_NEW_PACKET:
        {
            break;
        }
        case MIRA_NODE_JOINED:
            printf("New node joined: %016llX  (%d nodes connected)\n", event_data.data.node_info.node_id, mira_gateway_count_nodes());
            break;
        case MIRA_NODE_LEFT:
            printf("Node left: %016llX, reason: %u  (%d nodes connected)\n", event_data.data.node_info.node_id, event_data.tag, mira_gateway_count_nodes());
            break;
        case MIRA_ERROR:
            printf("Error, reason: %u\n", event_data.tag);
            break;
        default:
            break;
    }
}

//=========================== private ========================================
