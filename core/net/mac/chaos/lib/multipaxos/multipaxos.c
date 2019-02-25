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
 *         Wireless Multi-Paxos library
 * \author
 *         Valentin Poirot <poirotv@chalmers.se>
 *         Beshr Al Nahas <beshr@chalmers.se>
 *         Olaf Landsiedel <olafl@chalmers.se>
 */

#include <string.h>
#include "contiki.h"

#include "chaos-config.h"
#include "chaos-random-generator.h"
#include "chaos.h"
#include "multipaxos.h"
#include "node.h"

#undef ENABLE_COOJA_DEBUG
#define ENABLE_COOJA_DEBUG COOJA
#include "dev/cooja-debug.h"

#ifndef COMMIT_THRESHOLD
#define COMMIT_THRESHOLD 6
#endif

#define LIMIT_TX_NO_DELTA 0

#define FLAGS_LEN_X(X) (((X) >> 3) + (((X)&7) ? 1 : 0))
#define FLAGS_LEN (FLAGS_LEN_X(chaos_node_count))
#define LAST_FLAGS ((1 << (((chaos_node_count - 1) & 7) + 1)) - 1)
#define FLAG_SUM (((FLAGS_LEN - 1) << 8) - (FLAGS_LEN - 1) + LAST_FLAGS)

#if NETSTACK_CONF_WITH_CHAOS_NODE_DYNAMIC
#define FLAGS_ESTIMATE FLAGS_LEN_X(MAX_NODE_COUNT)
#warning "APP: due to packet size limitation: maximum network size = MAX_NODE_COUNT"
#else
#define FLAGS_ESTIMATE FLAGS_LEN_X(CHAOS_NODES)
#endif

static uint8_t bit_count(uint8_t u) { return (u - (u >> 1) - (u >> 2) - (u >> 3) - (u >> 4) - (u >> 5) - (u >> 6) - (u >> 7)); }

#if MULTIPAXOS_ADVANCED_STATISTICS
/* Number of flags set as locally seen by the node, for each Synchrotron slot */
uint8_t multipaxos_statistics_flags_evolution_per_slot[MULTIPAXOS_ROUND_MAX_SLOTS] = {0};
/* Local Multi-Paxos log of accepted values */
multipaxos_value_t multipaxos_statistics_values_in_log[MULTIPAXOS_LOG_SIZE] = {0};
#endif /* MULTIPAXOS_ADVANCED_STATISTICS */

/* Local memory struct of Wireless Multi-Paxos
 * multipaxos:    clean paxos_t instance, used to report results
 * flags:    all flags are unset, except the local flag
 */
typedef struct __attribute__((packed)) multipaxos_t_local_struct {
  multipaxos_t multipaxos;
  uint8_t flags[FLAGS_ESTIMATE];
} multipaxos_t_local;

/* Counter to start new leader */
uint8_t not_heard_from_leader_since = 0;

/* Should we transmit during the next slot */
static int tx = 0;
/* are all flags set */
static int complete = 0;
/* Slot at which all flags were set for the first time,
Slot at which we stopped participating in the round */
static uint16_t completion_slot, off_slot;
/* Number of times we have TX after receiving a packet with all flags set */
static int tx_count_complete = 0;
/* How many times did we have invalid RX in a row */
static int invalid_rx_count = 0;
/* RX was valid at this slot */
static int got_valid_rx = 0;
/* Number of flags set at this slot */
static uint16_t n_replies = 0;
/* Did we learn a chosen value this round? */
static uint8_t values_chosen_this_round = 0;
/* Timeout since last reception before TX again */
static unsigned short restart_threshold;
/* Used to report final values */
static multipaxos_t_local multipaxos_local;
/* Current state of the Wireless Multi-Paxos algorithm */
static multipaxos_state_t multipaxos_state;
/* Current flags */
static uint8_t *multipaxos_flags;

