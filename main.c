#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "diag/trace.h"
#include "semphr.h"

// #define PACKET_TOTAL_SIZE
#define L1                  500
#define L2                  1500
#define N                   1   // Window size for Go-Back-N
#define RETRY_LIMIT         4
#define T1                  100
#define T2                  200

#define ACK_TOTAL_SIZE      40
#define HEADER_SIZE         (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint16_t))
// #define PAYLOAD_SIZE        (PACKET_TOTAL_SIZE - HEADER_SIZE)
// #define DROP_PROBABILITY    0.01
#define MAX_SENDERS         2

#define capacity            100000
#define propagation_delay   5   // D
#define ack_drop            0.01

uint32_t expected[2][MAX_SENDERS] = {{0}};
int p_drop, Total_Packet_Size, payload_size, Tout;
SemaphoreHandle_t xSwitchReadyMutex; // to control the train-like behavior at the switch

/* Global stats */
volatile uint32_t dropped = 0, ack_dropped, received3 = 0, received4 = 0, lost3 = 0, lost4 = 0;
volatile uint32_t total_transmissions = 0, dropped_due_to_limit = 0, total_retransmissions = 0;

/* Packet structure */
typedef struct __attribute__((aligned(4))) {
    uint32_t seq_num;
    uint16_t length;
    uint8_t  sender_id; // 1 or 2
    uint8_t  dest;      // 3 or 4
    uint8_t  payload[];
} Packet;

typedef struct __attribute__((aligned(4))) {
    uint32_t seq_num;
    uint8_t  sender_id; // 3 or 4
    uint8_t  dest;      // 1 or 2
    uint8_t  padding[ACK_TOTAL_SIZE - (sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint8_t))];
} Ack_Packet;

typedef struct __attribute__((aligned(4))) BufferNode {
    Packet         *pkt;
    uint8_t         retries;
    TimerHandle_t   timer;
    uint8_t         alive;  // 1 => still in buffer, 0 => popped
    struct BufferNode *next;
} BufferNode;

static QueueHandle_t xQueueSwitchInput;
static QueueHandle_t xQueueReceiver3;
static QueueHandle_t xQueueReceiver4;
static QueueHandle_t xQueueSwitchReceiver;
static QueueHandle_t xQueueSender1;
static QueueHandle_t xQueueSender2;

// Separate buffers per sender
BufferNode *head1 = NULL, *tail1 = NULL;
BufferNode *head2 = NULL, *tail2 = NULL;
SemaphoreHandle_t BufferMutex1;
SemaphoreHandle_t BufferMutex2;

uint16_t buffer1_size = 0;
uint16_t buffer2_size = 0;

void buffer_push(Packet *pkt, uint8_t sender) {
    BufferNode *node = (BufferNode *) pvPortMalloc(sizeof(BufferNode));
    if (!node) return;

    node->pkt = pkt;
    node->retries = 0;
    node->timer = NULL;
    node->alive = 1;
    node->next = NULL;

    SemaphoreHandle_t mutex = (sender == 1) ? BufferMutex1 : BufferMutex2;
    BufferNode **head = (sender == 1) ? &head1 : &head2;
    BufferNode **tail = (sender == 1) ? &tail1 : &tail2;

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!*head) {
        *head = *tail = node;
    } else {
        (*tail)->next = node;
        *tail = node;
    }
    if (sender == 1) buffer1_size++;
    else            buffer2_size++;
    xSemaphoreGive(mutex);

    // trace_printf("gen[%u] pushed \n", sender);
}

void buffer_pop(uint8_t sender) {
    SemaphoreHandle_t mutex = (sender == 1) ? BufferMutex1 : BufferMutex2;
    BufferNode **head = (sender == 1) ? &head1 : &head2;
    BufferNode **tail = (sender == 1) ? &tail1 : &tail2;

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!*head) {
        xSemaphoreGive(mutex);
        return;
    }
    BufferNode *temp = *head;
    *head = (*head)->next;
    if (!*head) *tail = NULL;
    xSemaphoreGive(mutex);

    temp->alive = 0;
    if (temp->timer) {
        xTimerStop(temp->timer, portMAX_DELAY);
        xTimerDelete(temp->timer, portMAX_DELAY);
        temp->timer = NULL;
    }

    vPortFree(temp->pkt);
    vPortFree(temp);
    if (sender == 1) buffer1_size--;
    else            buffer2_size--;
    // trace_printf("Buffer[%d] popped \n", sender);
}


