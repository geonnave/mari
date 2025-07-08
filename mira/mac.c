/**
 * @file
 * @ingroup     net_mac
 *
 * @brief       Lower MAC driver for Mira
 *
 * @author Geovane Fedrecheski <geovane.fedrecheski@inria.fr>
 *
 * @copyright Inria, 2025
 */

#include <arm_cmse.h>
#include <nrf.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mira.h"
#include "mac.h"
#include "queue.h"
#include "scan.h"
#include "scheduler.h"
#include "association.h"
#include "mr_radio.h"
#include "mr_timer_hf.h"
#include "packet.h"
#include "mr_device.h"

//=========================== debug ============================================

#ifndef DEBUG  // FIXME: remove before merge. Just to make VS Code enable code behind `#ifdef DEBUG`
#define DEBUG
#endif

#ifdef DEBUG
#include "mr_gpio.h"  // for debugging
// pins connected to logic analyzer, variable names reflect the channel number
mr_gpio_t pin0 = { .port = 1, .pin = 2 };
mr_gpio_t pin1 = { .port = 1, .pin = 3 };
mr_gpio_t pin2 = { .port = 1, .pin = 4 };
mr_gpio_t pin3 = { .port = 1, .pin = 5 };
#define DEBUG_GPIO_TOGGLE(pin) mr_gpio_toggle(pin)
#define DEBUG_GPIO_SET(pin)    mr_gpio_set(pin)
#define DEBUG_GPIO_CLEAR(pin)  mr_gpio_clear(pin)
#else
// No-op when DEBUG is not defined
#define DEBUG_GPIO_TOGGLE(pin) ((void)0))
#define DEBUG_GPIO_SET(pin) ((void)0))
#define DEBUG_GPIO_CLEAR(pin) ((void)0))
#endif  // DEBUG

//=========================== defines ==========================================

typedef enum {
    // common
    STATE_SLEEP,

    // transmitter
    STATE_TX_OFFSET = 21,
    STATE_TX_DATA   = 22,

    // receiver
    STATE_RX_OFFSET      = 31,
    STATE_RX_DATA_LISTEN = 32,
    STATE_RX_DATA        = 33,

} mr_mac_state_t;

typedef struct {
    uint64_t device_id;  ///< Device ID

    mr_mac_state_t state;              ///< State within the slot
    uint32_t       start_slot_ts;      ///< Timestamp of the start of the slot
    uint64_t       asn;                ///< Absolute slot number
    mr_slot_info_t current_slot_info;  ///< Information about the current slot

    mr_event_cb_t mira_event_callback;  ///< Function pointer, stores the application callback

    mr_received_packet_t received_packet;  ///< Last received packet

    bool     is_scanning;           ///< Whether the node is scanning for gateways
    uint32_t scan_started_ts;       ///< Timestamp of the start of the scan
    uint32_t scan_expected_end_ts;  ///< Timestamp of the expected end of the scan
    uint32_t current_scan_item_ts;  ///< Timestamp of the current scan item

    bool is_bg_scanning;           ///< Whether the node is scanning for gateways in the background
    bool bg_scan_sleep_next_slot;  ///< Whether the next slot is a sleep slot

    uint64_t synced_gateway;     ///< ID of the gateway the node is synchronized with
    uint16_t synced_network_id;  ///< Network ID of the gateway the node is synchronized with
    uint32_t synced_ts;          ///< Timestamp of the last synchronization
} mac_vars_t;

//=========================== variables ========================================

mac_vars_t mac_vars = { 0 };

mr_slot_durations_t slot_durations = {
    .tx_offset = MIRA_TS_TX_OFFSET,
    .tx_max    = MIRA_PACKET_TOA_WITH_PADDING,

    .rx_guard  = MIRA_RX_GUARD_TIME,
    .rx_offset = MIRA_TS_TX_OFFSET - MIRA_RX_GUARD_TIME,
    .rx_max    = MIRA_RX_GUARD_TIME + MIRA_PACKET_TOA_WITH_PADDING,  // same as rx_guard + tx_max

    .end_guard = MIRA_END_GUARD_TIME,

    .whole_slot = MIRA_WHOLE_SLOT_DURATION,
};