/* Main function, called at each slot */
static chaos_state_t process(uint16_t round_count, uint16_t slot_count, chaos_state_t current_state, int chaos_txrx_success,
                             size_t payload_length, uint8_t *rx_payload, uint8_t *tx_payload, uint8_t **app_flags) {
  /* payload for the TX packet, possibly set with the last TX data */
  multipaxos_t *tx_multipaxos = (multipaxos_t *)tx_payload;
  /* payload for the last RX packet */
  multipaxos_t *rx_multipaxos = (multipaxos_t *)rx_payload;
  /* payload is set to the RX payload if we were listening, or the TX if we
   * transmitted last slot
   */
  multipaxos_t *payload = rx_multipaxos;
  if (current_state == CHAOS_TX) {
    payload = tx_multipaxos;
  }
  /* Is the RX packet containing novel information */
  uint8_t rx_delta = 0;
  /* Should we transmit next time */
  tx = 0;
  n_replies = 0;

  if (chaos_txrx_success                                                          /* Last slot was successful */
      && (current_state == CHAOS_RX                                               /* and we were listening during this slot */
          || (current_state == CHAOS_TX && multipaxos_state.leader.is_leader))) { /* or we are a leader and were TX this slot */

    /* Reception was correct for this slot */
    got_valid_rx = 1;

    /* TODO WHY??? */
    /* Reset tx_multipaxos (possibly populated with data from previous Synchrotron round) */
    if (!got_valid_rx && !multipaxos_state.leader.is_leader) {
      memset(tx_multipaxos, 0, sizeof(multipaxos_t) + FLAGS_LEN);
    }

    /* a PAXOS_INIT packet is a heartbeat from Synchrotron initiator to
     * allow any proposer to start a Paxos round
     */
    /* if min_proposal is 0, we haven't received any Paxos request yet */
    if (payload->phase == MULTIPAXOS_INIT) {
      /* ----- BEGIN LEADER - INITIATE PAXOS ALGORITHM (1/1) */
      if (multipaxos_state.leader.is_leader) {
        /* reset flags */
        memcpy(tx_multipaxos->flags, multipaxos_local.multipaxos.flags, FLAGS_LEN);

        /* ----- BEGIN LEADER LOGIC - PREPARE PHASE ------ */
        if (multipaxos_state.leader.phase == MULTIPAXOS_INIT) {
          tx_multipaxos->round = multipaxos_state.leader.current_round;
          multipaxos_state.leader.phase = MULTIPAXOS_PREPARE;
          tx_multipaxos->ballot.n = multipaxos_state.leader.proposed_ballot.n;
          tx_multipaxos->phase = MULTIPAXOS_PREPARE;
          multipaxos_state.acceptor.min_proposal.n = multipaxos_state.leader.proposed_ballot.n;  // TODO: check if useless or not
          multipaxos_state.leader.got_majority = 0;
        }
        /* ----- END LEADER LOGIC - PREPARE PHASE ------ */
        else { /* Leader has already sent a Prepare phase once before */
          /* set correct information in packet */
          tx_multipaxos->round = multipaxos_state.leader.current_round;
          tx_multipaxos->phase = multipaxos_state.leader.phase;
          /* We got the majority last time, we can write the new round
           * number, the new values and reset acceptor fields to 0
           */
          uint8_t i;
          if (multipaxos_state.leader.got_majority) {
            /* for each round, insert new value */
            for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
              tx_multipaxos->values[i] = multipaxos_state.leader.proposed_values[i];
              tx_multipaxos->proposals[i].n = 0;
            }
          } else {
            /* If we didn't have a majority, then we must continue
             * with the actual content
             */
          }
          multipaxos_state.leader.got_majority = 0;
        }
        /* force transmit */
        tx = 1;
      } else if (tx_multipaxos->ballot.n == 0) { /* Not a leader and did not hear from any
                                                   during that round */
        /* ----- BEGIN ACCEPTOR - INIT HEARTBEAT */
        /* If not a leader, just propagate the heartbeat with your flag */
        memcpy(tx_multipaxos, payload, sizeof(multipaxos_t));
        uint16_t flag_sum = 0;
        uint8_t i;
        for (i = 0; i < FLAGS_LEN; i++) {
          tx |= (payload->flags[i] != tx_multipaxos->flags[i]);
          tx_multipaxos->flags[i] |= payload->flags[i];
          flag_sum += tx_multipaxos->flags[i];
        }
        if (flag_sum >= FLAG_SUM) {
          complete = 1;
        }
        /* ----- END ACCEPTOR - INIT HEARTBEAT */
      }
      /* is this packet novel? */
      rx_delta = tx;

    } else {
      /* ----- BEGIN ACCEPTOR LOGIC ------ */

      /* New packet is possibly newer (strictly higher ballot or current
       * ballot, strictly higher round or current round, strictly higher
       * phase or current phase
       */
      if (payload->ballot.n > tx_multipaxos->ballot.n ||
          (payload->ballot.n == tx_multipaxos->ballot.n && payload->round > tx_multipaxos->round) ||
          (payload->ballot.n == tx_multipaxos->ballot.n && payload->round == tx_multipaxos->round &&
           payload->phase >= tx_multipaxos->phase)) {
        /* at least one leader is present */
        not_heard_from_leader_since = 0;
        /* A packet is new if it contains a strictly higher ballot or
         * strictly higher round if same ballot or a strictly higher
         * phase if same ballot and round
         */
        uint8_t new_phase = 0;
        new_phase = !(payload->ballot.n == tx_multipaxos->ballot.n && payload->phase == tx_multipaxos->phase &&
                      payload->round == tx_multipaxos->round); /* detect if exactly the same ballot
                                                                  & phase or something changed */
        if (new_phase) {                                       /* if new phase, local infos for merging must
                                                                  be discarded */
          memcpy(tx_multipaxos, payload, sizeof(multipaxos_t) + FLAGS_LEN);
          memset(&multipaxos_state.rx_accepted_proposals, 0, sizeof(multipaxos_state.rx_accepted_proposals));
          memset(&multipaxos_state.rx_accepted_values, 0, sizeof(multipaxos_state.rx_accepted_values));
          memset(&multipaxos_state.rx_max_heard_round, 0, sizeof(multipaxos_state.rx_max_heard_round));
          memset(&multipaxos_state.rx_min_proposal, 0, sizeof(multipaxos_state.rx_min_proposal));
        }

        /* ----- BEGIN ACCEPTOR LOGIC - PREPARE PHASE ------ */

        if (payload->phase == MULTIPAXOS_PREPARE) {
          if (payload->ballot.n > multipaxos_state.acceptor.min_proposal.n) /* Higher ballot received */
            multipaxos_state.acceptor.min_proposal.n = payload->ballot.n;
          /* Save the highest round in which the acceptor participated */
          multipaxos_state.rx_max_heard_round = MAX(payload->max_heard_round, multipaxos_state.rx_max_heard_round);
          multipaxos_state.rx_max_heard_round =
              MAX(multipaxos_state.acceptor.last_round_participation, multipaxos_state.rx_max_heard_round);
          tx_multipaxos->max_heard_round = multipaxos_state.rx_max_heard_round;
          /* Must reply with accepted_proposal and all values that got
           * accepted since round R
           */
          uint8_t i;
          /* for each value "slot" within packet */
          for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
            /* check if we have participated in this round, and if the local proposal is higher */
            if ((payload->round + i) <= multipaxos_state.acceptor.last_round_participation &&
                multipaxos_state.acceptor.accepted_proposals[(payload->round + i) % MULTIPAXOS_LOG_SIZE].n >
                    multipaxos_state.rx_accepted_proposals[i].n) {
              /* save local proposal into local aggregation variable */
              multipaxos_state.rx_accepted_proposals[i].n =
                  multipaxos_state.acceptor.accepted_proposals[(payload->round + i) % MULTIPAXOS_LOG_SIZE].n;
              multipaxos_state.rx_accepted_values[i] =
                  multipaxos_state.acceptor.accepted_values[(payload->round + i) % MULTIPAXOS_LOG_SIZE];
            }
            /* save local aggregation variable into packet */
            if (payload->proposals[i].n < multipaxos_state.rx_accepted_proposals[i].n) { /* We accepted a higher ballot before */
              tx_multipaxos->proposals[i].n = multipaxos_state.rx_accepted_proposals[i].n;
              tx_multipaxos->values[i] = multipaxos_state.rx_accepted_values[i];
              tx = rx_delta = 1; /* force transmit */
            } else {             /* save packet into local aggregation variable */
              multipaxos_state.rx_accepted_proposals[i].n = payload->proposals[i].n;
              multipaxos_state.rx_accepted_values[i] = payload->values[i];
            }
          }
          /* ----- END ACCEPTOR LOGIC - PREPARE PHASE ------ */

          /* ----- BEGIN ACCEPTOR LOGIC - ACCEPT PHASE ------ */

        } else if (payload->phase == MULTIPAXOS_ACCEPT) {
          if (payload->ballot.n >= multipaxos_state.acceptor.min_proposal.n) { /* Any ballot equal or higher to min_proposal
                                                                                */
            /* we must nullify every previous accepted values from
             * the last round participated until this one (in case
             * we missed some of them
             */
            uint8_t i;
            for (i = payload->round; i > multipaxos_state.acceptor.last_round_participation; --i) {
              multipaxos_state.acceptor.accepted_proposals[(i) % MULTIPAXOS_LOG_SIZE].n = 0;
              multipaxos_state.acceptor.accepted_values[(i) % MULTIPAXOS_LOG_SIZE] = 0;
            }
            /* Accept the value for this current round */
            multipaxos_state.acceptor.min_proposal.n = payload->ballot.n;
            for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
              multipaxos_state.acceptor.accepted_proposals[(payload->round + i) % MULTIPAXOS_LOG_SIZE].n =
                  multipaxos_state.acceptor.min_proposal.n;
              multipaxos_state.acceptor.accepted_values[(payload->round + i) % MULTIPAXOS_LOG_SIZE] =
                  payload->values[i]; /* we write it to the log
                                         only once a majority of
                                         votes are present */
            }
            /* save last round participation */
            multipaxos_state.acceptor.last_round_participation =
                MAX(multipaxos_state.acceptor.last_round_participation, payload->round + MULTIPAXOS_PKT_SIZE - 1);
          }

          /* Aggregation logic */
          multipaxos_state.rx_min_proposal.n = MAX(multipaxos_state.acceptor.min_proposal.n, multipaxos_state.rx_min_proposal.n);
          multipaxos_state.rx_min_proposal.n = MAX(payload->proposals[0].n, multipaxos_state.rx_min_proposal.n);

          if (payload->proposals[0].n < multipaxos_state.rx_min_proposal.n) { /* If a higher ballot have been heard,
                                                                                 then it must be put in the packet */
            tx_multipaxos->proposals[0].n = multipaxos_state.rx_min_proposal.n;
            tx = rx_delta = 1;
          }

          /* ----- END ACCEPTOR LOGIC - ACCEPT PHASE ------ */

        } else {
/* ERROR! Phase should be PAXOS_INIT, PAXOS_PREPARE or PAXOS_ACCEPT, and nothing else */
#if COOJA
          COOJA_DEBUG_STR("ACCEPTOR rcvd AN UNKNOWN PHASE!!");
#endif
        }

        /* ----- BEGIN TRANSMISSION LOGIC ------ */
        uint16_t flag_sum = 0;
        uint16_t i;
        if (!new_phase) {
          /* Set Synchrotron participation (progress) flags */
          for (i = 0; i < FLAGS_LEN; i++) { /* not a new phase, merge heard flags with the
                                               last flags we transmitted */
            n_replies += bit_count(tx_multipaxos->flags[i]);
            tx |= (payload->flags[i] != tx_multipaxos->flags[i]); /* transmit only if
                                                                     something new */
            tx_multipaxos->flags[i] |= payload->flags[i];
            flag_sum += tx_multipaxos->flags[i];
          }
        } else if (new_phase) {
          /* new phase received, flags have been copied already */
          for (i = 0; i < FLAGS_LEN; i++) {
            /* count the number of flags set (used to detect majority) */
            n_replies += bit_count(tx_multipaxos->flags[i]);
            /* used to detect if all flags are set */
            flag_sum += tx_multipaxos->flags[i];
            /* since it's a new phase, retransmit */
            tx = 1;
          }
          /* set my flag */
          unsigned int array_index = chaos_node_index / 8;
          unsigned int array_offset = chaos_node_index % 8;
          tx_multipaxos->flags[array_index] |= 1 << (array_offset);
        }
        /* novel information within packet? */
        rx_delta |= tx;

        /* Wireless Paxos optimization:
         * We can have a Quorum Read for 'free' simply by reading the
         * number of flags
         */
        if (payload->phase == MULTIPAXOS_ACCEPT && n_replies > chaos_node_count / 2) /* We are in phase ACCEPT (2) and we know
                                                                                        that a majority accepted the value*/
        {
          values_chosen_this_round = 1;
          /* we write the value into the log of chosen values */
          uint8_t i;
          for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
            multipaxos_state.learner.learned_values[(payload->round + i) % MULTIPAXOS_LOG_SIZE] = payload->values[i];
          }
          /* save the last time a value was chosen */
          multipaxos_state.learner.last_round = payload->round + MULTIPAXOS_PKT_SIZE - 1;
        }

        /* check if Synchrotron has converged */
        if (payload->phase == MULTIPAXOS_ACCEPT && flag_sum >= FLAG_SUM) { /* Chaos round is complete only for
                                                                              phase ACCEPT (2) */
          /* force transmit to end chaos round faster */
          tx = 1;
          if (!complete) { /* store when we reach completion */
            completion_slot = slot_count;
            complete = 1;
          }
        }
      } else { /* endif packet is "new" or current proposal */
        /* teach the higher ballot to the guys who sent the lower ballot */
        tx = 1;
      }
      /* ----- END ACCEPTOR LOGIC ------ */

      /* ----- BEGIN LEADER LOGIC ------ */
      if (multipaxos_state.leader.is_leader && !multipaxos_state.leader.got_majority) {
        /* lost_proposal indicates that another leader is present
         * update_phase == 1 means we iterate back from accept phase to prepare phase to learn previous rounds (see paper)
         * update_phase == 2 means we transition from prepare to accept phase
         */
        uint8_t lost_proposal = 0, update_phase = 0;

        /* my ballot, I am the current leader */
        if (payload->ballot.n == multipaxos_state.leader.proposed_ballot.n) {
          /* correct phase & round */
          if (payload->phase == multipaxos_state.leader.phase && payload->round == multipaxos_state.leader.current_round) {

            /* ----- BEGIN LEADER LOGIC - PREPARE PHASE ------ */
            if (multipaxos_state.leader.phase == MULTIPAXOS_PREPARE) {
              /* since the acceptor logic is done before, we can
               * use the rx_proposal as correct values
               */

              /* We must write the already accepted values into
               * memory and put a special NO-OP command if there is
               * non decided value before any decided value
               * We iterate through what we received and put NO-OP
               * when we see a NULL value
               */
              int i, any_value_accepted = 0;
              /* we go from the last towards the first to detect missing rounds */
              for (i = MIN(MULTIPAXOS_PKT_SIZE - 1, payload->max_heard_round - payload->round); i >= 0; --i) {
                /* A leader with higher proposal is around */
                if (multipaxos_state.rx_accepted_proposals[i].n > multipaxos_state.leader.proposed_ballot.n) {
                  lost_proposal = 1;
                }
                /* some value has been accepted in the past */
                if (multipaxos_state.rx_accepted_proposals[i].n != 0) {
                  multipaxos_state.leader.proposed_values[i] = multipaxos_state.rx_accepted_values[i];
                  any_value_accepted = 1;
                } else if (any_value_accepted) {
                  /* no value accepted for this round, but a round has been done AFTER this round, insert NO_OP */
                  multipaxos_state.leader.proposed_values[i] = MULTIPAXOS_NO_OP;
                }
              }
              /* If an acceptor has participated in max_heard_round, but we cannot put
               * all values in this packet, we need to iterate (see paper)
               */
              if (payload->max_heard_round > payload->round + MULTIPAXOS_PKT_SIZE - 1) {
                multipaxos_state.leader.do_another_phase_1 = 1;
              } else {
                multipaxos_state.leader.do_another_phase_1 = 0;
              }

              /* if majority => switch to next phase */
              if (!lost_proposal && n_replies > chaos_node_count / 2) {
                multipaxos_state.leader.phase = MULTIPAXOS_ACCEPT;
                update_phase = 2; /*from phase 1 to 2 */
              }
            }
            /* ----- END LEADER LOGIC - PREPARE PHASE ------ */


            /* ----- BEGIN LEADER LOGIC - ACCEPT PHASE ------ */
            else if (multipaxos_state.leader.phase == MULTIPAXOS_ACCEPT) {
              if (multipaxos_state.rx_min_proposal.n > multipaxos_state.leader.proposed_ballot.n) {
                lost_proposal = 1;
              }

              /* check majority */
              if (!lost_proposal && n_replies > chaos_node_count / 2) {
                if (!multipaxos_state.leader.got_majority) {
                  multipaxos_state.leader.got_majority = 1;
                  /* save next round */
                  multipaxos_state.leader.current_round += MULTIPAXOS_PKT_SIZE;
                  if (multipaxos_state.leader.do_another_phase_1) {
                    /* go back to phase 1 */
                    multipaxos_state.leader.phase = MULTIPAXOS_PREPARE;
                    update_phase = 1; /*from phase 2 to 1 */
                  }
                }
              } /* end majority */
            }
            /* ----- END LEADER LOGIC - ACCEPT PHASE ------ */


            /* transmit if new phase */
            if (tx_multipaxos->phase != multipaxos_state.leader.phase) {
              rx_delta = 1;
            }

          } else { /* endif our ballot, our phase and our round */
            tx = 1;

            if (payload->phase > multipaxos_state.leader.phase) {
              /* ERROR! phase should'nt be higher for same ballot */
              #if COOJA
                COOJA_DEBUG_STR("LEADER rcvd A HIGHER PHASE!!");
              #endif
            }
          }

        /* higher ballot, we lost */
        } else if (payload->ballot.n > multipaxos_state.leader.proposed_ballot.n && !multipaxos_state.leader.got_majority) {
          lost_proposal = 1;
        } else { /* endif our ballot or higher ballot */
          /* discard old packet, retransmit our own (higher) proposal */
          tx = 1;
        }


        /* We lost, we shall stop being a leader */
        if (lost_proposal) {
          multipaxos_state.leader.is_leader = 0; /* not a leader anymore */
        }


        /* ----- BEGIN LEADER - PHASE TRANSITION ----- */
        if (update_phase) {
          /* populate the packet memory with correct information */
          tx_multipaxos->ballot.n = multipaxos_state.leader.proposed_ballot.n;
          tx_multipaxos->phase = multipaxos_state.leader.phase;
          /* must reset merging memory too */
          memset(&multipaxos_state.rx_accepted_proposals, 0, sizeof(multipaxos_state.rx_accepted_proposals));
          memset(&multipaxos_state.rx_accepted_values, 0, sizeof(multipaxos_state.rx_accepted_values));
          memset(&multipaxos_state.rx_max_heard_round, 0, sizeof(multipaxos_state.rx_max_heard_round));
          memset(&multipaxos_state.rx_min_proposal, 0, sizeof(multipaxos_state.rx_min_proposal));
          if (update_phase == 1) /* from Accept to Prepare (iterative, see paper) */
          {
            /* set the lowest round we are asking for */
            tx_multipaxos->round = multipaxos_state.leader.current_round;
            /* set the max heard round to the lowest round we are
             * asking
             */
            tx_multipaxos->max_heard_round = tx_multipaxos->round;
            /* reset acceptors' fields */
            int i;
            for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
              tx_multipaxos->values[i] = 0;
              multipaxos_state.leader.proposed_values[i] = 0;
              tx_multipaxos->proposals[i].n = 0;
            }
            multipaxos_state.leader.got_majority = 0;
          } else if (update_phase == 2) /* from Prepare to Accept */
          {
            tx_multipaxos->proposals[0].n = 0; /* only the first one is used */
            /* populate packet with values to accept */
            int i;
            for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
              tx_multipaxos->values[i] = multipaxos_state.leader.proposed_values[i];
            }
            multipaxos_state.leader.got_majority = 0;
          }
          /* reset flags and set my flag only */
          memcpy(tx_multipaxos->flags, multipaxos_local.multipaxos.flags, FLAGS_LEN);
          /* update complete */
          update_phase = 0;
          /* force transmit */
          tx = 1;
        }
        /* ----- END LEADER - PHASE TRANSITION ----- */

      }
      /* ----- END LEADER LOGIC ------ */

    } /* endif NOT INIT heartbeat */

  } /* endif correct RX */

  /* ----- BEGIN SYNCHROTRON STATE LOGIC ------ */
  chaos_state_t next_state = CHAOS_RX;

  /* beginning of a Synchrotron round */
  if (IS_INITIATOR() && current_state == CHAOS_INIT) {
    next_state = CHAOS_TX;
    got_valid_rx = 1; /* enables retransmissions */

    /* ----- BEGIN LEADER LOGIC - if leader is also initiator ------ */
    if (multipaxos_state.leader.is_leader) {
      memcpy(tx_multipaxos->flags, multipaxos_local.multipaxos.flags, FLAGS_LEN);

      /* ----- BEGIN LEADER LOGIC - PREPARE PHASE ------ */
      if (multipaxos_state.leader.phase == MULTIPAXOS_INIT) {
        tx_multipaxos->round = multipaxos_state.leader.current_round;
        multipaxos_state.leader.phase = MULTIPAXOS_PREPARE;
        tx_multipaxos->ballot.n = multipaxos_state.leader.proposed_ballot.n;
        tx_multipaxos->phase = MULTIPAXOS_PREPARE;
        multipaxos_state.acceptor.min_proposal.n = multipaxos_state.leader.proposed_ballot.n;  // TODO: check if useless or not
        multipaxos_state.leader.got_majority = 0;
      }
      /* ----- END LEADER LOGIC - PREPARE PHASE ------ */

      /* we have started proposing in the past, repopulate memory with correct content */
      else {
        tx_multipaxos->round = multipaxos_state.leader.current_round;
        tx_multipaxos->phase = multipaxos_state.leader.phase;
        /* We got the majority last time, we can write the new round
         * number, the new values and reset the proposal number to 0 */
        int i;
        if (multipaxos_state.leader.got_majority) {
          /* populate from values given by the application */
          for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
            tx_multipaxos->values[i] = multipaxos_state.leader.proposed_values[i];
            tx_multipaxos->proposals[i].n = 0;
          }
          multipaxos_state.leader.got_majority = 0;
        } else {
          /* If we didn't have a majority, then we must continue with
           * the actual content
           */
        }
      }
      tx = 1; /* force transmit */
    }
    /* ----- END LEADER LOGIC - if leader is also initiator ----- */


  } else if (current_state == CHAOS_RX && chaos_txrx_success) {
    invalid_rx_count = 0;
    if (tx) {
      /* if we have not received a delta, then we limit tx rate */
      next_state = CHAOS_TX;
      if (complete) {
        if (rx_delta) {
          tx_count_complete = 0;
        } else {
          tx_count_complete++;
        }
      }
    }
  } else if (current_state == CHAOS_RX && !chaos_txrx_success && got_valid_rx) {
    invalid_rx_count++;
    if (invalid_rx_count > restart_threshold) {
      next_state = CHAOS_TX;
      invalid_rx_count = 0;
      if (complete) {
        tx_count_complete++;
      }
      restart_threshold = chaos_random_generator_fast() % (CHAOS_RESTART_MAX - CHAOS_RESTART_MIN) + CHAOS_RESTART_MIN;
    }
  } else if (current_state == CHAOS_TX && !chaos_txrx_success) { /* we missed tx go time. Retry */
    got_valid_rx = 1;
    next_state = CHAOS_TX;
  } else if (current_state == CHAOS_TX && tx_count_complete > N_TX_COMPLETE) {
    next_state = CHAOS_OFF;
    LEDS_OFF(LEDS_GREEN);
  }

  /* save tx buffer in the local state */
  memcpy(&multipaxos_local, tx_multipaxos, sizeof(multipaxos_t));

