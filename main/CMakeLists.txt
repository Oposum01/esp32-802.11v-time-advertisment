# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
# SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/config.h)
    message(FATAL_ERROR "main/config.h missing, copy main/config.h.example")
endif()

idf_component_register(SRCS "main.c" "beacon.c" "uplink.c" "channel.c" "txpower.c" "clock.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_wifi esp_netif esp_event esp_timer nvs_flash lwip)
