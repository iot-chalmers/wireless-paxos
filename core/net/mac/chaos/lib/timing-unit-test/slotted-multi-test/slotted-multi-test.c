/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2017 Beshr Al Nahas and Olaf Landsiedel.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************/
/*
 * \file
 *         Slotted-multi-test library
 * \author
 *         Beshr Al Nahas <beshr@chalmers.se>
 *         Olaf Landsiedel <olafl@chalmers.se>
 */

#include "contiki.h"
#include <string.h>

#include "node.h"
#include "chaos.h"
#include "slotted-multi-test.h"

#undef ENABLE_COOJA_DEBUG
#define ENABLE_COOJA_DEBUG COOJA
#include "dev/cooja-debug.h"

#ifndef CHAOS_CONCURRENT_TX_COUNT
#define CHAOS_CONCURRENT_TX_COUNT 2
#endif

static int got_valid_rx;

static chaos_state_t process(uint16_t round_count, uint16_t slot_count, chaos_state_t current_state, int rx_valid, size_t payload_length, uint8_t* rx_payload, uint8_t* tx_payload, uint8_t** app_flags){
  if( current_state == CHAOS_RX && rx_valid ){
    memcpy((void *)tx_payload, (void *)rx_payload, payload_length);
    got_valid_rx = 1;
  }
  chaos_state_t next_state = CHAOS_RX;
  uint8_t concurrent_tx = chaos_node_count / CHAOS_CONCURRENT_TX_COUNT;
  if( (IS_INITIATOR( ) && current_state == CHAOS_INIT) || ( (IS_INITIATOR( ) || got_valid_rx)
      && ((slot_count % concurrent_tx) == (chaos_node_index % concurrent_tx)) ) ){
    next_state = CHAOS_TX;
  }
  *app_flags = NULL;
  return next_state;
}

int slotted_multi_test_is_pending(const uint16_t round_count){
  return 1;
}

static int slotted_multi_test_get_flags_length() {
  return 0;
}

void slotted_multi_test_round_begin(const uint16_t round_number, const uint8_t app_id){
  got_valid_rx = 0;

  if( IS_INITIATOR() ){
    const char* const msg = "hi there!1234567890+qwertyuiopasdfghjklzxcvbnm,QWERTYUIOPASDFGHJKLZXCVBNM0987665554444443211234567890+";
    chaos_round(round_number, app_id, (const uint8_t const*)msg, strlen(msg), SLOTTED_MULTI_TEST_SLOT_LEN_DCO, SLOTTED_MULTI_TEST_ROUND_MAX_SLOTS, slotted_multi_test_get_flags_length(), process);
  } else {
    chaos_round(round_number, app_id, NULL, 0, SLOTTED_MULTI_TEST_SLOT_LEN_DCO, SLOTTED_MULTI_TEST_ROUND_MAX_SLOTS, slotted_multi_test_get_flags_length(), process);
  }
}