/* Inject random failures - for evaluation */
#if FAILURES_RATE
#warning "INJECT_FAILURES!!"
  if (chaos_random_generator_fast() < 1 * (CHAOS_RANDOM_MAX / (FAILURES_RATE))) {
    next_state = CHAOS_OFF;
  }
#endif

/* Advanced logging */
#if MULTIPAXOS_ADVANCED_STATISTICS
  /* report number of flags set at this slot */
  uint8_t i;
  for (i = 0; i < FLAGS_LEN; i++) {
    multipaxos_statistics_flags_evolution_per_slot[slot_count] += bit_count(tx_multipaxos->flags[i]);
  }
#endif

  /* reporting progress */
  *app_flags = payload->flags;

  /* stop Synchrotron? */
  uint8_t end = (slot_count >= MULTIPAXOS_ROUND_MAX_SLOTS - 2) || (next_state == CHAOS_OFF);
  if (end) {
    off_slot = slot_count;
  }

  /* return next Synchrotron state */
  return next_state;
}

/* Report the total number of flags */
int multipaxos_get_flags_length() { return FLAGS_LEN; }

/* Is Multi-Paxos running? */
int multipaxos_is_pending(const uint16_t round_count) { return 1; }

/* Report the slot at which Synchrotron received all flags set for the first time */
uint16_t multipaxos_get_completion_slot() { return completion_slot; }

