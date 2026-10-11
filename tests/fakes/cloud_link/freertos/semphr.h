#pragma once
#include "FreeRTOS.h"

struct FakeSemaphore;
using SemaphoreHandle_t = FakeSemaphore*;
SemaphoreHandle_t xSemaphoreCreateMutex();
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
