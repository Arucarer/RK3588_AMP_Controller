#ifndef MOTOR_TEST_QUEUE_H
#define MOTOR_TEST_QUEUE_H
#include "FreeRTOS.h"
typedef struct TestQueue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t);
void vQueueDelete(QueueHandle_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueuePeek(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueOverwrite(QueueHandle_t, const void *);
#endif
