#ifndef MOTOR_TEST_FREERTOS_H
#define MOTOR_TEST_FREERTOS_H
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define configTICK_RATE_HZ 100U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10U)
#define configASSERT(condition) assert(condition)
#endif