BufferNode* buffer_peek(uint8_t sender) {
    SemaphoreHandle_t mutex = (sender == 1) ? BufferMutex1 : BufferMutex2;
    BufferNode *node = (sender == 1) ? head1 : head2;

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (node && node->alive == 1) {
        xSemaphoreGive(mutex);
        // trace_printf("sender[%u] peeked, current head---> from: %d , to: %d , seq:%d, alive?: %d \n",
        //              sender, node->pkt->sender_id, node->pkt->dest, node->pkt->seq_num, node->alive);
        return node;
    }
    xSemaphoreGive(mutex);
    return NULL;
}

/* ----------------------------- Timer Callbacks ----------------------------- */

static void vPacketToutCallback(TimerHandle_t xTimer) {
    if (!xTimer) return;
    BufferNode *buffer_pkt = (BufferNode *)pvTimerGetTimerID(xTimer);
    if (!buffer_pkt || buffer_pkt->alive == 0) {
        trace_printf("[TimerCB] NULL or dead packet from timer!\n");
        return;
    }
    if (buffer_pkt->retries < RETRY_LIMIT && buffer_pkt->alive == 1) {
        if (xQueueSend(xQueueSwitchInput, &(buffer_pkt->pkt), 0) == pdTRUE) {
            total_transmissions++;
            total_retransmissions++;
            trace_printf("pkt from %d, retransmitted to [%d], seq=%u, trial no.: %d \n",
                        buffer_pkt->pkt->sender_id, buffer_pkt->pkt->dest,
                         buffer_pkt->pkt->seq_num, buffer_pkt->retries);
            if (buffer_pkt->timer) {
                xTimerReset(buffer_pkt->timer, 0);
            }
            buffer_pkt->retries++;

        }
        else {
            trace_printf("pkt failed to retransmit to [%d], seq=%u\n",
                          buffer_pkt->pkt->dest, buffer_pkt->pkt->seq_num);
        }
    } else if (buffer_pkt->retries >= RETRY_LIMIT) {
    	trace_printf("LIMIT REACHED!, pkt_seq: %d, retries: %d \n", buffer_pkt->pkt->seq_num, buffer_pkt->retries);
    	dropped_due_to_limit++;
    	buffer_pop(buffer_pkt->pkt->sender_id);
    }
}

static void vPacketDelayCallback(TimerHandle_t xTimer) {
    if (!xTimer) return;
    Packet *pkt = (Packet *)pvTimerGetTimerID(xTimer);
    if (!pkt) {
        trace_printf("[TimerCB] NULL packet from timer!\n");
        xTimerDelete(xTimer, 0);
        return;
    }

    BaseType_t success = pdFALSE;
    if (pkt->dest == 3) {
        success = xQueueSend(xQueueReceiver3, &pkt, 0);
    } else if (pkt->dest == 4) {
        success = xQueueSend(xQueueReceiver4, &pkt, 0);
    } else {
        trace_printf("[TimerCB] Invalid dest=%u\n", pkt->dest);
        xTimerDelete(xTimer, 0);
        return;
    }

    if (success != pdPASS) {
        trace_printf("[TimerCB] Failed to enqueue to Receiver %u\n", pkt->dest);
    } else {
        trace_printf("[Switch] Forwarded to %u seq=%u\n", pkt->dest, pkt->seq_num);
    }

    xTimerDelete(xTimer, 0);
}

static void vAckDelayCallback(TimerHandle_t xTimer) {
    if (!xTimer) return;
    Ack_Packet *ack_pkt = (Ack_Packet *)pvTimerGetTimerID(xTimer);
    if (!ack_pkt) {
        trace_printf("[TimerCB] NULL ACK from timer!\n");
        xTimerDelete(xTimer, 0);
        return;
    }

    BaseType_t success = pdFALSE;
    if (ack_pkt->dest == 1) {
        success = xQueueSend(xQueueSender1, &ack_pkt, 0);
    } else if (ack_pkt->dest == 2) {
        success = xQueueSend(xQueueSender2, &ack_pkt, 0);
    } else {
        vPortFree(ack_pkt);
        xTimerDelete(xTimer, 0);
        return;
    }

    if (success != pdPASS) {
        vPortFree(ack_pkt);
    } else {
        trace_printf("[Switch-ACK] Forwarded to %u seq=%u\n", ack_pkt->dest, ack_pkt->seq_num);
    }
    xTimerDelete(xTimer, 0);
}