/* Report the slot at which Synchrotron went to off state */
uint16_t multipaxos_get_off_slot() { return off_slot; }

/* Get local structure for reporting */
const multipaxos_t *const multipaxos_get_local() { return &multipaxos_local.multipaxos; }

/* get wireless Paxos internal state */
const multipaxos_state_t *const multipaxos_get_state() { return &multipaxos_state; }

/* If this node is leader, did he get a majority of accept responses? */
uint8_t multipaxos_leader_got_majority() { 
  if (multipaxos_state.leader.is_leader) {
    if (multipaxos_state.leader.got_majority && multipaxos_state.leader.phase == MULTIPAXOS_ACCEPT) {
      return 1;
    }
  }
  return 0;
}

/* If this node is leader, did he get 100% of accept responses? */
uint8_t multipaxos_leader_got_network_wide_consensus() { 
  if (multipaxos_state.leader.is_leader) {
    if (completion_slot > 0) {
      return 1;
    }
  }
  return 0;
}

/* Reset the leader to the previous round, to force adoption of the agreed values again */
void multipaxos_replay_last_consensus() {
  if (multipaxos_leader_got_majority()) {
    multipaxos_state.leader.phase = MULTIPAXOS_PREPARE;
    multipaxos_state.leader.current_round -= MULTIPAXOS_PKT_SIZE;
    multipaxos_state.leader.got_majority = 0;
  }
}