//=========================== prototypes =======================================

static inline void set_slot_state(mr_mac_state_t state);

static void new_slot_synced(void);
static void end_slot(void);
static void node_back_to_scanning(void);
static void disable_radio_and_intra_slot_timers(void);

static void activity_ti1(void);
static void activity_ti2(void);
static void activity_tie1(void);
static void activity_ti3(void);

static void activity_ri1(void);
static void activity_ri2(void);
static void activity_ri3(uint32_t ts);
static void activity_rie1(void);
static void activity_ri4(uint32_t ts);
static void activity_rie2(void);

static void fix_drift(uint32_t ts);

static void start_scan(void);
static void end_scan(void);
static void activity_scan_start_frame(uint32_t ts);
static void activity_scan_end_frame(uint32_t ts);
static bool select_gateway_and_sync(void);

static void start_background_scan(void);
static void end_background_scan(void);

static void isr_mac_radio_start_frame(uint32_t ts);
static void isr_mac_radio_end_frame(uint32_t ts);

//=========================== public ===========================================

void mr_mac_init(mr_event_cb_t event_callback) {
#ifdef DEBUG
#ifdef NRF5340_XXAA
    // Wait a bit to ensure application core has set up GPIO permissions
    // FIXME: replace with proper IPC handshake
    for (volatile int i = 0; i < 1000000; i++) {
        __NOP();
    }
#endif
    mr_gpio_init(&pin0, MR_GPIO_OUT);
    mr_gpio_init(&pin1, MR_GPIO_OUT);
    mr_gpio_init(&pin2, MR_GPIO_OUT);
    mr_gpio_init(&pin3, MR_GPIO_OUT);
#endif

    // initialize the high frequency timer
    mr_timer_hf_init(MIRA_TIMER_DEV);

    // initialize the radio
    mr_radio_init(&isr_mac_radio_start_frame, &isr_mac_radio_end_frame, MR_RADIO_BLE_2MBit);

    // node stuff
    mac_vars.device_id = mr_device_id();

    // synchronization stuff
    mac_vars.asn = 0;

    // application callback
    mac_vars.mira_event_callback = event_callback;

    // begin the slot
    set_slot_state(STATE_SLEEP);

    if (mira_get_node_type() == MIRA_GATEWAY) {
        mac_vars.start_slot_ts = mr_timer_hf_now(MIRA_TIMER_DEV);
        mr_assoc_set_state(JOIN_STATE_JOINED);
        mr_timer_hf_set_periodic_us(
            MIRA_TIMER_DEV,
            MIRA_TIMER_INTER_SLOT_CHANNEL,
            slot_durations.whole_slot,
            &new_slot_synced);
    } else {
        start_scan();
    }
}

uint64_t mr_mac_get_asn(void) {
    return mac_vars.asn;
}

uint64_t mr_mac_get_synced_ts(void) {
    return mac_vars.synced_ts;
}

uint64_t mr_mac_get_synced_gateway(void) {
    return mac_vars.synced_gateway;
}

uint16_t mr_mac_get_synced_network_id(void) {
    return mac_vars.synced_network_id;
}

inline bool mr_mac_node_is_synced(void) {
    return mac_vars.synced_gateway != 0;
}

//=========================== private ==========================================

static void set_slot_state(mr_mac_state_t state) {
    mac_vars.state = state;

    switch (state) {
        case STATE_RX_DATA_LISTEN:
            // DEBUG_GPIO_SET(&pin3);
        case STATE_TX_DATA:
        case STATE_RX_DATA:
            DEBUG_GPIO_SET(&pin1);
            break;
        case STATE_SLEEP:
            DEBUG_GPIO_CLEAR(&pin1);
            DEBUG_GPIO_CLEAR(&pin2);  // pin2 might be SET in case a packet had just started to arrive, so set it low again
            break;
        default:
            break;
    }
}

// --------------------- start/end synced slots -----------

