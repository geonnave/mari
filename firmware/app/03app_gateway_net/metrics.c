/**
 * @file
 * @ingroup     app
 *
 * @brief       Metrics collection for the gateway
 *
 * @author Geovane Fedrecheski <geovane.fedrecheski@inria.fr>
 *
 * @copyright Inria, 2025
 */

#include <stdbool.h>
#include <stdio.h>

#include "mr_radio.h"
#include "mac.h"
#include "models.h"

#include "metrics.h"

//=========================== types ============================================

typedef struct {
    uint64_t node_id;
    uint32_t tx_count;
    uint32_t rx_count;
} node_metrics_t;

//=========================== variables ========================================

typedef struct {
    node_metrics_t nodes[MARI_N_CELLS_MAX];
} metrics_vars_t;

metrics_vars_t metrics_vars = { 0 };

//=========================== functions ========================================

void metrics_init(void) {
}

void metrics_add_node(uint64_t node_id) {
    // A node already in the table keeps its slot. Adding it twice would take a
    // second slot for the same node, and the table is only freed by
    // metrics_clear_node, so every unmatched join leaks one. A gateway that
    // sees more joins than leaves then fills the table, after which the search
    // below finds nothing free: the node is never added, its tx_count and
    // rx_count never increment again, and the probe counters read as though
    // the gateway had stopped transmitting to a node it is still serving.
    for (uint8_t i = 0; i < MARI_N_CELLS_MAX; i++) {
        if (metrics_vars.nodes[i].node_id == node_id) {
            return;
        }
    }
    for (uint8_t i = 0; i < MARI_N_CELLS_MAX; i++) {
        if (metrics_vars.nodes[i].node_id == 0) {
            metrics_vars.nodes[i].node_id  = node_id;
            metrics_vars.nodes[i].tx_count = 0;
            metrics_vars.nodes[i].rx_count = 0;
            return;
        }
    }
    // Say so rather than carrying on silently: from here every metric this
    // gateway reports for a newly joined node is wrong, and nothing else in
    // the system can tell.
    printf("metrics: node table full (%d), not tracking %016llX\n", MARI_N_CELLS_MAX, node_id);
}

void metrics_clear_node(uint64_t node_id) {
    for (uint8_t i = 0; i < MARI_N_CELLS_MAX; i++) {
        if (metrics_vars.nodes[i].node_id == node_id) {
            metrics_vars.nodes[i].node_id  = 0;
            metrics_vars.nodes[i].tx_count = 0;
            metrics_vars.nodes[i].rx_count = 0;
            break;
        }
    }
}

bool metrics_is_probe(uint8_t *payload, uint32_t payload_len) {
    return payload_len == sizeof(mr_metrics_payload_t) && payload[0] == MARI_PAYLOAD_TYPE_METRICS_PROBE;
}

void metrics_handle_rx_probe(uint64_t node_id, uint8_t *payload) {
    mr_metrics_payload_t *metrics_payload = (mr_metrics_payload_t *)payload;

    metrics_payload->gw_rx_asn  = mr_mac_get_asn();
    metrics_payload->rssi_at_gw = mr_radio_rssi();

    for (uint8_t i = 0; i < MARI_N_CELLS_MAX; i++) {
        if (metrics_vars.nodes[i].node_id == node_id) {
            metrics_payload->gw_rx_count = ++metrics_vars.nodes[i].rx_count;
            break;
        }
    }
}

void metrics_handle_tx_probe(uint64_t node_id, uint8_t *payload) {
    mr_metrics_payload_t *metrics_payload = (mr_metrics_payload_t *)payload;

    metrics_payload->gw_tx_enqueued_asn = mr_mac_get_asn();

    for (uint8_t i = 0; i < MARI_N_CELLS_MAX; i++) {
        if (metrics_vars.nodes[i].node_id == node_id) {
            metrics_payload->gw_tx_count = ++metrics_vars.nodes[i].tx_count;
            break;
        }
    }
}
