// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#pragma once

#include <stdint.h>

/* Scans all channels and returns the one of 1, 6 and 11 with the fewest
 * BSSIDs. The current channel (0: none yet) is kept unless another one has
 * strictly fewer. Wi-Fi must be started in STA mode and not connected. */
uint8_t channel_select(uint8_t current);