static void new_slot_synced(void) {
    mac_vars.start_slot_ts = mr_timer_hf_now(MIRA_TIMER_DEV);
    DEBUG_GPIO_SET(&pin0);
    DEBUG_GPIO_CLEAR(&pin0);  // debug: show that a new slot started

    // perform timeout checks
    if (mira_get_node_type() == MIRA_GATEWAY) {
        // too long without receiving a packet from certain nodes? disconnect them
    } else if (mira_get_node_type() == MIRA_NODE) {
        if (mr_assoc_node_should_leave(mac_vars.asn)) {
            // assoc module determined that the node should leave, so disconnect and back to scanning
            mr_assoc_node_handle_disconnect();
            node_back_to_scanning();
            return;
        } else if (mr_assoc_node_too_long_waiting_for_join_response()) {
            // too long without receiving a join response? notify the association module which will backfoff
            bool keep_trying_to_join = mr_assoc_node_handle_failed_join();
            if (!keep_trying_to_join) {
                node_back_to_scanning();
                return;
            }
        } else if (mr_assoc_node_too_long_synced_without_joining()) {
            // too long synced without being able to join? give up and go back to scanning
            mr_assoc_node_handle_give_up_joining();
            node_back_to_scanning();
            return;
        }
    }

    mac_vars.current_slot_info = mr_scheduler_tick(mac_vars.asn++);

    if (mac_vars.current_slot_info.radio_action == MIRA_RADIO_ACTION_TX) {
        activity_ti1();
    } else if (mac_vars.current_slot_info.radio_action == MIRA_RADIO_ACTION_RX) {
        activity_ri1();
    } else if (mac_vars.current_slot_info.radio_action == MIRA_RADIO_ACTION_SLEEP) {
        // check if we should use this slot for background scan
        if (mira_get_node_type() == MIRA_GATEWAY || !MIRA_ENABLE_BACKGROUND_SCAN) {
            set_slot_state(STATE_SLEEP);
            end_slot();
        } else {  // node with background scan enabled
            start_background_scan();
        }
    }
}

static void node_back_to_scanning(void) {
    mac_vars.synced_gateway    = 0;
    mac_vars.synced_network_id = 0;
    mac_vars.synced_ts         = 0;
    set_slot_state(STATE_SLEEP);
    end_slot();
    start_scan();
}

static void end_slot(void) {
    if (!mr_mac_node_is_synced()) {
        // not synced, so we are not in a slot
        return;
    }
    disable_radio_and_intra_slot_timers();
}

static void disable_radio_and_intra_slot_timers(void) {
    mr_radio_disable();

    // NOTE: clean all timers
    mr_timer_hf_cancel(MIRA_TIMER_DEV, MIRA_TIMER_CHANNEL_1);
    mr_timer_hf_cancel(MIRA_TIMER_DEV, MIRA_TIMER_CHANNEL_2);
    mr_timer_hf_cancel(MIRA_TIMER_DEV, MIRA_TIMER_CHANNEL_3);
}

// --------------------- start/end scan -------------------

static void start_scan(void) {
    mac_vars.scan_started_ts      = mr_timer_hf_now(MIRA_TIMER_DEV);
    mac_vars.scan_expected_end_ts = mac_vars.scan_started_ts + MIRA_SCAN_MAX_DURATION;
    DEBUG_GPIO_SET(&pin0);  // debug: show that a new scan started
    mac_vars.is_scanning = true;
    mr_assoc_set_state(JOIN_STATE_SCANNING);

    // end_scan will be called when the scan is over
    mr_timer_hf_set_oneshot_with_ref_us(
        MIRA_TIMER_DEV,
        MIRA_TIMER_INTER_SLOT_CHANNEL,
        mac_vars.scan_started_ts,
        MIRA_SCAN_MAX_DURATION,  // scan during a certain amount of slots
        &end_scan);

    // mac_vars.assoc_info = mr_assoc_get_info(); // NOTE: why this?

    set_slot_state(STATE_RX_DATA_LISTEN);
    mr_radio_disable();
#ifdef MIRA_FIXED_SCAN_CHANNEL
    mr_radio_set_channel(MIRA_FIXED_SCAN_CHANNEL);  // not doing channel hopping for now
#else
    puts("Channel hopping not implemented yet for scanning");
#endif
    mr_radio_rx();
}

