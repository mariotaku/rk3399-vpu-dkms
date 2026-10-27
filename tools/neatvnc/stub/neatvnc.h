/* SPDX-License-Identifier: GPL-2.0 */
/* Stand-ins for the parts of neatvnc's headers that v4l2m2m-impl.c uses. */
#pragma once
#include <stdio.h>
enum { NVNC_LOG_PANIC, NVNC_LOG_ERROR, NVNC_LOG_WARNING, NVNC_LOG_INFO, NVNC_LOG_DEBUG, NVNC_LOG_TRACE };
#define nvnc_log(level, fmt, ...) fprintf(stderr, "neatvnc[%d]: " fmt "\n", level, ##__VA_ARGS__)
#define nvnc_trace(fmt, ...) do { if (getenv("HARNESS_TRACE")) fprintf(stderr, "trace: " fmt "\n", ##__VA_ARGS__); } while (0)
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
