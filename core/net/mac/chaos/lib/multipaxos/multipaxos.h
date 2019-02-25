/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2017 Beshr Al Nahas, Valentin Poirot and Olaf Landsiedel.
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
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************/
/*
 * \file
 *         Wireless Multi-Paxos library
 * \author
 *         Valentin Poirot <poirotv@chalmers.se>
 *         Beshr Al Nahas <beshr@chalmers.se>
 *         Olaf Landsiedel <olafl@chalmers.se>
 */

#ifndef _MULTIPAXOS_H_
#define _MULTIPAXOS_H_

#include "chaos-config.h"
#include "chaos.h"
#include "testbed.h"

/* Print more details about Wireless Multi-Paxos results */
#ifndef MULTIPAXOS_PRINT_DETAILS
#define MULTIPAXOS_PRINT_DETAILS 1
#endif

/* Advanced statistics:
  - (Synchrotron) flags evolution per slot,
  - local log of values
*/
#ifndef MULTIPAXOS_ADVANCED_STATISTICS
#define MULTIPAXOS_ADVANCED_STATISTICS 1
#endif

/* Wireless Multi-Paxos require a slot of 6 ms at least on Tmote Sky boards */
#define MULTIPAXOS_SLOT_LEN       \
    (6 * (RTIMER_SECOND / 1000) + \
     0 * (RTIMER_SECOND / 1000) / 4)  // 1 rtimer tick == 2*31.52 us

/* Define the maximal number of slots forming a Synchrotron round */
#ifndef MULTIPAXOS_ROUND_MAX_SLOTS
#warning "define MULTIPAXOS_ROUND_MAX_SLOTS"
#define MULTIPAXOS_ROUND_MAX_SLOTS (255)
#endif

/* Wireless Multi-Paxos slot length from number of ticks to VHT */
#define MULTIPAXOS_SLOT_LEN_DCO (MULTIPAXOS_SLOT_LEN * CLOCK_PHI)

/* Maximum number of rounds kept in the local log */
#ifndef MULTIPAXOS_LOG_SIZE
#define MULTIPAXOS_LOG_SIZE 8
#endif

/* Number of values that can be agreed upon with one Synchrotron round */
#ifndef MULTIPAXOS_PKT_SIZE
/* Cannot be higher than MULTIPAXOS_LOG_SIZE!!! */
#define MULTIPAXOS_PKT_SIZE 2
#endif

/* No-Operation special identifier
After a leader failure, a special no-op identifier can be inserted
Allows a correct realization of state machine replication
See "The Part Time Parliament" by L. Lamport
Also see "Paxos Made Simple" by L. Lamport */
#ifndef MULTIPAXOS_NO_OP
#define MULTIPAXOS_NO_OP \
    255 /* default 255, cannot be an accepted value by application */
#endif

/* Leader timeout.
A node proclaims itself as leader after this number of Synchrotron round
without a leader. */
#ifndef BECOME_LEADER_AFTER
#define BECOME_LEADER_AFTER 3 /* default 3 */
#endif

/* Wireless Paxos defines a proposal number as a "ballot".
A ballot is made of two elements:
  - round (MSB): "paxos round" competition, increased after each competition
loss
  - id (LSB): node id, used to have unique ballot numbers
*/
typedef union __attribute__((packed)) ballot_number_t_struct {
    uint16_t n;
    struct {
        uint16_t id : 8, round : 8;
    };
} ballot_number_t;

/* Wireless Paxos value type
The value is the actual data being agreed on
*/
typedef uint8_t multipaxos_value_t;
typedef uint16_t multipaxos_round_t;

/* Wireless Paxos defines three "phases":
  - MULTIPAXOS_INIT: a PAXOS_INIT packet is a heartbeat from Synchrotron
  initiator to allow any proposer to start a Paxos round
  - MULTIPAXOS_PREPARE: Paxos Prepare (1.a and 1.b) phase
  - MULTIPAXOS_ACCEPT: Paxos Accept (2.a and 2.b) phase
*/
enum { MULTIPAXOS_INIT = 0, MULTIPAXOS_PREPARE = 1, MULTIPAXOS_ACCEPT };

