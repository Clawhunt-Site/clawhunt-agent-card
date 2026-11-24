#pragma once

#include "freertos/FreeRTOS.h"

typedef struct host_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *arg);

// Remembers the task's function (host_task_run calls it); nothing runs by itself.
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t priority,
                       TaskHandle_t *handle);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
// Where the cloud task waits at the end of each round: here the round ends and control goes back to the test.
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
// A pause in the middle of a round: the clock moves on, and the screen's timers with it.
void vTaskDelay(TickType_t ticks);

// Run one round of the task created under this name; false if there is no such task.
int host_task_round(const char *name);
