#pragma once
/*
 * bambu_mqtt.h - Bambu cloud MQTT status feed.
 * TLS to broker (cn|us).mqtt.bambulab.com:8883, username "u_{uid}",
 * password = accessToken. Subscribes device/{dev_id}/report and parses the
 * "print" object into g_state.
 */
#include <stdbool.h>
#include <esp_err.h>
#include "bambu_api.h"

/* Start (or restart) the cloud MQTT client. */
esp_err_t bambu_mqtt_start(const bambu_auth_t *auth, const bambu_dev_t *dev);

/* Stop and destroy the client (blocking). */
void bambu_mqtt_stop(void);

/* Publish a status request (pushall). Call while connected, every few s. */
void bambu_mqtt_request_status(void);

bool bambu_mqtt_is_up(void);
