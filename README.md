# 🌐 Network Communication Simulation with FreeRTOS 🚀

This project simulates a basic network communication system using **FreeRTOS**. It demonstrates how data is sent and received over a simulated "lossy" network link, where packets can be dropped. The simulation includes two common protocols: **Send and Wait (Stop-and-Wait)** and **Go-Back-N (GBN)**.

## What is this project? 💡

This project models a small network with:
* **Two Sender Nodes (Node 1 & Node 2):** 📤 These nodes generate and send data packets.
* **A Switch Node:** 🔄 This acts like a router, forwarding packets and acknowledgements (ACKs) between senders and receivers. It also introduces delays and simulates packet/ACK loss.
* **Two Receiver Nodes (Node 3 & Node 4):** 📥 These nodes receive the data and send back ACKs to confirm successful reception.

The core idea is to show how these network protocols work in a real-time operating system (RTOS) environment like FreeRTOS.

## Key Features ✨

* **Packet Handling:** 📦 Data is sent in "packets" which have a header (with sequence number, sender, and destination) and a random length payload.
* **Acknowledgement (ACKs):** ✅ Receivers send small ACK packets back to the sender to confirm they got the data.
* **Retransmissions:** 🔄 If a sender doesn't get an ACK back within a certain time (a "timeout" period), it will retransmit the packet. It tries up to 4 times.
* **Go-Back-N (GBN):** 🚀 This is a more advanced sending method where the sender can send multiple packets (`N`) before waiting for an ACK, making communication more efficient than Send and Wait.
* **Performance Measurement:** 📊 The simulation tracks how many packets are successfully received (throughput) and how many times packets had to be retransmitted. It also counts packets that were dropped because they couldn't be retransmitted enough times.

## Why FreeRTOS? 🧠

We used FreeRTOS to manage the different parts of the simulation (like sending, receiving, and switching) as separate "tasks". It also allowed us to use RTOS features like:
* **Tasks:** ⚙️ Each part of the network (sender, receiver, switch) runs as its own independent task.
* **Timers:** ⏱️ Used for packet timeouts and simulating network delays.
* **Queues:** 📩 Used to send packets and ACKs between the different tasks.
* **Semaphores:** 🚦 Used to protect shared resources and control access.

## How it was developed 🛠️

This project was developed for the ELC 2080 course in Spring 2025.

## Files in this repository 📂

* `main.c`: The main C code for the entire simulation logic.
* `Project Report.pdf`: Our submitted project report, which includes detailed system design, results, and analysis.

## Authors ✍️

* Mohamed Nasr Hassan Ali
* Mahmoud Ismail Mahmoud Abdallah