static void end_scan(void) {
    mac_vars.is_scanning = false;
    DEBUG_GPIO_CLEAR(&pin0);  // debug: show that the scan is over
    set_slot_state(STATE_SLEEP);
    disable_radio_and_intra_slot_timers();

    if (select_gateway_and_sync()) {
        // found a gateway and synchronized to it
        mr_assoc_node_handle_synced();
    } else {
        // no gateway found, back to scanning
        start_scan();
    }
}

// --------------------- start/end background scan --------

static void start_background_scan(void) {
    // 1. prepare timestamps and and arm timer
    if (!mac_vars.is_bg_scanning) {
        mac_vars.scan_started_ts      = mac_vars.start_slot_ts;  // reuse the slot start time as reference
        mac_vars.scan_expected_end_ts = mac_vars.scan_started_ts + MIRA_BG_SCAN_DURATION;
    }

    // end_scan will be called when the scan is over
    mr_timer_hf_set_oneshot_with_ref_us(
        MIRA_TIMER_DEV,
        MIRA_TIMER_CHANNEL_1,    // remember that the inter-slot timer is already being used for the slot
        mac_vars.start_slot_ts,  // in this case, we use the slot start time as reference because we are synced
        MIRA_BG_SCAN_DURATION,   // scan for some time during this slot
        &end_background_scan);

    // 2. turn on the radio, in case it was off (bg scan might be already running since the last slot)
    if (!mac_vars.is_bg_scanning) {
        set_slot_state(STATE_RX_DATA_LISTEN);
        mr_radio_disable();
#ifdef MIRA_FIXED_SCAN_CHANNEL
        mr_radio_set_channel(MIRA_FIXED_SCAN_CHANNEL);  // not doing channel hopping for now
#else
        puts("Channel hopping not implemented yet for scanning");
#endif
        mr_radio_rx();
    }
    mac_vars.is_bg_scanning = true;
}

static void end_background_scan(void) {
    cell_t next_slot                 = mr_scheduler_node_peek_slot(mac_vars.asn);  // remember: the asn was already incremented at new_slot_synced
    mac_vars.bg_scan_sleep_next_slot = next_slot.type == SLOT_TYPE_UPLINK && next_slot.assigned_node_id != mr_device_id();

    if (!mac_vars.bg_scan_sleep_next_slot) {
        // if next slot is not sleep, stop the background scan and check if there is an alternative gateway to join
        mac_vars.is_bg_scanning = false;
        set_slot_state(STATE_SLEEP);
        disable_radio_and_intra_slot_timers();

        if (select_gateway_and_sync()) {
            // found a gateway and synchronized to it
            mr_assoc_node_handle_synced();
        }
    }
}

// --------------------- tx activities --------------------

static void activity_ti1(void) {
    // ti1: arm tx timers and prepare the radio for tx
    // called by: function new_slot_synced
    set_slot_state(STATE_TX_OFFSET);

    mr_timer_hf_set_oneshot_with_ref_us(  // TODO: use PPI instead
        MIRA_TIMER_DEV,
        MIRA_TIMER_CHANNEL_1,
        mac_vars.start_slot_ts,
        slot_durations.tx_offset,
        &activity_ti2);

    mr_timer_hf_set_oneshot_with_ref_us(
        MIRA_TIMER_DEV,
        MIRA_TIMER_CHANNEL_2,
        mac_vars.start_slot_ts,
        slot_durations.tx_offset + slot_durations.tx_max,
        &activity_tie1);

    // FIXME: check if there is a packet to send before arming the timers
    uint8_t packet[MIRA_PACKET_MAX_SIZE];
    uint8_t packet_len = mr_queue_next_packet(mac_vars.current_slot_info.type, packet);
    if (packet_len > 0) {
        mr_radio_disable();
        mr_radio_set_channel(mac_vars.current_slot_info.channel);
        mr_radio_tx_prepare(packet, packet_len);
    } else {
        // nothing to tx
        set_slot_state(STATE_SLEEP);
        end_slot();
    }
}

static void activity_ti2(void) {
    // ti2: tx actually begins
    // called by: timer isr
    set_slot_state(STATE_TX_DATA);

    // FIXME: replace this call with a direct PPI connection, i.e., TsTxOffset expires -> radio tx
    mr_radio_tx_dispatch();
}

