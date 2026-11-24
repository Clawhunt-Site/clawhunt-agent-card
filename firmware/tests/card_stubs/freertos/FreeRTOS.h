// The FreeRTOS calls the card's code makes. Queues are plain first-in-first-out lists; tasks are
// never started: the test runs their work itself.
#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

// Critical sections: the tests run on one thread, so there is nothing to keep out.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
