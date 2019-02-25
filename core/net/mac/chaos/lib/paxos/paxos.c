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

#include <string.h>
#include "contiki.h"

#include "chaos-config.h"
#include "chaos-random-generator.h"
#include "chaos.h"
#include "node.h"
#include "paxos.h"

#undef ENABLE_COOJA_DEBUG
#define ENABLE_COOJA_DEBUG COOJA
#include "dev/cooja-debug.h"

/* Log statistics:
 * - (Synchrotron) flags evolution per slot,
 * - Accepted value evolution per slot,
 * - Accepted proposal evolution per slot,
 * - Min proposal evolution per slot
 */
#ifndef PAXOS_ADVANCED_STATISTICS
#define PAXOS_ADVANCED_STATISTICS 0
#endif

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

#if PAXOS_ADVANCED_STATISTICS
/* Number of flags set as locally seen by the node, for each Synchrotron slot */
uint8_t paxos_statistics_flags_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS] = {0};
/* Locally saved accepted value, for each Synchrotron slot */
paxos_value_t paxos_statistics_value_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS] = {0};
/* Locally saved min proposal, for each Synchrotron slot */
/* 0 means the min proposal hasn't changed since the previous slot */
uint16_t paxos_statistics_min_proposal_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS] = {0};
/* Temporary variable, used to save the last update of min proposal (reduce log
 * printing)
 */
static uint16_t paxos_statistics_min_proposal_last_update = 0;
/* Locally saved accepted proposal, for each Synchrotron slot */
/* 0 means the min proposal hasn't changed since the previous slot */
uint16_t paxos_statistics_accepted_proposal_evolution_per_slot[PAXOS_ROUND_MAX_SLOTS] = {0};
/* Temporary variable, used to save the last update of accepted proposal (reduce
 * log printing)
 */
static uint16_t paxos_statistics_accepted_proposal_last_update = 0;
#endif

/* Local memory struct of Wireless Paxos
 * paxos:    clean paxos_t instance, used to report results
 * flags:    all flags are unset, except the local flag
 */
typedef struct __attribute__((packed)) paxos_t_local_struct {
  paxos_t paxos;
  uint8_t flags[FLAGS_ESTIMATE];
} paxos_t_local;

/* Should we transmit during the next slot */
static int tx = 0;
/* are all flags set */
static int complete = 0;
/* Slot at which all flags were set for the first time,
 * Slot at which we stopped participating in the round
 */
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
static uint8_t value_chosen_this_round = 0;
/* Timeout since last reception before TX again */
static unsigned short restart_threshold;
/* Used to report final values, and reset flags between rounds */
static paxos_t_local paxos_local;
/* Current state of the Wireless Paxos algorithm */
static paxos_state_t paxos_state;
/* Current flags */
static uint8_t* paxos_flags;

/*
 * Wireless Paxos Assumptions:
 * 1. Every participant acts at least as an acceptor.
 *  1.a. Proposers act both as acceptors and proposers.
 *  1.b. The acceptor logic is executed prior to the proposer logic.
 * 2. Any node can receive the content of a broadcast primitive.
 *  2.a. A proposer can directly hear from another proposer.
 * 3. We locally keep the RX packet data with the highest ballot (=proposal) N
 * received so far. 3.a. Upon reception of a ballot n < N, we discard it and
 * retransmit N
 * 4. A Proposer learning that there is a higher ballot around competes after a
 * backoff.
 */

