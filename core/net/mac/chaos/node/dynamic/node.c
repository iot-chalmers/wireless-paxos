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
 *         Node manager
 * \author
 *         Beshr Al Nahas <beshr@chalmers.se>
 *         Olaf Landsiedel <olafl@chalmers.se>
 */

#include "contiki.h"
#include "chaos.h"
#include "join.h"
#include "node.h"
#include "net/mac/chaos/node/testbed.h"

volatile uint8_t chaos_node_index = 0;
volatile uint8_t chaos_node_count = 0;
volatile uint8_t chaos_has_node_index = 0;

#if NETSTACK_CONF_WITH_CHAOS_LEADER_ELECTION
volatile uint16_t initiator_node_id = 0;
#endif /* NETSTACK_CONF_WITH_CHAOS_LEADER_ELECTION */

volatile uint16_t stable_initiator = 0;

const uint16_t mapping[] = (uint16_t[])TESTBED_MAPPING;

void init_node_index(){
  join_init();
}

void set_stable_initiator(uint8_t x){
  stable_initiator = x;
}

uint8_t has_stable_initiator(){
  return INITIATOR_NODE_ID != 0 && stable_initiator != 0;
}

void update_initiator_stability_status(){
  if(INITIATOR_NODE_ID != 0 && chaos_node_count > MAX_NODE_COUNT/2){
    set_stable_initiator(1);
  }
}