/* ----------------------------- PacketGen & Sender Task ----------------------------- */

static void vPacketGenTask(void *pvParameters) {
    int sender_id = (int)(intptr_t)pvParameters; // 1 or 2
    uint32_t seq3 = 0, seq4 = 0;
    for (;;) {
        Total_Packet_Size = rand() % (L2 - L1 + 1) + L1;
        Packet *pkt;
        payload_size = Total_Packet_Size - sizeof(*pkt);
        pkt = (Packet *) pvPortMalloc(sizeof(Packet) + payload_size);
        if (!pkt) {
            trace_printf("[Gen-%d] Failed to allocate memory for packet\n", sender_id);
            continue;
        }
        pkt->sender_id = sender_id;
        pkt->dest = (rand() % 2 == 0) ? 3 : 4;
        pkt->seq_num = (pkt->dest == 3) ? seq3++ : seq4++;
        pkt->length = Total_Packet_Size;
        memset(pkt->payload, 0xAA, payload_size);
        // trace_printf("[Gen-%d] Packet dest=%u seq=%u\n", sender_id, pkt->dest, pkt->seq_num);

        buffer_push(pkt, sender_id);
        int delay = (rand() % T1) + T1;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void vSender(void *pvParameters) {
    uint8_t id = (int)(intptr_t)pvParameters;  // 1 or 2
    QueueHandle_t q = (id == 1) ? xQueueSender1 : xQueueSender2;
    Ack_Packet *ack_pkt;

    for (;;) {
        // Process incoming ACKs
        while (xQueueReceive(q, &ack_pkt, 0) == pdTRUE) {
        	// trace_printf("stuck 270");
            if (!ack_pkt) continue;

            // trace_printf("Ack received at sender: %d, from: %d, seq: %d \n",
            //              id, ack_pkt->sender_id, ack_pkt->seq_num);


            uint32_t ack_seq = ack_pkt->seq_num;
            vPortFree(ack_pkt);


            BufferNode *head = (id == 1) ? head1 : head2;
            while (head && head->alive == 1 && head->pkt->seq_num <= ack_seq) {
                buffer_pop(id);
                head = (id == 1) ? head1 : head2;
            }
        }

        // Send up to N packets in window
        uint16_t in_flight = 0;
        BufferNode *cursor = (id == 1) ? head1 : head2;
        for (int i = 0; cursor && i < N; ++i) {
            if (cursor->timer != NULL) {
            	trace_printf("current head---> from: %d , to: %d , seq:%d, alive?: %d, retries: %d \n", cursor->pkt->sender_id, cursor->pkt->dest, cursor->pkt->seq_num, cursor->alive, cursor->retries);
                // if (cursor->retries >= RETRY_LIMIT) dropped_due_to_limit++;
            	in_flight++;
            }
            cursor = cursor->next;
        }


        while (in_flight < N) {
            BufferNode *walker = (id == 1) ? head1 : head2;
            while (walker && walker->retries > 0) {
            	// if ()
                walker = walker->next;
            }
            if (!walker) break; // no more packets to send

            // Create and start timer for this packet
            TimerHandle_t Tout_Timer = xTimerCreate(
                "Tout_Timer",
                pdMS_TO_TICKS(Tout),
                pdFALSE,
                (void *)walker,
                vPacketToutCallback
            );
            if (Tout_Timer == NULL) {
                trace_printf("Tout Timer creation failed for seq=%u\n", walker->pkt->seq_num);
                buffer_pop(id);
                continue;
            }
            walker->timer = Tout_Timer;

            // Transmit packet
            xSemaphoreTake(xSwitchReadyMutex, portMAX_DELAY);
            if (xQueueSend(xQueueSwitchInput, &(walker->pkt), 0) != pdPASS) {
                trace_printf("[Gen-%d] SwitchInput queue is full, dropping packet seq=%u\n",
                             walker->pkt->sender_id, walker->pkt->seq_num);
                buffer_pop(id);
                xSemaphoreGive(xSwitchReadyMutex);
                continue;
            }
            walker->retries += 1;
            total_transmissions++;
            //trace_printf("first send, pkt sent from %d , seq: %u \n",
            //            walker->pkt->sender_id, walker->pkt->seq_num);

            if (xTimerStart(Tout_Timer, 0) != pdPASS) {
                trace_printf("Tout Timer start failed for seq=%u\n", walker->pkt->seq_num);
                xTimerDelete(Tout_Timer, 0);
                walker->timer = NULL;
            }
            xSemaphoreGive(xSwitchReadyMutex);

            in_flight++;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ----------------------------- Switch Task ----------------------------- */
static void SwitchTask(void *pvParameters) {
    Packet *pkt;
    Ack_Packet *ack_pkt;

    for (;;) {
        if (xQueueReceive(xQueueSwitchInput, &pkt, 0) == pdTRUE) {
            if (!pkt) continue;
            if (pkt->dest != 3 && pkt->dest != 4) continue;
            // trace_printf("[Switch] Received dest=%u seq=%u\n", pkt->dest, pkt->seq_num);
            uint8_t rand_value = rand() % 100;
            if (rand_value < p_drop) {
                trace_printf("[Switch] Dropped dest=%u seq=%u\n", pkt->dest, pkt->seq_num);
                dropped++;
                continue;
            }

            int transmission_delay = (pkt->length * 8000) / capacity;  // in ms
            int total_delay = transmission_delay + 2 * propagation_delay;
            TimerHandle_t hTimer = xTimerCreate(
                "PktDelay",
                pdMS_TO_TICKS(total_delay),
                pdFALSE,
                (void *)pkt,
                vPacketDelayCallback
            );
            if (hTimer) {
                if (xTimerStart(hTimer, 0) != pdPASS) {
                    trace_printf("[Switch] Timer start failed for seq=%u\n", pkt->seq_num);
                    xTimerDelete(hTimer, 0);
                }
            } else {
                trace_printf("[Switch] Timer creation failed for seq=%u\n", pkt->seq_num);
            }
        } else {
            trace_printf("SwitchInput Q is empty \n");
        }
        // trace_printf("passed switch packets\n");

        if (xQueueReceive(xQueueSwitchReceiver, &ack_pkt, 0) == pdTRUE) {
            // trace_printf("ACK at switch now, from: %d, to: %d, seq: %d \n",
            //              ack_pkt->sender_id, ack_pkt->dest, ack_pkt->seq_num);
            if (!ack_pkt) continue;
            if (rand() % 1000 < (ack_drop * 1000)) {
                ack_dropped++;
                vPortFree(ack_pkt);
                continue;
            }

            int ack_transmission_delay = (ACK_TOTAL_SIZE * 8000) / capacity;  // in ms
            int ack_total_delay = ack_transmission_delay + 2 * propagation_delay;
            TimerHandle_t ack_Timer = xTimerCreate(
                "AckDelay",
                pdMS_TO_TICKS(ack_total_delay),
                pdFALSE,
                (void *)ack_pkt,
                vAckDelayCallback
            );
            if (ack_Timer) {
                if (xTimerStart(ack_Timer, 0) != pdPASS) {
                    vPortFree(ack_pkt);
                    xTimerDelete(ack_Timer, 0);
                }
            } else {
                vPortFree(ack_pkt);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ----------------------------- Receiver Task ----------------------------- */
static void ReceiverTask(void *pvParameters) {
    int id = (int)(intptr_t)pvParameters;  // 3 or 4
    int idx = (id == 3) ? 0 : 1;
    QueueHandle_t q = (id == 3) ? xQueueReceiver3 : xQueueReceiver4;
    Packet *pkt;
    Ack_Packet *ack_pkt;

    for (;;) {
        if (xQueueReceive(q, &pkt, portMAX_DELAY) == pdTRUE) {
            if (!pkt) continue;

            uint8_t sid = pkt->sender_id;
            if (sid < 1 || sid > MAX_SENDERS) {
                trace_printf("[Recv%d] Invalid sender_id=%u\n", id, sid);
                continue;
            }

            int sid_idx = sid - 1;
            // trace_printf("[Recv%d] Got seq=%u from sender %u\n", id, pkt->seq_num, sid);

            if (pkt->seq_num > expected[idx][sid_idx] &&
                (pkt->seq_num - expected[idx][sid_idx] <= 50)) {
                uint32_t lost = pkt->seq_num - expected[idx][sid_idx];
                ack_pkt = (Ack_Packet *)pvPortMalloc(sizeof(Ack_Packet));
                if (!ack_pkt) {
                    trace_printf("[Recv%d] Failed to allocate memory for ACK\n", id);
                    continue;
                }

                ack_pkt->sender_id = id;
                ack_pkt->dest = pkt->sender_id;
                ack_pkt->seq_num = pkt->seq_num;
                memset(ack_pkt->padding, 0, sizeof(ack_pkt->padding));

                if (xQueueSend(xQueueSwitchReceiver, &ack_pkt, 0) != pdPASS) {
                    trace_printf("[Recv%d] Switch queue full, dropping ACK seq=%u\n", id, ack_pkt->seq_num);
                    vPortFree(ack_pkt);
                }

                expected[idx][sid_idx] = pkt->seq_num + 1;
                if (id == 3) { received3++; lost3 += lost; }
                else         { received4++; lost4 += lost; }
                dropped += lost;
                trace_printf("[Recv%d] Lost %u packets from sender %u\n", id, lost, sid);

            } else if (pkt->seq_num < expected[idx][sid_idx]) {
                trace_printf("[Recv%d] Duplicate packet from sender %u\n", id, sid);
                ack_pkt = (Ack_Packet *)pvPortMalloc(sizeof(Ack_Packet));
                if (!ack_pkt) {
                    trace_printf("[Recv%d] Failed to allocate memory for ACK\n", id);
                    continue;
                }
                ack_pkt->sender_id = id;
                ack_pkt->dest = pkt->sender_id;
                ack_pkt->seq_num = expected[idx][sid_idx] - 1;
                memset(ack_pkt->padding, 0, sizeof(ack_pkt->padding));

                if (xQueueSend(xQueueSwitchReceiver, &ack_pkt, 0) != pdPASS) {
                    trace_printf("[Recv%d] Switch queue full, dropping ACK seq=%u\n", id, ack_pkt->seq_num);
                    vPortFree(ack_pkt);
                }

            } else if (pkt->seq_num == expected[idx][sid_idx]) {
                ack_pkt = (Ack_Packet *)pvPortMalloc(sizeof(Ack_Packet));
                if (!ack_pkt) {
                    trace_printf("[Recv%d] Failed to allocate memory for ACK\n", id);
                    continue;
                }

                ack_pkt->sender_id = id;
                ack_pkt->dest = pkt->sender_id;
                ack_pkt->seq_num = pkt->seq_num;
                memset(ack_pkt->padding, 0, sizeof(ack_pkt->padding));

                if (xQueueSend(xQueueSwitchReceiver, &ack_pkt, 0) != pdPASS) {
                    trace_printf("[Recv%d] Switch queue full, dropping ACK seq=%u\n", id, ack_pkt->seq_num);
                    vPortFree(ack_pkt);
                }

                expected[idx][sid_idx] = pkt->seq_num + 1;
                if (id == 3) { received3++; }
                else         { received4++; }
            }

            if (pkt->dest != id) {
                trace_printf("[Recv%d] ERROR: wrong dest field %u\n", id, pkt->dest);
            }
        } else {
            trace_printf("RecQ[%d] is empty \n", id);
        }
    }
}

/* ----------------------------- Monitor Task ----------------------------- */
static void MonitorTask(void *pvParameters) {
    const uint32_t target_per_rec = 1000;
    TickType_t lastPrint = xTaskGetTickCount();
    TickType_t starttime = xTaskGetTickCount();
    TickType_t endtime = 0;
    size_t xPortGetFreeHeapSize(void);
    size_t xPortGetMinimumEverFreeHeapSize(void);

    for (;;) {
        vTaskDelayUntil(&lastPrint, pdMS_TO_TICKS(50));
        uint32_t total_received = received3 + received4;

        trace_printf("\n\n\n---------------------[Monitor] Total received: %u | Recv3=%u Lost3=%u | Recv4=%u Lost4=%u, Drop_due_to_limit: %i -----------------\n",
                     total_received, received3, lost3, received4, lost4, dropped_due_to_limit);
        trace_printf("\n total retransmissions: %i \n", total_retransmissions);
        trace_printf("\n\n[Heap] Free: %u | Min ever: %u \n\n\n",
                     xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize());

        if (received3 >= target_per_rec && received4 >= target_per_rec) {
        	endtime = xTaskGetTickCount();
        	TickType_t  elapsedtime = endtime - starttime;
        	double elapsed_sec = ((double)elapsedtime) / configTICK_RATE_HZ;

        	double raw_throughput = ((double)total_received) / elapsed_sec;           // packets/sec
        	double raw_avr_trans = ((double)total_transmissions) / (double)total_received;

        	double tmp;
        	double frac_th = modf(raw_throughput, &tmp);
        	int int_th    = (int)tmp;                   // integer part
        	int dec_th    = (int)(frac_th * 100.0 + 0.5); // two decimal digits, rounded

        	double frac_at = modf(raw_avr_trans, &tmp);
        	int int_at   = (int)tmp;
        	int dec_at   = (int)(frac_at * 100.0 + 0.5);

        	trace_printf("\n=== FINAL STATS (Limit packets reached) ===\n");
        	trace_printf("Throughput: %d.%02d KPps, Avr_Trans_per_packet: %d.%02d, Drop_due_to_limit: %i \n", int_th, dec_th, int_at, dec_at, dropped_due_to_limit);
        	vTaskEndScheduler();
        }
    }
}

/* ----------------------------- Main ----------------------------- */
int main(void) {
    srand((unsigned int)time(NULL));
    float p_drop_set[] = {0.02, 0.04, 0.08};
    uint8_t Tout_set[] = {150, 175, 200, 225};
    p_drop = 2;    // percent for drop
    Tout = 150;

    // Create queues for packet routing
    xQueueSwitchInput = xQueueCreate(10, sizeof(Packet *));
    xQueueReceiver3   = xQueueCreate(10, sizeof(Packet *));
    xQueueReceiver4   = xQueueCreate(10, sizeof(Packet *));
    xQueueSwitchReceiver = xQueueCreate(10, sizeof(Ack_Packet *));
    xQueueSender1     = xQueueCreate(10, sizeof(Ack_Packet *));
    xQueueSender2     = xQueueCreate(10, sizeof(Ack_Packet *));
    configASSERT(xQueueSwitchInput && xQueueReceiver3 && xQueueReceiver4 && xQueueSwitchReceiver);

    BufferMutex1     = xSemaphoreCreateMutex();
    BufferMutex2     = xSemaphoreCreateMutex();
    xSwitchReadyMutex = xSemaphoreCreateMutex();
    configASSERT(BufferMutex1 && BufferMutex2 && xSwitchReadyMutex);

    // Create tasks
    xTaskCreate(vPacketGenTask, "PktGen1", 1024, (void*)1, 2, NULL);
    xTaskCreate(vPacketGenTask, "PktGen2", 2048, (void*)2, 2, NULL);
    xTaskCreate(vSender,         "Sender1", 1024, (void*)1, 2, NULL);
    xTaskCreate(vSender,         "Sender2", 1024, (void*)2, 2, NULL);
    xTaskCreate(SwitchTask,      "Switch",  2048, NULL,     2, NULL);
    xTaskCreate(ReceiverTask,    "Recv3",   1024, (void*)3, 1, NULL);
    xTaskCreate(ReceiverTask,    "Recv4",   1024, (void*)4, 1, NULL);
    xTaskCreate(MonitorTask,     "Monitor", 1024, NULL,     1, NULL);
    trace_printf("created tasks...\n");

    // Start the scheduler
    vTaskStartScheduler();

    // Should never reach here
    for (;;);
}

#pragma GCC diagnostic pop

#ifdef __cplusplus
extern "C" {
#endif

void vApplicationMallocFailedHook(void)             { for(;;); }
void vApplicationStackOverflowHook(TaskHandle_t px, char *pn) { (void)px; (void)pn; for(;;); }
void vApplicationIdleHook(void)                     { }
void vApplicationTickHook(void)                     { }

void vApplicationGetIdleTaskMemory(StaticTask_t **pxTCB, StackType_t **pxStack, uint32_t *pSize) {
    static StaticTask_t tcb;
    static StackType_t  stack[configMINIMAL_STACK_SIZE];
    *pxTCB   = &tcb;
    *pxStack = stack;
    *pSize   = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **pxTCB, StackType_t **pxStack, uint32_t *pSize) {
    static StaticTask_t tcb;
    static StackType_t  stack[configTIMER_TASK_STACK_DEPTH];
    *pxTCB   = &tcb;
    *pxStack = stack;
    *pSize   = configTIMER_TASK_STACK_DEPTH;
}

#ifdef __cplusplus
}
#endif
