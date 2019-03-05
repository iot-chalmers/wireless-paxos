# Paxos Made Wireless: Consensus in the Air

## Introduction

This repository hosts the source code of the Wireless Paxos primitive published in the International Conference on Embedded Wireless Systems and Networks (EWSN) 2019.

Valentin poirot, Beshr Al Nahas and Olaf Landsiedel. 2019. 
"Paxos Made Wireless: Consensus in the Air". 
_In Proceedings of the International Conference on Embedded Wireless Systems and Networks (EWSN)_.
[paper](https://research.chalmers.se/en/publication/508924), [talk](./resources/ewsn_wireless_paxos.pdf).

## Abstract

Many applications in low-power wireless networks require complex coordination between their members. Swarms of robots or sensors and actuators in industrial closed-loop control need to coordinate within short periods of time to execute tasks. Failing to agree on a common decision can cause substantial consequences, like system failures and threats to human life. Such applications require consensus algorithms to enable coordination. While consensus has been studied for wired networks decades ago, with, for example, Paxos and Raft, it remains an open problem in multi-hop low-power wireless networks due to the limited resources available and the high cost of established solutions.

This paper presents Wireless Paxos, a fault-tolerant, network-wide consensus primitive for low-power wireless networks. It is a new flavor of Paxos, the most-used consensus protocol today, and is specifically designed to tackle the challenges of low-power wireless networks. By building on top of concurrent transmissions, it provides low-latency, high reliability, and guarantees on the consensus. Our results show that Wireless Paxos requires only 289 ms to complete a consensus between 188 nodes in testbed experiments. Furthermore, we show that Wireless Paxos stays consistent even when injecting controlled failures.

## Implementation

We implement our low-power consensus primitives in C for the Contiki OS targeting simple wireless nodes equipped with a low-power radio such as TelosB and Wsn430 platforms which feature a 16bit MSP430 CPU @ 4 MHz, 10 kB of RAM, 48 kB of firmware storage and CC2420 radio compatible with IEEE 802.15.4.

We design our primitives for Synchrotron, the transmission kernel of [Agreement in the Air (A^2)](https://github.com/iot-chalmers/a2-synchrotron), to provide low-latency and high reliability.

## Code structure

### Consensus primitives

The implementation of Wireless Paxos can be found in [core/net/mac/chaos/lib/paxos](./core/net/mac/chaos/lib/paxos/), while the Multi-Paxos primitive can be found in [core/net/mac/chaos/lib/multipaxos](./core/net/mac/chaos/lib/multipaxos/).


### Applications using consensus
An example of application using Wireless Paxos can be found at [apps/chaos/paxos](./apps/chaos/paxos).
An example of application using Wireless Multi-Paxos can be found at [apps/chaos/multipaxos](./apps/chaos/multipaxos).

## Running Wireless Paxos

### In Cooja

Go to [apps/chaos/paxos](./apps/chaos/paxos) and compile using:
```
make clean TARGET=sky && make cooja TARGET=sky log=0 printf=1 tx=31 mch=1 pch=4 sec=5 src=2 sync=0 failure=0 dynamic=1 initiator=3 interval=39 max_node_count=50 duration=15 description=“paxos” -j4
```
Then, start a new simulation in Cooja. Add new motes in Motes > Add motes > Create new mote type > Sky mote using paxos-app.sky as firmware.
Run the simulation. After a group initialization, the nodes will run the Paxos application.

You can run Wireless Multi-Paxos in [apps/chaos/multipaxos](./apps/chaos/multipaxos)

### In Flocklab

Use the same firmware as for cooja.
