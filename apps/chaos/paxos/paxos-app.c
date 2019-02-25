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
/**
 * \file
 *         Wireless Paxos application.
 * \author
 *         Valentin Poirot <poirotv@chalmers.se>
 *         Beshr Al Nahas <beshr@chalmers.se>
 *         Olaf Landsiedel <olafl@chalmers.se>
 *
 */

#include <stdio.h> /* For printf() */
#include "contiki.h"
#include "net/netstack.h"

#include "chaos-control.h"
#include "node.h"
#include "paxos.h"

/* Value to be proposed by the node if it is a proposer */
static paxos_value_t paxos_value = 1;
/* Did we learn a value this round? */
static uint8_t success = 0;
/* Value chosen by a majority of acceptors, as seen locally by this node */
static paxos_value_t paxos_learned_value = 0;
/* Is this node a (Paxos) proposer? */
static uint8_t is_proposer = 0;
/* Synchrotron round number */
static uint16_t round_count_local = 0;
/* Synchotron participation flags */
static uint8_t* flags;
/* Slot at which the Wireless Paxos round completed
 * If 0, not all nodes participated in the consensus.
 * If > 0, all nodes participated in the consensus.
 */
static uint16_t complete = 0;
/* Slot at which Synchrotron stopped the radio */
static uint16_t off_slot;

/* defined at the end of this file */
static void round_begin(const uint16_t round_count, const uint8_t id);
static void paxos_app_print_advanced_statistics();

/* Define this application as a Synchrotron application */
CHAOS_APP(chaos_paxos_app, PAXOS_SLOT_LEN, PAXOS_ROUND_MAX_SLOTS, 1, paxos_is_pending, round_begin);

/* Should Synchrotron use dynamic join? */
#if NETSTACK_CONF_WITH_CHAOS_NODE_DYNAMIC
#include "join.h"
CHAOS_APPS(&join, &chaos_paxos_app);
#else
CHAOS_APPS(&chaos_paxos_app);
#endif /* NETSTACK_CONF_WITH_CHAOS_NODE_DYNAMIC */

/* Create this application as a Contiki process */
PROCESS(chaos_paxos_app_process, "Wireless Paxos App Process");
/* Autostart application */
AUTOSTART_PROCESSES(&chaos_paxos_app_process);

/* Wireless Paxos Application */
PROCESS_THREAD(chaos_paxos_app_process, ev, data) {
  PROCESS_BEGIN();
  printf("{boot} Wireless Paxos Application\n");
  NETSTACK_MAC.on();

  while (1) {
    PROCESS_YIELD();


    /* node has Synchrotron group membership */
    if (chaos_has_node_index) {
      /* final value agreed upon, if any */
      if (success) {
        printf("{rd %u state} Paxos: chosen value is %u\n", round_count_local, paxos_learned_value);
      } else {
        printf("{rd %u state} Paxos: no value chosen\n", round_count_local);
      }
      /* Print full completion latency (see paper for definition) */
      printf("{rd %u full completion latency} %u ms\n", round_count_local, paxos_get_completion_slot()*5); /* 1 slot = 5ms */

#if PAXOS_ADVANCED_STATISTICS
      /* Print advanced statistics */
      paxos_app_print_advanced_statistics();
      /* reset statistics */
      memset(&paxos_statistics_flags_evolution_per_slot,
             0,
             sizeof(paxos_statistics_flags_evolution_per_slot));
      memset(&paxos_statistics_value_evolution_per_slot,
             0,
             sizeof(paxos_statistics_value_evolution_per_slot));
      memset(&paxos_statistics_min_proposal_evolution_per_slot,
             0,
             sizeof(paxos_statistics_min_proposal_evolution_per_slot));
      memset(&paxos_statistics_accepted_proposal_evolution_per_slot,
             0,
             sizeof(paxos_statistics_accepted_proposal_evolution_per_slot));
#endif /* PAXOS_ADVANCED_STATISTICS */

    } else { /* end if chaos_has_node_index */
      printf(
          "{rd %u res} Paxos: node doesn't have Synchrotron group "
          "membership, n: %u\n",
          round_count_local, chaos_node_count);
    }
  }
  PROCESS_END();
}

