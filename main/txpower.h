// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#pragma once

#include "esp_err.h"

/* Sets the maximum TX power to TX_POWER_DBM, or to the value last set via the
 * BOOT button, which lowers it in 1 dB steps (long press: back to
 * TX_POWER_DBM). NVS and Wi-Fi must be
 * initialized, Wi-Fi started. */
esp_err_t txpower_init(void);
