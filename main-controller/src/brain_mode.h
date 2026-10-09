#pragma once

#ifndef BABYTECH_BOARD_LINK_V4
#define BABYTECH_BOARD_LINK_V4 1
#endif

#if BABYTECH_BOARD_LINK_V4 != 1
#error "Only Brain UART v4 is supported; legacy v3 firmware is retired"
#endif
