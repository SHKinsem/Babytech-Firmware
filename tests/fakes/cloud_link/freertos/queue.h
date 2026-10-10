#pragma once
#include "FreeRTOS.h"

struct FakeQueue;
using QueueHandle_t = FakeQueue*;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void* item);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
