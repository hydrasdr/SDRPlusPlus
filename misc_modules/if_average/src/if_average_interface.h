/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define IF_AVG_CMD_START          0
#define IF_AVG_CMD_STOP           1
#define IF_AVG_CMD_RESET          2
#define IF_AVG_CMD_GET_STATUS     3   // out: int (0=idle, 1=integrating)
#define IF_AVG_CMD_GET_INT_TIME   4   // out: double (seconds)
#define IF_AVG_CMD_GET_INT_COUNT  5   // out: uint64_t
#define IF_AVG_CMD_EXPORT         6   // Trigger immediate export
#define IF_AVG_CMD_CAPTURE_REF    7   // Capture as reference spectrum
#define IF_AVG_CMD_CAPTURE_HOT    8   // Capture hot cal
#define IF_AVG_CMD_CAPTURE_COLD   9   // Capture cold cal
#define IF_AVG_CMD_SET_MODE       10  // in: int averaging mode