static void activity_tie1(void) {
    // tte1: something went wrong, stayed in tx for too long, abort
    // called by: timer isr
    set_slot_state(STATE_SLEEP);

    end_slot();
}

static void activity_ti3(void) {
    // ti3: all fine, finished tx, cancel error timers and go to sleep
    // called by: radio isr
    set_slot_state(STATE_SLEEP);

    // cancel tte1 timer
    mr_timer_hf_cancel(MIRA_TIMER_DEV, MIRA_TIMER_CHANNEL_2);

    end_slot();
}

// --------------------- rx activities --------------

// just write the placeholders for ri1

static void activity_ri1(void) {
    // ri1: arm rx timers and prepare the radio for rx
    // called by: function new_slot_synced
    set_slot_state(STATE_RX_OFFSET);

    mr_timer_hf_set_oneshot_with_ref_us(  // TODO: use PPI instead
        MIRA_TIMER_DEV,
        MIRA_TIMER_CHANNEL_1,
        mac_vars.start_slot_ts,
        slot_durations.rx_offset,
        &activity_ri2);

    mr_timer_hf_set_oneshot_with_ref_us(
        MIRA_TIMER_DEV,
        MIRA_TIMER_CHANNEL_2,
        mac_vars.start_slot_ts,
        slot_durations.tx_offset + slot_durations.rx_guard,
        &activity_rie1);

    mr_timer_hf_set_oneshot_with_ref_us(
        MIRA_TIMER_DEV,
        MIRA_TIMER_CHANNEL_3,
        mac_vars.start_slot_ts,
        slot_durations.rx_offset + slot_durations.rx_max,
        &activity_rie2);
}

static void activity_ri2(void) {
    // ri2: rx actually begins
    // called by: timer isr
    set_slot_state(STATE_RX_DATA_LISTEN);

    mr_radio_disable();
    mr_radio_set_channel(mac_vars.current_slot_info.channel);
    mr_radio_rx();
}

static void activity_ri3(uint32_t ts) {
    // ri3: a packet started to arrive
    // called by: radio isr
    set_slot_state(STATE_RX_DATA);

    // cancel timer for rx_guard
    mr_timer_hf_cancel(MIRA_TIMER_DEV, MIRA_TIMER_CHANNEL_2);

    mac_vars.received_packet.start_ts = ts;
}

static void activity_rie1(void) {
    // rie1: didn't receive start of packet before rx_guard, abort
    // called by: timer isr
    set_slot_state(STATE_SLEEP);

    // cancel timer for rx_max (rie2)
    mr_timer_hf_cancel(MIRA_TIMER_DEV, MIRA_TIMER_CHANNEL_3);

    end_slot();
}

static void activity_ri4(uint32_t ts) {
    // ri4: all fine, finished rx, cancel error timers and go to sleep
    // called by: radio isr
    set_slot_state(STATE_SLEEP);

    // cancel timer for rx_max (rie2)
    mr_timer_hf_cancel(MIRA_TIMER_DEV, MIRA_TIMER_CHANNEL_3);

    if (!mr_radio_pending_rx_read()) {
        // no packet received
        end_slot();
        return;
    }

    mr_radio_get_rx_packet(mac_vars.received_packet.packet, &mac_vars.received_packet.packet_len);

    mr_packet_header_t *header = (mr_packet_header_t *)mac_vars.received_packet.packet;

    if (header->version != MIRA_PROTOCOL_VERSION) {
        end_slot();
        return;
    }

    if (mira_get_node_type() == MIRA_NODE && mr_assoc_is_joined() && header->src == mac_vars.synced_gateway) {
        // only fix drift if the packet comes from the gateway we are synced to
        // NOTE: this should ideally be done at ri3 (when the packet starts), but we don't have the id there.
        //       could use use the physical BLE address for that?
        fix_drift(mac_vars.received_packet.start_ts);
    }

    // now that we know it's a mira packet, store some info about it
    mac_vars.received_packet.channel = mac_vars.current_slot_info.channel;
    mac_vars.received_packet.rssi    = mr_radio_rssi();
    mac_vars.received_packet.end_ts  = ts;
    mac_vars.received_packet.asn     = mac_vars.asn;

    mr_handle_packet(mac_vars.received_packet.packet, mac_vars.received_packet.packet_len);

    end_slot();
}