/* initialize variablesz at beginning of a new round */
void multipaxos_initialize_variables_for_new_round() {
  off_slot = MULTIPAXOS_ROUND_MAX_SLOTS;
  tx = 0;
  got_valid_rx = 0;
  complete = 0;
  completion_slot = 0;
  tx_count_complete = 0;
  invalid_rx_count = 0;
  values_chosen_this_round = 0;
  /* init random restart threshold */
  restart_threshold = chaos_random_generator_fast() % (CHAOS_RESTART_MAX - CHAOS_RESTART_MIN) + CHAOS_RESTART_MIN;
  /* set my flag */
  unsigned int array_index = chaos_node_index / 8;
  unsigned int array_offset = chaos_node_index % 8;
  multipaxos_local.multipaxos.flags[array_index] |= 1 << (array_offset);
  /* always increment leader failure counter before round, set back to zero if
   * we receive any paxos packet
   */
  not_heard_from_leader_since++;
}

/* Set the leader memory the first time it becomes leader */
void multipaxos_set_initial_leader_state() {
  multipaxos_state.leader.is_leader = 1;
  multipaxos_state.leader.proposed_ballot.id = chaos_node_index;
  multipaxos_state.leader.proposed_ballot.round = MAX(multipaxos_state.leader.proposed_ballot.round, 1);
  /* We must ask for any rounds happened since the last CHOSEN value (we
   * therefore know a majority of acceptors learned that value), or our oldest
   * round in memory */
  multipaxos_state.leader.current_round = multipaxos_state.learner.last_round + 1;
  if (multipaxos_state.acceptor.last_round_participation > 0)
    multipaxos_state.leader.current_round =
        MAX(multipaxos_state.leader.current_round, (multipaxos_state.acceptor.last_round_participation - MULTIPAXOS_PKT_SIZE + 1));
  not_heard_from_leader_since = 0;
}

