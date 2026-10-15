#ifndef MOTOR_TEST_TASK_H
#define MOTOR_TEST_TASK_H
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *, TickType_t);
BaseType_t xTaskCreate(TaskFunction_t, const char *, unsigned, void *, UBaseType_t, void *);
#endif
