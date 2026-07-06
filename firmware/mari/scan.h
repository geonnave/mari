#ifndef __SCAN_H
#define __SCAN_H

/**
 * @ingroup     mari
 * @brief       Scan management
 *
 * @{
 * @file
 * @author Geovane Fedrecheski <geovane.fedrecheski@inria.fr>
 * @copyright Inria, 2024-now
 * @}
 */

#include <nrf.h>
#include <stdint.h>
#include <stdbool.h>

#include "packet.h"
#include "models.h"

//=========================== defines =========================================

#define MARI_MAX_SCAN_LIST_SIZE       (5)
#define MARI_SCAN_OLD_US              (1000 * 1000)      // rssi reading considered old after 1 s (must cover a full advertising-channel sweep: 3 slotframes, ~770 ms on the huge schedule)
#define MARI_HANDOVER_RSSI_HYSTERESIS (24)               // hysteresis (in dBm) for handover
#define MARI_HANDOVER_MIN_INTERVAL    (1000 * 1000 * 5)  // minimum interval between handovers (in us)
#define MARI_HANDOVER_RSSI_EWMA_SHIFT (2)                // serving-gateway rssi smoothing: ewma alpha = 1/2^shift

//=========================== variables =======================================

// a lightweight scan structure without bloom filter
typedef struct __attribute__((packed)) {
    uint8_t          version;
    mr_packet_type_t type;
    uint16_t         network_id;
    uint64_t         asn;
    uint64_t         src;
    uint8_t          remaining_capacity;
    uint8_t          active_schedule_id;
} mr_beacon_scan_header_t;

typedef struct {
    int8_t                  rssi;
    uint32_t                timestamp;
    uint64_t                captured_asn;
    mr_beacon_scan_header_t beacon;
} mr_channel_info_t;

typedef struct {
    uint64_t          gateway_id;
    mr_channel_info_t channel_info[MARI_N_BLE_ADVERTISING_CHANNELS];  // channels 37, 38, 39
} mr_gateway_scan_t;

//=========================== prototypes ======================================

void mr_scan_add(mr_beacon_packet_header_t beacon, int8_t rssi, uint8_t channel, uint32_t ts_scan, uint64_t asn_scan);

bool mr_scan_select(mr_channel_info_t *best_channel_info, uint32_t ts_scan_started, uint32_t ts_scan_ended);

#endif  // __SCAN_H