/* set new values to propose if we got a majority last round */
void multipaxos_set_leader_values(multipaxos_value_t multipaxos_values[]) {
  if (multipaxos_leader_got_majority()) {
    int8_t i;
    for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) multipaxos_state.leader.proposed_values[i] = multipaxos_values[i];
  } else {
    /* we did not get a majority, we should keep the old values */
  }
}

void multipaxos_report_values_chosen_this_round(multipaxos_value_t learned_values[]) {
  if (values_chosen_this_round) {
    uint8_t i;
    for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
      learned_values[i] = multipaxos_state.learner.learned_values[(multipaxos_state.learner.last_round - MULTIPAXOS_PKT_SIZE + 1 + i) % MULTIPAXOS_LOG_SIZE];
    }
  } else {
    uint8_t i;
    for (i = 0; i < MULTIPAXOS_PKT_SIZE; ++i) {
      learned_values[i] = 0;
    }
  }
}








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
uint8_t multipaxos_round_begin(const uint16_t round_number, const uint8_t app_id, uint8_t is_leader, multipaxos_value_t multipaxos_values[],
                           multipaxos_value_t learned_values[], uint8_t **final_flags) {

  /* intilialize variables */
  multipaxos_initialize_variables_for_new_round();

  /* if we are designed as a leader by the application */
  if (is_leader) {
    /* we were already leader last round */
    if (multipaxos_state.leader.is_leader) {
      multipaxos_set_leader_values(multipaxos_values);
    } else if (multipaxos_state.leader.is_leader == 0) { /* we are newly self-promoted leader by the app */
      /* we must set the memory space */
      multipaxos_set_initial_leader_state();
    }
  } /* endif leader */

  chaos_round(round_number, app_id, (const uint8_t const *)&multipaxos_local.multipaxos,
              sizeof(multipaxos_t) + multipaxos_get_flags_length(), MULTIPAXOS_SLOT_LEN_DCO, MULTIPAXOS_ROUND_MAX_SLOTS,
              multipaxos_get_flags_length(), process);

  /* Report values chosen during this round */
  multipaxos_report_values_chosen_this_round(learned_values);

#if MULTIPAXOS_ADVANCED_STATISTICS
  /* report accepted values in memory */
  uint8_t i;
  for (i = 0; i < MULTIPAXOS_LOG_SIZE; ++i) {
      multipaxos_statistics_values_in_log[i] = multipaxos_state.acceptor.accepted_values[i];
    }
#endif

  /* return wether values were chosen this round or not */
  return values_chosen_this_round;
}