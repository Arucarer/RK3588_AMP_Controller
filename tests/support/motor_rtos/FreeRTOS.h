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
#define configMINIMAL_STACK_SIZE 512U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10U)
#define configASSERT(condition) assert(condition)
void test_enter_critical(void);
void test_exit_critical(void);
#define taskENTER_CRITICAL() test_enter_critical()
#define taskEXIT_CRITICAL() test_exit_critical()
#endif
