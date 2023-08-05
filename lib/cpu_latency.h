/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef CPU_LATENCY_H
#define CPU_LATENCY_H

#ifdef __cplusplus
extern "C" {
#endif

int start_low_latency(void);
void stop_low_latency(void);

#ifdef __cplusplus
}
#endif

#endif // CPU_LATENCY_H
