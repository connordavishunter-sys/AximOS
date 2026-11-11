#pragma once

#include <interrupt.h>

void syscall_dispatch(struct interrupt_frame *frame);