/* Wireless Multi-Paxos Packet struct:
Represents the data in packets */
typedef struct __attribute__((packed)) multipaxos_t_struct {
    /* The current ballot number (set by a proposer) */
    ballot_number_t ballot;
    /* the phase (INIT, PREPARE or ACCEPT), set by a proposer */
    uint8_t phase;
    /* In PREPARE phase, round is the lowest round with no accepted value.
    In ACCEPT phase, round is the current round nodes are agreeing on.
    */
    multipaxos_round_t round;
    /* In PREPARE phase, filled by acceptors with the highest round they
     * accepted a value for */
    multipaxos_round_t max_heard_round;
    /* In PREPARE phase, the array contains all values possibly accepted since
     round N until round last_round_participation In ACCEPT phase, only the
     first row is set, and contains the value proposed by the leader.
     */
    multipaxos_value_t values[MULTIPAXOS_PKT_SIZE];
    /* In PREPARE phase, filled by acceptors with the highest accepted proposal
     (=ballot) for this round. In ACCEPT phase, first element is filled by
     acceptors with the highest min proposal (=ballot).
     */
    ballot_number_t proposals[MULTIPAXOS_PKT_SIZE];
    /* Synchrotron flags */
    uint8_t flags[];
} multipaxos_t;

/* Wireless Multi-Paxos LEADER struct.
A leader in Multi-Paxos is equivalent to a proposer in Paxos.
*/
typedef struct __attribute__((packed)) leader_state_t_struct {
    /* Ballot number proposed by the proposer */
    /* Assumption: Ballot shall never be 0.0 !!!
    Trick : ballot.round starts with 1
    Reason: ballot 0.0 is used during prepare phase to allow acceptors to write
    previously accepted packets
    */
    ballot_number_t proposed_ballot;
    /* initially set by the application, is overwritten if an accepted value
     * exists in the system */
    multipaxos_value_t proposed_values[MULTIPAXOS_PKT_SIZE];
    /* Is this node a leader? */
    uint8_t is_leader;
    /* local phase (init, prepare, accept) */
    uint8_t phase;
    /* The round number set in the packet.
    Upon being elected as a leader, this is set to the lowest non-set round */
    multipaxos_round_t current_round;
    /* did this node got a majority of replies for this phase? */
    uint8_t got_majority;
    /* Wireless Multi-Paxos optimization:
    Execute iterative prepare phase if the packet size is limited.
     */
    uint8_t do_another_phase_1;
} leader_state_t;

/* Wireless Multi-Paxos ACCEPTOR struct */
typedef struct __attribute__((packed)) acceptor_state_t_struct {
    /* Assumption: Ballot shall never be 0.0 !!!
    Trick : ballot.round starts with 1
    Reason: ballot 0.0 is used during prepare phase to allow acceptors to write
    previously accepted packets
    */
    /* min_proposal: smallest proposal (=ballot) an acceptor can accept
    accepted_proposals: last proposal (=ballot) an acceptor accepted ( >=
    min_proposal) for each round
    */
    ballot_number_t min_proposal;
    ballot_number_t accepted_proposals[MULTIPAXOS_LOG_SIZE];
    /* Last value accepted by the acceptor for each round */
    multipaxos_value_t accepted_values[MULTIPAXOS_LOG_SIZE];
    /* Last round this node participated in */
    multipaxos_round_t last_round_participation;

} acceptor_state_t;

/* Wireless Multi-Paxos LEARNER struct */
typedef struct __attribute((packed)) learner_state_t_struct {
    /* Values agreed upon by the Wireless Multi-Paxos Consensus Algorithm */
    multipaxos_value_t learned_values[MULTIPAXOS_LOG_SIZE];
    /* Last round a value was chosen in */
    multipaxos_round_t last_round;
} learner_state_t;