static void activity_rie2(void) {
    // rie2: something went wrong, stayed in rx for too long, abort
    // called by: timer isr
    set_slot_state(STATE_SLEEP);

    end_slot();
}

static void fix_drift(uint32_t ts) {
    uint32_t time_cpu_periph = 78;  // got this value by looking at the logic analyzer

    uint32_t expected_ts     = mac_vars.start_slot_ts + slot_durations.tx_offset + time_cpu_periph;
    int32_t  clock_drift     = ts - expected_ts;
    uint32_t abs_clock_drift = abs(clock_drift);

    if (abs_clock_drift < 100) {
        // drift is acceptable
        // adjust the slot reference
        mr_timer_hf_adjust_periodic_us(
            MIRA_TIMER_DEV,
            MIRA_TIMER_INTER_SLOT_CHANNEL,
            clock_drift);
    } else {
        // drift is too high, need to re-sync
        mr_event_data_t event_data = { .data.gateway_info.gateway_id = mac_vars.synced_gateway, .tag = MIRA_OUT_OF_SYNC };
        mac_vars.mira_event_callback(MIRA_DISCONNECTED, event_data);
        mr_assoc_set_state(JOIN_STATE_IDLE);
        set_slot_state(STATE_SLEEP);
        end_slot();
        start_scan();
    }
}

// --------------------- scan activities ------------------

static void activity_scan_dispatch_new_schedule(void) {
    mr_timer_hf_set_periodic_us(
        MIRA_TIMER_DEV,
        MIRA_TIMER_INTER_SLOT_CHANNEL,
        slot_durations.whole_slot,
        &new_slot_synced);
}

static bool select_gateway_and_sync(void) {
    uint32_t now_ts      = mr_timer_hf_now(MIRA_TIMER_DEV);
    bool     is_handover = false;

    mr_channel_info_t selected_gateway = { 0 };
    if (!mr_scan_select(&selected_gateway, mac_vars.scan_started_ts, now_ts)) {
        // no gateway found
        return false;
    }

    if (mr_assoc_is_joined()) {
        // this is a handover attempt
        if (selected_gateway.beacon.src == mac_vars.synced_gateway) {
            // should not happen, but just in case: already synced to this gateway, ignore it
            return false;
        }
        if (mac_vars.synced_ts - selected_gateway.timestamp < MIRA_HANDOVER_MIN_INTERVAL) {
            // just recently performed a synchronization, will not try again so soon
            return false;
        }
        if (selected_gateway.rssi > mac_vars.received_packet.rssi + MIRA_HANDOVER_RSSI_HYSTERESIS) {
            // the new gateway is not strong enough, ignore it
            return false;
        }
        is_handover = true;
    }

    if (!mr_scheduler_set_schedule(selected_gateway.beacon.active_schedule_id)) {
        // schedule not found, a new scan will begin again via new_scan
        return false;
    }

    if (is_handover) {
        // a handover is going to happen, notify application about network disconnection
        mr_event_data_t event_data = { .data.gateway_info.gateway_id = mac_vars.synced_gateway, .tag = MIRA_HANDOVER };
        mac_vars.mira_event_callback(MIRA_DISCONNECTED, event_data);
        // NOTE: should we `mr_assoc_set_state(JOIN_STATE_IDLE);` here?
        // during handover, we don't want the inter slot timer to tick again before we finish sync, so just set if far away in the future
        mr_timer_hf_set_periodic_us(
            MIRA_TIMER_DEV,
            MIRA_TIMER_INTER_SLOT_CHANNEL,
            slot_durations.whole_slot << 4,  // 16 slots in the future
            &new_slot_synced);
    }

    mac_vars.synced_gateway    = selected_gateway.beacon.src;
    mac_vars.synced_network_id = selected_gateway.beacon.network_id;
    mac_vars.synced_ts         = now_ts;

    // the selected gateway may have been scanned a few slot_durations ago, so we need to account for that difference
    // NOTE: this assumes that the slot duration is the same for gateways and nodes
    uint32_t time_since_beacon      = now_ts - selected_gateway.timestamp;
    uint64_t asn_count_since_beacon = (time_since_beacon / slot_durations.whole_slot) + 1;  // +1 because we are inside the current slot
    uint64_t time_into_gateway_slot = time_since_beacon % slot_durations.whole_slot;

    uint64_t time_to_skip_one_slot = 0;
    if (time_into_gateway_slot > slot_durations.whole_slot / 2) {
        // too close to the next slot, skip this one
        asn_count_since_beacon++;
        time_to_skip_one_slot = slot_durations.whole_slot;
    }

    uint64_t time_cpu_and_toa = 445;  // magic number: measured using the logic analyzer
    if (is_handover) {
        time_cpu_and_toa += 116;  // magic number: measured using the logic analyzer (why??)
    }

    uint32_t time_dispatch_new_schedule = ((slot_durations.whole_slot - time_into_gateway_slot) + time_to_skip_one_slot) - time_cpu_and_toa;
    mr_timer_hf_set_oneshot_us(
        MIRA_TIMER_DEV,
        MIRA_TIMER_CHANNEL_1,
        time_dispatch_new_schedule,
        &activity_scan_dispatch_new_schedule);

    // set the asn to match the gateway's
    mac_vars.asn = selected_gateway.beacon.asn + asn_count_since_beacon;

    return true;
}

