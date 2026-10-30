// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#pragma once

#include <stdint.h>
#include <sys/time.h>

/* SNTP sync callback: records the NTP time together with the monotonic
 * clock. Runs in the TCP/IP task. */
void clock_ntp_cb(struct timeval *tv);

/* Takes over the time recorded by the last clock_ntp_cb() after a successful
 * sync. Measures the drift of the local clock since the previous sync and
 * stores it in NVS. */
void clock_synced(void);

/* Current UTC in us: the last NTP time plus the monotonic time since then,
 * corrected by the measured drift. Valid after clock_synced(). */
int64_t clock_utc_us(void);

/* Estimated standard deviation of the error of clock_utc_us() in ms */
uint64_t clock_error_ms(void);