/* Perform initial computation before each Wireless Paxos round */
static void round_begin(const uint16_t round_count, const uint8_t id) {
  /* Define proposers.
   * Here, the Synchrotron initiator is the proposer
   */
  if (IS_INITIATOR()) {
    is_proposer = 1;
    /* define value to agree on */
    paxos_value = round_count_local+1; /* simple counter here */
  }

  /* Reset Paxos internal state to start a new consensus, only if all nodes received the value */
  if (complete > 0) {
    paxos_reset_state();
  }
  complete = 0;

  /* execute Wireless Paxos */
  success = paxos_round_begin(round_count, id, is_proposer, &paxos_value, &flags);
  /* read chosen value */
  if (success) paxos_learned_value = paxos_get_learned_value();
  /* Get time statsitics */
  off_slot = paxos_get_off_slot();
  complete = paxos_get_completion_slot();
  /* Update local round */
  round_count_local = round_count;

  process_poll(&chaos_paxos_app_process);
}


/* Print advanced statistics - internal state and flags evolution through time */
static void paxos_app_print_advanced_statistics() {
#if PAXOS_ADVANCED_STATISTICS
  paxos_state_t* paxos_state_report = paxos_get_state();
  /* Print acceptor's internal state */
  printf(
      "{rd %u state} Paxos: Acceptor (min proposal: (%u.%u), "
      "accepted proposal: (%u.%u), accepted value: %u) ",
      round_count_local, paxos_state_report->acceptor.min_proposal.round, paxos_state_report->acceptor.min_proposal.id,
      paxos_state_report->acceptor.accepted_proposal.round, paxos_state_report->acceptor.accepted_proposal.id,
      paxos_state_report->acceptor.accepted_value);
  /* Print node's internal aggregation state */
  /*
  printf("rx_aggregate (RX min proposal: %x, RX accepted proposal: %x,
  RX accepted value: %u)", paxos_state_report->rx_min_proposal.n,
                  paxos_state_report->rx_accepted_proposal.n,
                  paxos_state_report->rx_accepted_value);
  */
 /* Print proposer's internal state */
  if (is_proposer) {
    printf(
        "Proposer (ballot (%u.%u), proposed value %u, phase %u, "
        "got majority at slot %u)",
        paxos_state_report->proposer.proposed_ballot.round, paxos_state_report->proposer.proposed_ballot.id,
        paxos_state_report->proposer.proposed_value, paxos_state_report->proposer.phase, paxos_state_report->proposer.got_majority_at_slot);
  }
  printf("\n");
  int i;
  /* print total number of flags received per slot */
  printf("{rd %u fl} ", round_count_local);
  for (i = 0; i < off_slot; i++) {
    printf("%u,", paxos_statistics_flags_evolution_per_slot[i]);
  }
  printf("\n");
  /* print accepted value evolution per slot */
  printf("{rd %u val} ", round_count_local);
  for (i = 0; i < off_slot; i++) {
    printf("%u,", paxos_statistics_value_evolution_per_slot[i]);
  }
  printf("\n");
  /* print min proposal evolution per slot
  0 means min proposal has not changed since last slot*/
  printf("{rd %u minP} ", round_count_local);
  for (i = 0; i < off_slot; i++) {
    printf("%u,", paxos_statistics_min_proposal_evolution_per_slot[i]);
  }
  printf("\n");
  /* print accepted proposal evolution per slot
  0 means accepted proposal has not changed since last slot*/
  printf("{rd %u acP} ", round_count_local);
  for (i = 0; i < off_slot; i++) {
    printf("%u,", paxos_statistics_accepted_proposal_evolution_per_slot[i]);
  }
  printf("\n");
#endif /* PAXOS_ADVANCED_STATISTICS */
}