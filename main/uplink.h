// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#pragma once

#include "esp_err.h"

/* Initializes netif, event loop and Wi-Fi (STA mode, started, not connected). */
esp_err_t uplink_init(void);

/* Connects to the uplink networks in the configured order until the system
 * clock was synced via NTP, then disconnects again. */
esp_err_t uplink_sync_time(void);
