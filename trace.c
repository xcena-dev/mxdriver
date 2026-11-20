// SPDX-License-Identifier: <SPDX License Expression>

#define CREATE_TRACE_POINTS
#include "trace.h"

#include "mx_dma.h"

static_assert(IO_OPCODE_DATA_READ     == 0,  "trace opcode names out of sync");
static_assert(IO_OPCODE_DATA_WRITE    == 1,  "trace opcode names out of sync");
static_assert(IO_OPCODE_CONTEXT_READ  == 2,  "trace opcode names out of sync");
static_assert(IO_OPCODE_CONTEXT_WRITE == 3,  "trace opcode names out of sync");
static_assert(IO_OPCODE_SQ_READ       == 4,  "trace opcode names out of sync");
static_assert(IO_OPCODE_SQ_WRITE      == 5,  "trace opcode names out of sync");
static_assert(IO_OPCODE_CQ_READ       == 6,  "trace opcode names out of sync");
static_assert(IO_OPCODE_CQ_WRITE      == 7,  "trace opcode names out of sync");
static_assert(IO_OPCODE_PASSTHRU      == 8,  "trace opcode names out of sync");
static_assert(IO_OPCODE_SEND          == 9,  "trace opcode names out of sync");
static_assert(IO_OPCODE_RECV          == 10, "trace opcode names out of sync");

static_assert(DMA_BIDIRECTIONAL == 0, "trace dir names out of sync");
static_assert(DMA_NONE          == 3, "trace dir names out of sync");