static void activity_scan_start_frame(uint32_t ts) {
    set_slot_state(STATE_RX_DATA);
    mac_vars.current_scan_item_ts = ts;

    // NOTE: should probably set an error timer here, in case the end event doesn't happen
}

static void activity_scan_end_frame(uint32_t end_frame_ts) {
    uint8_t packet[MIRA_PACKET_MAX_SIZE];
    uint8_t packet_len;
    mr_radio_get_rx_packet(packet, &packet_len);

    mr_assoc_handle_beacon(packet, packet_len, MIRA_FIXED_SCAN_CHANNEL, mac_vars.current_scan_item_ts);

    // if there is still enough time before end of scan, re-enable the radio
    bool still_time_for_rx_scan    = mac_vars.is_scanning && (end_frame_ts + MIRA_BEACON_TOA_WITH_PADDING < mac_vars.scan_expected_end_ts);
    bool still_time_for_rx_bg_scan = mac_vars.is_bg_scanning && mac_vars.bg_scan_sleep_next_slot;
    if (still_time_for_rx_scan || still_time_for_rx_bg_scan) {
        // re-enable the radio, if there still time to scan more (conditions for normal / bg scan)
        set_slot_state(STATE_RX_DATA_LISTEN);
        // we cannot call rx immediately, because this runs in isr context/
        // and it might interfere with `if (NRF_RADIO->EVENTS_DISABLED)` in RADIO_IRQHandler
        mr_timer_hf_set_oneshot_with_ref_us(
            MIRA_TIMER_DEV,
            MIRA_TIMER_CHANNEL_2,
            end_frame_ts,
            20,  // arbitrary value, just to give some time for the radio to turn off
            &mr_radio_rx);
    } else {
        set_slot_state(STATE_SLEEP);
    }
}

// --------------------- tx/rx activities ------------

// --------------------- radio ---------------------
static void isr_mac_radio_start_frame(uint32_t ts) {
    DEBUG_GPIO_SET(&pin2);
    if (mac_vars.is_scanning || mac_vars.is_bg_scanning) {
        activity_scan_start_frame(ts);
        return;
    }

    switch (mac_vars.state) {
        case STATE_RX_DATA_LISTEN:
            activity_ri3(ts);
            break;
        default:
            break;
    }
}

static void isr_mac_radio_end_frame(uint32_t ts) {
    DEBUG_GPIO_CLEAR(&pin2);

    if (mac_vars.is_scanning || mac_vars.is_bg_scanning) {
        activity_scan_end_frame(ts);
        return;
    }

    switch (mac_vars.state) {
        case STATE_TX_DATA:
            activity_ti3();
            break;
        case STATE_RX_DATA:
            activity_ri4(ts);
            break;
        default:
            break;
    }
}
