#pragma once
/*
 * portal.h - web configuration / login pages.
 *
 * Two modes, selected by `on_sta`:
 *   on_sta = false -> config AP (192.168.4.1): WiFi setup + DNS hijack.
 *   on_sta = true  -> device IP on the LAN: Bambu SMS-code login.
 */
#include <stdbool.h>
#include <esp_err.h>

esp_err_t portal_start(bool on_sta);