/* Main function, called at each slot */
static chaos_state_t process(uint16_t round_count, uint16_t slot_count, chaos_state_t current_state, int chaos_txrx_success,
                             size_t payload_length, uint8_t* rx_payload, uint8_t* tx_payload, uint8_t** app_flags) {
  /* payload for the TX packet, possibly set with the last TX data */
  paxos_t* tx_paxos = (paxos_t*)tx_payload;
  /* payload for the last RX packet */
  paxos_t* rx_paxos = (paxos_t*)rx_payload;
  /* payload is set to the RX payload if we were listening, or the TX if we
   * transmitted last slot */
  paxos_t* payload = rx_paxos;
  if (current_state == CHAOS_TX) {
    payload = tx_paxos;
  }
  /* Is the RX packet containing novel information */
  uint8_t rx_delta = 0;
  /* Should we transmit next time */
  tx = 0;

  if (chaos_txrx_success                                                         /* Last slot was successful */
      && (current_state == CHAOS_RX                                              /* and we were listening during this slot */
          || (current_state == CHAOS_TX && paxos_state.proposer.is_proposer))) { /* or we are a proposer and
                                                                                    were TX this slot */

    /* Reception was correct for this slot */
    got_valid_rx = 1;
    /* Set the flags counter to zero */
    n_replies = 0;

    /* a PAXOS_INIT packet is a heartbeat from Synchrotron initiator to
     * allow any proposer to start a Paxos round
     */
    /* if min_proposal is 0, we haven't received any Paxos request yet */
    if (payload->phase == PAXOS_INIT && paxos_state.acceptor.min_proposal.n == 0) {
      if (paxos_state.proposer.is_proposer) {
        /* BEGIN PROPOSER - INITIATE PAXOS ALGORITHM (1/3) */

        /* this proposer has not started a Paxos round yet */
        if (paxos_state.proposer.phase == PAXOS_INIT) {
          memcpy(tx_paxos->flags, paxos_local.paxos.flags, FLAGS_LEN);
          paxos_state.proposer.phase = PAXOS_PREPARE;
          tx_paxos->ballot.n = paxos_state.proposer.proposed_ballot.n;
          tx_paxos->phase = PAXOS_PREPARE;
          /* Optimization: We directly set the acceptor phase to
           * accept the new ballot */
          paxos_state.acceptor.min_proposal.n = paxos_state.proposer.proposed_ballot.n;

          /* END PROPOSER - INITIATE PAXOS ALGORITHM (1/3) */
        }
        /* We transmit next slot */
        tx = rx_delta = 1;
      } else { /* not a proposer */
        /* we retransmit received packet */
        memcpy(tx_paxos, payload, sizeof(paxos_t)); /* TODO remove? */
        uint16_t flag_sum = 0;
        int i;
        for (i = 0; i < FLAGS_LEN; i++) {
          tx |= (rx_paxos->flags[i] != tx_paxos->flags[i]);
          tx_paxos->flags[i] |= rx_paxos->flags[i];
          flag_sum += tx_paxos->flags[i];
        }
        if (flag_sum >= FLAG_SUM) {
          complete = 1;
        }
      }
      rx_delta |= tx;

    } else { /* Not a PAXOS_INIT packet or we have received a correct Paxos
                request earlier */
      tx = 0;

      /* BEGIN ACCEPTOR LOGIC */

      /* New packet  is possibly newer (strictly higher ballot or current
       * ballot
       */
      if (payload->ballot.n > tx_paxos->ballot.n || (payload->ballot.n == tx_paxos->ballot.n && payload->phase >= tx_paxos->phase)) {
        /* is this a new phase? */
        uint8_t new_phase = 0;
        /* A packet is new if it contains a strictly higher ballot or
         * strictly higher phase if same ballot
         */
        new_phase = !(payload->ballot.n == tx_paxos->ballot.n && payload->phase == tx_paxos->phase);
        if (new_phase) {
          /* Strictly new ballot, we discard previous flags and copy
           * the RX packet into TX memory
           */
          memcpy(tx_paxos, payload, sizeof(paxos_t) + FLAGS_LEN);
          /* We reset local aggregated variables */
          memset(&paxos_state.rx_accepted_proposal, 0, sizeof(paxos_state.rx_accepted_proposal));
          memset(&paxos_state.rx_accepted_value, 0, sizeof(paxos_state.rx_accepted_value));
          memset(&paxos_state.rx_min_proposal, 0, sizeof(paxos_state.rx_min_proposal));
        }

        /* BEGIN ACCEPTOR LOGIC - PREPARE PHASE */

        if (payload->phase == PAXOS_PREPARE) {
          /* Paxos algorithm: Save ballot as min_proposal is higher
           * ballot received so far
           */
          if (payload->ballot.n > paxos_state.acceptor.min_proposal.n) {
            paxos_state.acceptor.min_proposal.n = payload->ballot.n;
          }
          /* Paxos algorithm: report the maximum accepted proposal and
           * corresponding value (if any)
           */
          /* Wireless Paxos optimization: some other acceptor might
           * have a higher accepted proposal, we transmit the highest
           * acceptor proposal heard rather than our own
           */
          if (payload->proposal.n < paxos_state.rx_accepted_proposal.n) {
            tx_paxos->proposal.n = paxos_state.rx_accepted_proposal.n;
            tx_paxos->value = paxos_state.rx_accepted_value;
            /* transmit novel information next slot */
            tx = rx_delta = 1;
          } else { /* Our local aggreagted data is not up to date */
            paxos_state.rx_accepted_proposal.n = payload->proposal.n;
            paxos_state.rx_accepted_value = payload->value;
          } /* end report accepted proposal and value if any */
            /* END ACCEPTOR LOGIC - PREPARE PHASE */

          /* BEGIN ACCEPTOR LOGIC - ACCEPT PHASE */

        } else if (payload->phase == PAXOS_ACCEPT) {
          /* Paxos algorithm: If ballot is higher than min_rposal,
           * then accept new proposal
           */
          if (payload->ballot.n >= paxos_state.acceptor.min_proposal.n) {
            /* accept proposal AND change min_proposal to accepted
             * proposal */
            paxos_state.acceptor.accepted_proposal.n = paxos_state.acceptor.min_proposal.n = payload->ballot.n;
            paxos_state.acceptor.accepted_value = payload->value;
          }

          /* Wireless Paxos optimization: report highest min proposal
           * ever heard
           */
          paxos_state.rx_min_proposal.n = MAX(paxos_state.acceptor.min_proposal.n, paxos_state.rx_min_proposal.n);
          paxos_state.rx_min_proposal.n = MAX(payload->proposal.n, paxos_state.rx_min_proposal.n);
          /* If reported min_rposal is lower than local value, we
           * report highest value
           */
          if (tx_paxos->proposal.n != paxos_state.rx_min_proposal.n) {
            tx_paxos->proposal.n = paxos_state.rx_min_proposal.n;
            /* transmit novel information next slot */
            tx = rx_delta = 1;
          }

          /* update rx_accepted_proposal with maximum accepted or
           * heard so far
           */
          if (paxos_state.acceptor.accepted_proposal.n > paxos_state.rx_accepted_proposal.n) {
            paxos_state.rx_accepted_proposal.n = paxos_state.acceptor.accepted_proposal.n;
            paxos_state.rx_accepted_value = paxos_state.acceptor.accepted_value;
          }

          /* END ACCEPTOR LOGIC - ACCEPT PHASE */
        } else {
/* ERROR! Phase should be PAXOS_INIT, PAXOS_PREPARE or PAXOS_ACCEPT, and nothing else */
#if COOJA
          COOJA_DEBUG_STR("ACCEPTOR rcvd AN UNKNOWN PHASE!!");
#endif
        }

        /* BEGIN TRANSMISSION LOGIC */
        uint16_t flag_sum = 0;
        n_replies = 0;
        int i;
        if (!new_phase) {
          /* We didn't memcopy, we need to merge flags */
          for (i = 0; i < FLAGS_LEN; i++) {
            rx_delta |= (payload->flags[i] != tx_paxos->flags[i]);
            tx_paxos->flags[i] |= payload->flags[i];
            flag_sum += tx_paxos->flags[i];
            n_replies += bit_count(tx_paxos->flags[i]);
          }
        } else if (new_phase) {
          tx = rx_delta = 1;
          for (i = 0; i < FLAGS_LEN; i++) {
            tx_paxos->flags[i] = payload->flags[i]; /* TODO REMOVE? no need since we
                                                       copied flags */
            flag_sum += tx_paxos->flags[i];
            n_replies += bit_count(tx_paxos->flags[i]);
          }
        }
        /* Add our own flag */
        unsigned int array_index = chaos_node_index / 8;
        unsigned int array_offset = chaos_node_index % 8;
        tx_paxos->flags[array_index] |= 1 << (array_offset);

        /* Something new? We should transmit */
        tx |= rx_delta;

        /* Wireless Paxos optimization:
         * An acceptor will reduce its tx rate if a majority of flags are
         * present during a prepare phase in order to to help the proposer
         * starts the second phase faster
         */
        if (!paxos_state.proposer.is_proposer && payload->phase == PAXOS_PREPARE && (n_replies > (chaos_node_count / 2)) && tx == 1) {
          tx = (chaos_random_generator_fast() % (chaos_node_count / 2) == 0) ? 1 : 0; /* We reduce tx rate to improve transition
                                                                                         time */
        }

        /* Wireless Paxos optimization:
         * We can have a Quorum Read for 'free' simply by reading the
         * number of flags
         */
        if (payload->phase == PAXOS_ACCEPT && payload->ballot.n == payload->proposal.n && (n_replies > chaos_node_count / 2)) {
          /* save accepted_value as learned value since a majority
           * accepted this proposal
           */
          paxos_state.learner.learned_value = payload->value;
          value_chosen_this_round = 1;
        }

        /* All flags are set */
        if (payload->phase == PAXOS_ACCEPT && flag_sum >= FLAG_SUM) {
          if (!complete) {
            /* Save the first time completion is met */
            completion_slot = slot_count;
            complete = 1;
          }
          /* transmit next time, but no novel information contained */
          tx = 1;
        }

      } else { /* ballot is current ballot or higher ballot */
        /* We received an old ballot, inform network about newest data */
        tx = 1;
      }
      /* END ACCEPTOR LOGIC */

      /* BEGIN PROPOSER LOGIC */

      /* Apply proposer logic until majority is met in accept phase */
      if (paxos_state.proposer.is_proposer && !paxos_state.proposer.got_majority) {
        /* lost_proposal: is a higher proposal circulating in the
         * network? update_phase: switch from prepare to accept phase
         */
        uint8_t lost_proposal = 0, update_phase = 0;
        /* If we lost a competition, decrease the timeout counter until
         * next competition
         */
        if (paxos_state.proposer.loser_timeout > 0) {
          paxos_state.proposer.loser_timeout--;
          if (paxos_state.proposer.loser_timeout == 0) {
            /* Timeout finished, compete again */
            update_phase = 1;
          }
        } else { /* We didn't loose the competition yet */
          /* Received packet is my own request */
          if (payload->ballot.n == paxos_state.proposer.proposed_ballot.n) {
            /* Received packet is my own phase */
            if (payload->phase == paxos_state.proposer.phase) {
              /* BEGIN PROPOSER LOGIC - PREPARE PHASE */
              if (paxos_state.proposer.phase == PAXOS_PREPARE) {
                /* Aggregated data has been updated by the
                 * acceptor beforehand (see assumption 1.b in
                 * the comments)
                 */
                /* Paxos algorithm: We adopt the highest
                 * accepted value as our new proposed value
                 */
                if (paxos_state.rx_accepted_proposal.n > 0) {
                  paxos_state.proposer.proposed_value = paxos_state.rx_accepted_value;
                }
                /* This shouldn't happen since old proposal are
                 * discarded
                 */
                if (paxos_state.rx_accepted_proposal.n > paxos_state.proposer.proposed_ballot.n) {
                  lost_proposal = 1;
                }
                /* END PROPOSER LOGIC - PREPARE PHASE */

                /* BEGIN PROPOSER LOGIC - ACCEPT PHASE */
              } else if (paxos_state.proposer.phase == PAXOS_ACCEPT) {
                /* Paxos algorithm: round is lost if acceptors
                 * reported a higher min proposal */
                if (paxos_state.rx_min_proposal.n > paxos_state.proposer.proposed_ballot.n) {
                  lost_proposal = 1;
                }
              }
              /* END PROPOSER LOGIC - ACCEPT PHASE */

              /* start counting number of responses */
              n_replies = 0;
              int i;
              for (i = 0; i < FLAGS_LEN; i++) {
                /* no need to merge flags because we did in
                 * acceptor logic
                 */
                n_replies += bit_count(tx_paxos->flags[i]);
              }

              /* if majority => switch to next phase */
              if (!lost_proposal && n_replies > chaos_node_count / 2) {
                /* BEGIN PROPOSER LOGIC - PREPARE PHASE */
                if (paxos_state.proposer.phase == PAXOS_PREPARE) {
                  paxos_state.proposer.phase = PAXOS_ACCEPT;
                  update_phase = 1;
                  /* END PROPOSER LOGIC - PREPARE PHASE */
                  /* BEGIN PROPOSER LOGIC - ACCEPT PHASE */
                } else if (paxos_state.proposer.phase == PAXOS_ACCEPT) {
                  if (!paxos_state.proposer.got_majority) {
                    paxos_state.proposer.got_majority = 1;
                    paxos_state.proposer.got_majority_at_slot = slot_count;
                  }
                }
                /* END PROPOSER LOGIC - ACCEPT PHASE */
              }

              /* Paxos algorithm: we lose if acceptors report a
               * higher proposal Wireless Paxos Optimization: If we
               * got a majority before, the next proposal will have
               * the same value, no need to compete again
               */
              if (paxos_state.proposer.phase == PAXOS_ACCEPT && paxos_state.rx_min_proposal.n > paxos_state.proposer.proposed_ballot.n &&
                  !paxos_state.proposer.got_majority) {
                lost_proposal = 1;
              }

            } else { /* end same phase  as expected */
              /* older phase received, propagate new information */
              tx = 1;

              if (payload->phase > paxos_state.proposer.phase) {
/* We shouldn't received a higher phase than our own with our ballot */
#if COOJA
                COOJA_DEBUG_STR("PROPOSER rcvd AN ADVANCED PHASE!!");
#endif
              }
            }

            /* end received our own ballot */
            /* received packet with higher ballot */
          } else if (payload->ballot.n > paxos_state.proposer.proposed_ballot.n && !paxos_state.proposer.got_majority) {
            lost_proposal = 1;
          } else { /* end our ballot or higher ballot */
                   /* smaller ballot, transmit our ballot */
            tx = 1;

            /* BEGIN PROPOSER - INITIATE PAXOS ALGORITHM (2/3) */
            if (paxos_state.proposer.phase == PAXOS_INIT) {
              paxos_state.proposer.phase = PAXOS_PREPARE;
              tx_paxos->ballot.n = paxos_state.proposer.proposed_ballot.n;
              tx_paxos->phase = PAXOS_PREPARE;
              paxos_state.acceptor.min_proposal.n = paxos_state.proposer.proposed_ballot.n;
            }
            /* END PROPOSER - INITIATE PAXOS ALGORITHM (2/3)*/
          }
        }

        /* competition was lost? */
        if (lost_proposal) {
          /* increase ballot for next time */
          paxos_state.proposer.proposed_ballot.round++;
          /* already adopt accepted value for next time */
          if (paxos_state.rx_accepted_proposal.n > 0) {
            paxos_state.proposer.proposed_value = paxos_state.rx_accepted_value;
          }
          /* reset state to beginning */
          paxos_state.proposer.phase = PAXOS_PREPARE;
          paxos_state.proposer.got_majority = 0;
          /* timeout before updating phase and starting a new proposal
           */
          paxos_state.proposer.loser_timeout = PAXOS_ROUND_MAX_SLOTS - 1; /* WILL NOT COMPETE THIS ROUND */
          //paxos_state.proposer.loser_timeout = chaos_random_generator_fast() % (50 - 0) + 50; /* WILL COMPETE THIS ROUND */
        }

        /* proposer got a majority in prepare phase */
        if (update_phase) {
          tx_paxos->ballot.n = paxos_state.proposer.proposed_ballot.n;
          tx_paxos->phase = paxos_state.proposer.phase; /* changed during majority check */
          tx_paxos->proposal.n = 0;
          tx_paxos->value = paxos_state.proposer.proposed_value;
          /* reset flags and set my flag only */
          memcpy(tx_paxos->flags, paxos_local.paxos.flags, FLAGS_LEN);
          /* transmit new phase */
          rx_delta = tx = 1;
        }
      }

      /* END PROPOSER LOGIC */

    } /* END not PAXOS_INIT phase received */
  }   /* END correct packet received and (chaos == RX or (chaos == TX and
       *  is_proposer))
       */



  /* BEGIN SYNCHROTRON STATE LOGIC */

  chaos_state_t next_state = CHAOS_RX;
  uint8_t initiate_round = 0;
  /* if more than one Synchrotron initiators are present */
#if ENABLE_MULTIPLE_INITIATORS
  /* Warning! Might cause timing issues if enabled */
  initiate_round = chaos_node_index < N_SOURCES;
#else
  initiate_round = IS_INITIATOR();
#endif
  if (initiate_round && current_state == CHAOS_INIT) {
    next_state = CHAOS_TX;
    /* Chaos trick to enable retransmissions */
    got_valid_rx = 1;

    /* BEGIN PROPOSER - INITIATE PAXOS ALGORITHM (3/3)*/

    if (paxos_state.proposer.is_proposer && paxos_state.proposer.phase == PAXOS_INIT) {
      paxos_state.proposer.phase = PAXOS_PREPARE;
      tx_paxos->ballot.n = paxos_state.proposer.proposed_ballot.n;
      tx_paxos->phase = PAXOS_PREPARE;
      paxos_state.acceptor.min_proposal.n = paxos_state.proposer.proposed_ballot.n;
    }
    /* END PROPOSER - INITIATE PAXOS ALGORITHM (3/3)*/

  } else if (tx_count_complete > N_TX_COMPLETE) { /* Round is completed and we transmitted
                                                     multiple time after completion */
    next_state = CHAOS_OFF;
    LEDS_OFF(LEDS_GREEN);
  } else if (current_state == CHAOS_RX && chaos_txrx_success) { /* slot was successful */
    invalid_rx_count = 0;
    if (tx) {
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
    got_valid_rx = 1;                                            /* Trick from Chaos. TODO keep it? */
    next_state = CHAOS_TX;
  }

#if FAILURES_RATE
#warning "INJECT_FAILURES!!"
  if (chaos_random_generator_fast() < 1 * (CHAOS_RANDOM_MAX / (FAILURES_RATE))) {
    next_state = CHAOS_OFF;
  }
#endif

  /* report final results */
  if (complete || slot_count >= PAXOS_ROUND_MAX_SLOTS - 1) {
    paxos_flags = tx_paxos->flags;
    paxos_local.paxos.value = paxos_state.acceptor.accepted_value;
    paxos_local.paxos.proposal.n = paxos_state.acceptor.accepted_proposal.n;
    paxos_local.paxos.ballot.n = paxos_state.acceptor.min_proposal.n;
    paxos_local.paxos.phase = tx_paxos->phase;
    if (!paxos_state.proposer.is_proposer) {
      paxos_state.proposer.phase = tx_paxos->phase;
    }
  }
  *app_flags = payload->flags;

#if PAXOS_ADVANCED_STATISTICS
  /* report accepted value at this slot */
  paxos_statistics_value_evolution_per_slot[slot_count] = paxos_state.acceptor.accepted_value;
  /* report min proposal at this slot */
  /* Assumption: Once a value is saved, it cannot go back to 0, we therefore
   * replace to 0 to avoid too long string
   */
  if (paxos_statistics_min_proposal_last_update != paxos_state.acceptor.min_proposal.n || slot_count == 0) {
    paxos_statistics_min_proposal_evolution_per_slot[slot_count] = paxos_state.acceptor.min_proposal.n;
    paxos_statistics_min_proposal_last_update = paxos_state.acceptor.min_proposal.n;
  } else {
    paxos_statistics_min_proposal_evolution_per_slot[slot_count] = 0;
  }
  /* report accepted proposal at this slot */
  if (paxos_statistics_accepted_proposal_last_update != paxos_state.acceptor.accepted_proposal.n || slot_count == 0) {
    paxos_statistics_accepted_proposal_evolution_per_slot[slot_count] = paxos_state.acceptor.accepted_proposal.n;
    paxos_statistics_accepted_proposal_last_update = paxos_state.acceptor.accepted_proposal.n;
  } else {
    paxos_statistics_accepted_proposal_evolution_per_slot[slot_count] = 0;
  }
  /* report number of flags set at this slot */
  uint8_t i;
  for (i = 0; i < FLAGS_LEN; i++) {
    paxos_statistics_flags_evolution_per_slot[slot_count] += bit_count(tx_paxos->flags[i]);
  }
#endif

  /* Stop Synchrotron if round is finished soon */
  int end = (slot_count >= PAXOS_ROUND_MAX_SLOTS - 2) || (next_state == CHAOS_OFF);
  if (end) {
    off_slot = slot_count;
  }

  /* return next Synchrotron state */
  return next_state;
}

/* Report the total number of flags */
int paxos_get_flags_length() { return FLAGS_LEN; }

/* Is Paxos running? */
int paxos_is_pending(const uint16_t round_count) { return 1; }

/* Report the slot at which Synchrotron received all flags set for the first time */
uint16_t paxos_get_completion_slot() { return completion_slot; }

/* Report the slot at which Synchrotron went to off state */
uint16_t paxos_get_off_slot() { return off_slot; }

/* If this node is a proposer, did he get a majority of accept responses? */
uint8_t paxos_proposer_got_majority() { 
  if (paxos_state.proposer.is_proposer) {
    if (paxos_state.proposer.got_majority && paxos_state.proposer.phase == PAXOS_ACCEPT) {
      return 1;
    }
  }
  return 0;
}

/* If this node is a proposer, did he get 100% of accept responses? */
uint8_t paxos_proposer_got_network_wide_consensus() { 
  if (paxos_state.proposer.is_proposer) {
    if (completion_slot > 0) {
      return 1;
    }
  }
  return 0;
}

/* Get local structure for reporting */
const paxos_t* const paxos_get_local() { return &paxos_local.paxos; }

/* get wireless Paxos internal state */
const paxos_state_t* const paxos_get_state() { return &paxos_state; }

/* reset wireless Paxos internal state */
void paxos_reset_state() {
  /* reset transaction state to start a new one */
  memset(&paxos_state, 0, sizeof(paxos_state));
  memset(&paxos_local, 0, sizeof(paxos_local));
}

/* Report the slot at which Synchrotron went to off state */
paxos_value_t paxos_get_learned_value() { return paxos_state.learner.learned_value; }

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
                      uint8_t** final_flags) {
  /* initialize variables */
  off_slot = PAXOS_ROUND_MAX_SLOTS;
  tx = 0; /* should we transmit at this slot */
  got_valid_rx = 0; /* received at least one correct packet this round */
  n_replies = 0; /* how many nodes have participated in this packet */
  complete = 0; /* did we received all flags this round */
  completion_slot = 0; /* slot until completion */
  tx_count_complete = 0; /* final flood counter */
  invalid_rx_count = 0; /* invalid reception counter */
  value_chosen_this_round = 0; /* Was a value chosen this round */
  /* init random TX timeout backoff */
  restart_threshold = chaos_random_generator_fast() % (CHAOS_RESTART_MAX - CHAOS_RESTART_MIN) + CHAOS_RESTART_MIN;
#if PAXOS_ADVANCED_STATISTICS
  paxos_statistics_min_proposal_last_update = 0; /* used to save space when printing statistics */
  paxos_statistics_accepted_proposal_last_update = 0; /* used to save space when printing statistics */
#endif


  if (is_proposer) {
    /* initialize the proposer */
    paxos_state.proposer.phase = PAXOS_INIT;
    paxos_state.proposer.proposed_ballot.id = chaos_node_index;
    paxos_state.proposer.proposed_ballot.round = 1; /* note that we start with 1 */
    paxos_state.proposer.proposed_value = *paxos_value;
    paxos_state.proposer.is_proposer = 1;
    paxos_local.paxos.value = *paxos_value;
  }
  /* set my flag */
  unsigned int array_index = chaos_node_index / 8;
  unsigned int array_offset = chaos_node_index % 8;
  paxos_local.paxos.flags[array_index] |= 1 << (array_offset);

  /* start the Wireless paxos round */
  chaos_round(round_number, app_id, (const uint8_t const*)&paxos_local.paxos, sizeof(paxos_t) + paxos_get_flags_length(),
              PAXOS_SLOT_LEN_DCO, PAXOS_ROUND_MAX_SLOTS, paxos_get_flags_length(), process);


  memcpy(paxos_local.paxos.flags, paxos_flags, paxos_get_flags_length());
  /* report flags */
  *final_flags = paxos_local.flags;

  /* Report 1 if a value has been chosen and learned by that node
   * returns 0 if that node is not aware of a chosen value
   */
  return value_chosen_this_round;
}