/* Wireless Multi-Paxos internal struct */
typedef struct __attribute__((packed)) multipaxos_state_t_struct {
    /* leader internal struct */
    leader_state_t leader;
    /* acceptor internal struct */
    acceptor_state_t acceptor;
    /* learner internal struct */
    learner_state_t learner;
    /* Local aggregation variables
      - rx_min_proposal: highest min_proposal heard during an Accept phase
      - rx_accepted_proposala: highest accepted proposal heard during a Prepare
      phase for each round in packet
      - rx_accepted_value: value associated with rx_accepted_proposals for each
      round in packet
      - rx_max_round: highest round heard by any acceptor
    */
    ballot_number_t rx_min_proposal, rx_accepted_proposals[MULTIPAXOS_PKT_SIZE];
    multipaxos_value_t rx_accepted_values[MULTIPAXOS_PKT_SIZE];
    multipaxos_round_t rx_max_heard_round;

} multipaxos_state_t;

/* Start a new Wireless Multi-Paxos round
 * Input:
 *     round_number: Synchrotron round number
 *     app_id: Wireless Paxos app_id
 *     is_leader: 1 if this node should act as leader, 0 otherwise
 *     multipaxos_values: set the value leader will propose for this round, will be
 *          changed to locally accepted values final_flags: report the flags at the end of
 *          the round
 *     learned_values: Memory location to report chosen values
 * Output:
 *     success: returns 1 if a consensus was met and learned_values updated, 0 otherwise
*/
uint8_t multipaxos_round_begin(const uint16_t round_number, const uint8_t app_id,
                           uint8_t is_leader,
                           multipaxos_value_t multipaxos_values[],
                           multipaxos_value_t learned_values[],
                           uint8_t **final_flags);

/* Is Wireless Multi-Paxos running? */
int multipaxos_is_pending(const uint16_t round_count);

/* Report the total number of flags */
int multipaxos_get_flags_length(void);

/* Report the slot at which Synchrotron received all flags set for the first
 * time */
uint16_t multipaxos_get_completion_slot();

/* Report the slot at which Synchrotron went to off state */
uint16_t multipaxos_get_off_slot();

/* Populate the internal leader state */
void multipaxos_set_initial_leader_state();

/* Application-level function: Must be redefined by the application!
 * Should the node propose itself as a leader if the current leader has crashed?
 * Returns 1 if node proposes itself as new leader
 * Returns 0 otherwise
 */
uint8_t multipaxos_app_should_node_become_leader(multipaxos_state_t *multipaxos_state);

/* Get local structure for reporting */
const multipaxos_t *const multipaxos_get_local();

/* get Wireless Multi-Paxos internal state */
const multipaxos_state_t *const multipaxos_get_state();

/* If this node is leader, did he get a majority of accept responses? */
uint8_t multipaxos_leader_got_majority();

/* If this node is leader, did he get 100% of accept responses? */
uint8_t multipaxos_leader_got_network_wide_consensus();

/* Reset the leader to the previous round, to force adoption of the agreed values again */
void multipaxos_replay_last_consensus();

/* No-leader counter - allows new leader */
extern uint8_t not_heard_from_leader_since;

#if MULTIPAXOS_ADVANCED_STATISTICS
/* Number of flags set as locally seen by the node, for each Synchrotron slot */
extern uint8_t
    multipaxos_statistics_flags_evolution_per_slot[MULTIPAXOS_ROUND_MAX_SLOTS];
/* Final values in the log at the end of the Synchrotron round */
extern multipaxos_value_t
    multipaxos_statistics_values_in_log[MULTIPAXOS_LOG_SIZE];
#endif /* MULTIPAXOS_ADVANCED_STATISTICS */

#endif /* _MULTIPAXOS_H_ */
