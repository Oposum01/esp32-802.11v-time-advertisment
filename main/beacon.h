// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Starts sending raw beacons on the given channel carrying the Time
 * Advertisement and Time Zone elements, and answering probe requests if
 * PROBE_RESPONSES is set. clock_synced() must have been called. Wi-Fi must be
 * started in STA mode and not connected. */
esp_err_t beacon_start(uint8_t channel);

/* To be called after every successful NTP sync, before beacon_start().
 * Increments the Time Update Counter. */
void beacon_time_synced(void);

void beacon_stop(void);
