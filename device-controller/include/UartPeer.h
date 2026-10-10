#pragma once
#define MOTION_UART_PEER_PRODUCT_BRAIN 4
#ifndef MOTION_UART_PEER
#define MOTION_UART_PEER MOTION_UART_PEER_PRODUCT_BRAIN
#endif
#define MOTION_HAS_PRODUCT 1
#if MOTION_UART_PEER != MOTION_UART_PEER_PRODUCT_BRAIN
#error "Only Motion UART v4 (MOTION_UART_PEER=4) is supported; legacy peers 1/2 are retired"
#endif
