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
 *         A2-Synchrotron MultiPaxos application.
 * \author
*         Valentin Poirot <poirotv@chalmers.se>
 *         Beshr Al Nahas <beshr@chalmers.se>
 *         Olaf Landsiedel <olafl@chalmers.se>
 *
 */

#include "contiki.h"
#include <stdio.h> /* For printf() */
#include "net/netstack.h"

#include "chaos-control.h"
#include "node.h"
#include "multipaxos.h"

static multipaxos_value_t multipaxos_values_to_propose[MULTIPAXOS_PKT_SIZE];
static multipaxos_value_t multipaxos_chosen_values[MULTIPAXOS_PKT_SIZE];
static uint8_t is_proposer = 0;
static uint8_t success = 0;
static uint16_t round_count_local = 0;
static uint8_t* flags;
static uint16_t complete = 0;
static uint16_t off_slot;

/* defined at the end of this file */
/* starts a multi-paxos round */
static void round_begin(const uint16_t round_count, const uint8_t id);
/* defines the rule to propose itself as new leader if current leader is believed to be crashed */
uint8_t multipaxos_app_should_node_become_leader(multipaxos_state_t* multipaxos_state);
/* set the values to be proposed during the multi-paxos round */
void multipaxos_app_set_new_values_to_propose();

/* Define this application as a Synchrotron application */
CHAOS_APP(chaos_multipaxos_app, MULTIPAXOS_SLOT_LEN, MULTIPAXOS_ROUND_MAX_SLOTS, 1, multipaxos_is_pending, round_begin);

/* Should Synchrotron use dynamic join? */
#if NETSTACK_CONF_WITH_CHAOS_NODE_DYNAMIC
#include "join.h"
CHAOS_APPS(&join, &chaos_multipaxos_app);
#else /* don't use group membership */
CHAOS_APPS(&chaos_multipaxos_app);
#endif /* NETSTACK_CONF_WITH_CHAOS_NODE_DYNAMIC */

/* Create this application as a Contiki process */
PROCESS(chaos_multipaxos_app_process, "Wireless Multi-Paxos App Process");
/* Autostart application */
AUTOSTART_PROCESSES(&chaos_multipaxos_app_process);

/* Wireless Multi-Paxos Application */
PROCESS_THREAD(chaos_multipaxos_app_process, ev, data) {
  PROCESS_BEGIN();
  printf("{boot} Wireless Multi-Paxos Application\n");
  NETSTACK_MAC.on();

  while (1) {
    PROCESS_YIELD();

    /* node has Synchrotron group membership */
    if (chaos_has_node_index) {
      /* final values agreed upon, if any */
      if (success) {
        printf("{rd %u chosen values} ", round_count_local);
        uint8_t i;
        for (i = 0; i < MULTIPAXOS_PKT_SIZE; i++) printf("%u,", multipaxos_chosen_values[i]);
        printf("\n");

      } else {
        printf("{rd %u chosen values} No values were chosen this round.\n", round_count_local);
      }
      /* Print full completion latency (see paper for definition) */
      printf("{rd %u full completion latency} %u ms\n", round_count_local, multipaxos_get_completion_slot()*6); /* 1 slot = 6ms */

/* Print advanced statistics */
#if MULTIPAXOS_ADVANCED_STATISTICS
      /* print internal state of the node */
      multipaxos_app_print_advanced_statistics();
      /* reset statistics variables */
      memset(&multipaxos_statistics_flags_evolution_per_slot, 0, sizeof(multipaxos_statistics_flags_evolution_per_slot));
#endif

    } else { /* end if chaos_has_node_index */
      printf("{rd %u res} multipaxos: waiting to join, n: %u\n", round_count_local, chaos_node_count);
    }
  }
  PROCESS_END();
}


/* Perform initial computation before each Wireless Multi-Paxos round */
static void round_begin(const uint16_t round_count, const uint8_t id) {
  /* if node is Synchrotron initiator, let us define it as the leader as well */
  if (IS_INITIATOR()) {
    is_proposer = 1;
    /* Leader got majority last time, set new values to agree on */
    if (multipaxos_leader_got_majority()) {
      /* Call the application for new values to share */
      multipaxos_app_set_new_values_to_propose();
    }
  }
  /* Run Wireless Multi-Paxos */
  success = multipaxos_round_begin(round_count, id, is_proposer, multipaxos_values_to_propose, multipaxos_chosen_values, &flags);
  /* Retrieve full completion (Synchrotron) latency */
  off_slot = multipaxos_get_off_slot();
  complete = multipaxos_get_completion_slot();
  /* Update belief of round number */
  round_count_local = round_count;

  process_poll(&chaos_multipaxos_app_process);
}



/* If this node hasn't heard of the current leader for BECOME_LEADER_AFTER rounds,
 * this function decides if the node proposes itself as the new
 * leader
 */
uint8_t multipaxos_app_should_node_become_leader(multipaxos_state_t* multipaxos_state) {
  return (chaos_random_generator_fast() % (chaos_node_count / 4) == 0); /* throw a dice */
}



/* Define here the logic to set values */
void multipaxos_app_set_new_values_to_propose() {
  /* Dummy application:
   * Send counters, each with a different step
   */
  uint8_t i = 0;
  for (i = 0; i < MULTIPAXOS_PKT_SIZE; i++) {
    multipaxos_values_to_propose[i] = multipaxos_chosen_values[i] + (i + 1);
  }
  /* end dummy application */
}



void multipaxos_app_print_advanced_statistics() {
#if MULTIPAXOS_ADVANCED_STATISTICS
  int i;
  multipaxos_state_t* multipaxos_state_report = multipaxos_get_state();
      printf("{rd %u state} Multi-Paxos: Acceptor (min proposal: (%u,%u), last round %u, ",
          round_count_local,
          multipaxos_state_report->acceptor.min_proposal.round,
          multipaxos_state_report->acceptor.min_proposal.id,
          multipaxos_state_report->acceptor.last_round_participation);
      printf("accepted values ");
      for (i=0; i < MULTIPAXOS_LOG_SIZE; ++i) {
        printf("(%u.%u: %u), ",
            multipaxos_state_report->acceptor.accepted_proposals[i].round,
            multipaxos_state_report->acceptor.accepted_proposals[i].id,
            multipaxos_state_report->acceptor.accepted_values[i]);
      }
      printf("), ");
      if (is_proposer) {
        printf(
            "Proposer (proposal (%u,%u), current round %u, phase %u, ",
            multipaxos_state_report->leader.proposed_ballot.round,
            multipaxos_state_report->leader.proposed_ballot.id,
            multipaxos_state_report->leader.current_round,
            multipaxos_state_report->leader.phase);
        printf("proposed values ");
        for (i=0; i < MULTIPAXOS_PKT_SIZE; ++i) {
          printf("%u, ",
              multipaxos_state_report->leader.proposed_values[i]);
        }
        printf("), ");
      }
      printf("\n");
  /* print the number of flags seen by this node for each Synchrotron slot */
  printf("{rd %u fl} ", round_count_local);
  for (i = 0; i < off_slot; i++) printf("%u,", multipaxos_statistics_flags_evolution_per_slot[i]);
  printf("\n");
#endif
}