# Network Communication Simulation with FreeRTOS

This project simulates a basic network communication system using **FreeRTOS**. It demonstrates how data is sent and received over a simulated "lossy" network link, where packets can be dropped. The simulation includes two common protocols: **Send and Wait (Stop-and-Wait)** and **Go-Back-N (GBN)**.

## What is this project?

This project models a small network with:
* [cite_start]**Two Sender Nodes (Node 1 & Node 2):** These nodes generate and send data packets.
* [cite_start]**A Switch Node:** This acts like a router, forwarding packets and acknowledgements (ACKs) between senders and receivers. [cite_start]It also introduces delays and simulates packet/ACK loss.
* [cite_start]**Two Receiver Nodes (Node 3 & Node 4):** These nodes receive the data and send back ACKs to confirm successful reception.

[cite_start]The core idea is to show how these network protocols work in a real-time operating system (RTOS) environment like FreeRTOS.

## Key Features

* [cite_start]**Packet Handling:** Data is sent in "packets" which have a header (with sequence number, sender, and destination) and a random length payload.
* [cite_start]**Acknowledgement (ACKs):** Receivers send small ACK packets back to the sender to confirm they got the data.
* [cite_start]**Retransmissions:** If a sender doesn't get an ACK back within a certain time (a "timeout" period, `Tout`), it will retransmit the packet. [cite_start]It tries up to 4 times.
* [cite_start]**Go-Back-N (GBN):** This is a more advanced sending method where the sender can send multiple packets (`N`) before waiting for an ACK, making communication more efficient than Send and Wait.
* **Performance Measurement:** The simulation tracks how many packets are successfully received (throughput) and how many times packets had to be retransmitted. It also counts packets that were dropped because they couldn't be retransmitted enough times.

## Why FreeRTOS?

[cite_start]We used FreeRTOS to manage the different parts of the simulation (like sending, receiving, and switching) as separate "tasks". It also allowed us to use RTOS features like:
* [cite_start]**Tasks:** Each part of the network (sender, receiver, switch) runs as its own independent task.
* [cite_start]**Timers:** Used for packet timeouts and simulating network delays.
* [cite_start]**Queues:** Used to send packets and ACKs between the different tasks.
* [cite_start]**Semaphores:** Used to protect shared resources and control access.

## How it was developed

[cite_start]This project was developed for the ELC 2080 course in Spring 2025.

## Files in this repository

* `main.c`: The main C code for the entire simulation logic.
* `Final_Prjct - 2025v2.pdf`: The official project description and specifications.
* `9230816_9230829.pdf`: Our submitted project report, which includes detailed system design, results, and analysis.

## Authors

* Mohamed Nasr Hassan Ali (ID: 9230816)
* Mahmoud Ismail Mahmoud Abdallah (ID: 9230829)
