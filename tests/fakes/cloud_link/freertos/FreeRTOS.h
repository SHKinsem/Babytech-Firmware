#pragma once
#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;
using TaskFunction_t = void (*)(void*);
using TaskHandle_t = void*;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(value) (value)

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char* name,
    uint32_t stack, void* context, UBaseType_t priority, TaskHandle_t* handle, BaseType_t core);
void vTaskDelay(TickType_t ticks);
