/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2018 Valentin Poirot and Beshr Al Nahas and Olaf Landsiedel.
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
 *         Wireless Paxos library
 * \author
 *         Valentin Poirot <poirotv@chalmers.se>
 *         Beshr Al Nahas <beshr@chalmers.se>
 *         Olaf Landsiedel <olafl@chalmers.se>
 */

#ifndef _PAXOS_H_
#define _PAXOS_H_

#include "chaos-config.h"
#include "chaos.h"
#include "testbed.h"

/* Advanced statistics:
 * - (Synchrotron) flags evolution per slot,
 * - Accepted value evolution per slot,
 * - Accepted proposal evolution per slot,
 * - Min proposal evolution per slot
 */
#ifndef PAXOS_ADVANCED_STATISTICS
#define PAXOS_ADVANCED_STATISTICS 0
#endif

/* Wireless Paxos require a slot of 5 ms at least on Tmote Sky boards */
#define PAXOS_SLOT_LEN (5 * (RTIMER_SECOND / 1000) + 0 * (RTIMER_SECOND / 1000) / 4)  // 1 rtimer tick == 2*31.52 us

/* Define the maximal number of slots forming a Synchrotron round */
#ifndef PAXOS_ROUND_MAX_SLOTS
#warning "define PAXOS_ROUND_MAX_SLOTS"
#define PAXOS_ROUND_MAX_SLOTS (255) /* default 255 */
#endif

/* Wireless Paxos slot length from number of ticks to VHT */
#define PAXOS_SLOT_LEN_DCO (PAXOS_SLOT_LEN * CLOCK_PHI)

/* Wireless Paxos defines a proposal number as a "ballot".
 * A ballot is made of two elements:
 *   - round (MSB): "paxos round" competition, increased after each competition
 *                  loss
 *   - id (LSB): node id, used to have unique ballot numbers
 */
typedef union __attribute__((packed)) ballot_number_t_struct {
  uint16_t n;
  struct {
    uint16_t id : 8, round : 8;
  };
} ballot_number_t;

/* Wireless Paxos value type
 * The value is the actual data being agreed on
 */
typedef uint8_t paxos_value_t;

/* Wireless Paxos defines three "phases":
 *   - PAXOS_INIT: a PAXOS_INIT packet is a heartbeat from Synchrotron initiator to
 *                 allow any proposer to start a Paxos round
 *   - PAXOS_PREPARE: Paxos Prepare (1.a and 1.b) phase
 *   - PAXOS_ACCEPT: Paxos Accept (2.a and 2.b) phase
 */
enum { PAXOS_INIT = 0, PAXOS_PREPARE = 1, PAXOS_ACCEPT };

/* Wireless Paxos Packet struct:
 * Represents the data in packets
 */
typedef struct __attribute__((packed)) paxos_t_struct {
  /* The current ballot number (set by a proposer) */
  ballot_number_t ballot;
  /* the phase (INIT, PREPARE or ACCEPT), set by a proposer */
  uint8_t phase;
  /* In PREPARE phase, value is the latest accepted value by a acceptor.
   * In phase ACCEPT, the value is set by the proposer
   */
  paxos_value_t value;
  /* In PREPARE phase, proposal corresponds to the highest proposal (ballot
   * number) ACCEPTED by an acceptor replying to a prepare request. In ACCEPT
   * phase, proposal corresponds to the latest proposal (ballot number) PREPARED
   * by an acceptor replying to an accept request
   */
  ballot_number_t proposal;
  /* Synchrotron flags */
  uint8_t flags[];
} paxos_t;

/* Wireless Paxos PROPOSER struct */
typedef struct __attribute__((packed)) proposer_state_t_struct {
  /* Ballot number proposed by the proposer
   * Assumption: Ballot shall never be 0.0 !!!
   * Trick : ballot.round starts with 1
   * Reason: ballot 0.0 is used during prepare phase to allow acceptors to write
   * previously accepted packets
   */
  ballot_number_t proposed_ballot;
  /* initially set by the application, is overwritten if an accepted value
   * exists in the system
   */
  paxos_value_t proposed_value;
  /* is this node a proposer? */
  uint8_t is_proposer;
  /* local phase (init, prepare, accept) */
  uint8_t phase;
  /* did this node got a majority of replies in Accept phase? */
  uint8_t got_majority;
  /* used to report slot at which majority of replies were received for Accept
   * phase
   */
  uint16_t got_majority_at_slot;
  /* Competition backoff, allows another proposer to propose its value */
  uint8_t loser_timeout;
} proposer_state_t;

