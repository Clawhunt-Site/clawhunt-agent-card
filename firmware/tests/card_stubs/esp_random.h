// esp_random() on a computer: a fixed run of numbers (card_host.h: host_random), so a test can say
// which ids the card made up.
#pragma once

#include <stdint.h>

uint32_t esp_random(void);
