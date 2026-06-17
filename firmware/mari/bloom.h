#ifndef __BLOOM_H
#define __BLOOM_H

/**
 * @ingroup     mari
 * @brief       Bloom filter header file
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

//=========================== defines =========================================

#define MARI_BLOOM_M_BITS   1024
#define MARI_BLOOM_M_BYTES  (MARI_BLOOM_M_BITS / 8)
#define MARI_BLOOM_K_HASHES 2

#define MARI_BLOOM_FNV1A_H2_SALT 0x5bd1e995

//=========================== variables =======================================

//=========================== prototypes ======================================

uint64_t mr_bloom_hash_fnv1a64(uint64_t input);

void    mr_bloom_gateway_init(void);
void    mr_bloom_gateway_set_dirty(void);
void    mr_bloom_gateway_set_clean(void);
bool    mr_bloom_gateway_is_dirty(void);
bool    mr_bloom_gateway_is_available(void);
uint8_t mr_bloom_gateway_copy(uint8_t *output);
void    mr_bloom_gateway_compute(void);
void    mr_bloom_gateway_event_loop(void);

bool mr_bloom_node_contains(uint64_t node_id, const uint8_t *bloom);
bool mr_bloom_is_empty(const uint8_t *bloom);

#endif  // __BLOOM_H