/* Wireless Paxos ACCEPTOR struct */
typedef struct __attribute__((packed)) acceptor_state_t_struct {
  /* Assumption: Ballot shall never be 0.0 !!!
   * Trick : ballot.round starts with 1
   * Reason: ballot 0.0 is used during prepare phase to allow acceptors to write
   * previously accepted packets
   */
  /* min_proposal: smallest proposal (=ballot) an acceptor can accept
   * accepted_proposal: last proposal (=ballot) an acceptor accepted
   * ( >= min_proposal)
   */
  ballot_number_t min_proposal, accepted_proposal;
  /* Last value accepted by the acceptor */
  paxos_value_t accepted_value;

} acceptor_state_t;

/* Wireless Paxos LEARNER struct */
typedef struct __attribute__((packed)) paxos_learner_t_struct {
  /* Value agreed upon by the Wireless Paxos Consensus Algorithm */
  paxos_value_t learned_value;
} learner_state_t;

/* Wireless Paxos internal struct */
typedef struct __attribute__((packed)) paxos_state_t_struct {
  /* proposer internal struct */
  proposer_state_t proposer;
  /* acceptor internal struct */
  acceptor_state_t acceptor;
  /* learner internal struct */
  learner_state_t learner;
  /* Local aggregation variables
   *   - rx_min_proposal: highest min_proposal heard during an Accept phase
   *   - rx_accepted_proposal: highest accepted proposal heard during a Prepare
   *                           phase
   *   - rx_accepted_value: value associated with rx_accepted_proposal
   */
  ballot_number_t rx_min_proposal, rx_accepted_proposal;
  paxos_value_t rx_accepted_value;

} paxos_state_t;

/* Start a new Wireless Paxos round
 * Input:
 *     round_number: Synchrotron round number
 *     app_id: Wireless Paxos app_id
 *     is_proposer: 1 if this node should act as proposer, 0 otherwise
 *     paxos_value: set the value proposer will propose for this round, will be
 *      changed to the locally accepted value final_flags: report the flags at the end
 *      of the round 
 * Output:
 *     learned_value: return a value if a proposal was accepted by
 *      a majority of node, return 0 otherwise
 */
uint8_t paxos_round_begin(const uint16_t round_number, const uint8_t app_id, uint8_t is_proposer, paxos_value_t* paxos_value,
                      uint8_t** final_flags);

/* Is Wireless Paxos running? */
int paxos_is_pending(const uint16_t round_count);

/* Report the total number of flags */
int paxos_get_flags_length(void);

/* Report the slot at which Synchrotron received all flags set for the first
 * time 
 */
uint16_t paxos_get_completion_slot();

/* Report the slot at which Synchrotron went to off state */
uint16_t paxos_get_off_slot();

/* If this node is a proposer, did he get a majority of accept responses? */
uint8_t paxos_proposer_got_majority();

/* If this node is a proposer, did he get 100% of accept responses? */
uint8_t paxos_proposer_got_network_wide_consensus();

/* Get local structure for reporting */
const paxos_t* const paxos_get_local();

/* get Wireless Paxos internal state */
const paxos_state_t* const paxos_get_state();

/* reset wireless Paxos internal state */
void paxos_reset_state();

/* Report the slot at which Synchrotron went to off state */
paxos_value_t paxos_get_learned_value();

#if PAXOS_ADVANCED_STATISTICS
/* Number of flags set as locally seen by the node, for each Synchrotron slot */
extern uint8_t paxos_statistics_flags_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS];
/* Locally saved accepted value, for each Synchrotron slot */
extern uint8_t paxos_statistics_value_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS];
/* Locally saved min proposal, for each Synchrotron slot */
/* 0 means the min proposal hasn't changed since the previous slot */
extern uint16_t paxos_statistics_min_proposal_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS];
/* Locally saved accepted proposal, for each Synchrotron slot */
/* 0 means the min proposal hasn't changed since the previous slot */
extern uint16_t paxos_statistics_accepted_proposal_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS];
#endif /* PAXOS_ADVANCED_STATISTICS */

#endif /* _PAXOS_H_ */
