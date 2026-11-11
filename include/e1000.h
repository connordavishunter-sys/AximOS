#pragma once

#include <stddef.h>
#include <stdint.h>

// Looks for a card, returning false if there isn't one.
bool e1000_init(void);

void e1000_mac(uint8_t out[6]);

// Queues a frame, which is copied. Dropped if the queue is full, as a network may do.
void e1000_send(const void *frame, size_t len);

// Hands whatever has arrived to net_receive.
void e1000_poll(void);
