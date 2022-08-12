/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef COMMON_H
#define COMMON_H

#ifndef O_PATH
#define O_PATH		010000000
#endif
#define AT_EMPTY_PATH		0x1000	/* Allow empty relative pathname */

#define MIN(x, y) x < y ? x : y
#define MAX(x, y) x > y ? x : y

#endif // COMMON_H