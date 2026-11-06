// -----------------------------------------------------------------------------
// The few Arduino calls the epd97 driver uses, mapped onto ESP-IDF.
// Needs CONFIG_FREERTOS_HZ=1000 (sdkconfig.defaults) so delay(1) is 1 ms.
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_attr.h"

static inline void delay(uint32_t ms)
{
  TickType_t t = pdMS_TO_TICKS(ms);
  vTaskDelay(t ? t : 1);
}

static inline uint32_t millis(void)
{
  return (uint32_t)(esp_timer_get_time() / 1000);
}
