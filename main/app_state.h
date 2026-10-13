#pragma once
/*
 * app_state.h - shared runtime state between cloud task, UI task and portal.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum {
    APP_PHASE_BOOT = 0,
    APP_PHASE_PORTAL,       /* AP provisioning: WiFi only (A1MON-XXXX)    */
    APP_PHASE_LOGIN_PORTAL, /* STA online: cloud login web page on the IP  */
    APP_PHASE_WIFI_CONN,    /* connecting to WiFi                         */
    APP_PHASE_WIFI_FAIL,    /* WiFi failed, falling back to portal        */
    APP_PHASE_LOGIN,        /* logging into Bambu cloud                   */
    APP_PHASE_DEVICE,       /* fetching device list                       */
    APP_PHASE_MQTT_CONN,    /* connecting cloud MQTT                      */
    APP_PHASE_RUNNING,      /* MQTT connected, monitoring                 */
    APP_PHASE_PORTAL_WAIT,  /* login failed too often -> reboot to portal */
    APP_PHASE_ERROR,        /* fatal-ish error, will retry                */
} app_phase_t;

typedef struct {
    /* connection / lifecycle */
    app_phase_t phase;
    char        err[28];        /* short ASCII message for the OLED        */
    char        ip[16];         /* ESP32 station IP                        */
    char        ap_ssid[24];    /* config-mode AP name                     */

    /* device */
    char        dev_name[24];
    char        dev_model[16];
    bool        dev_online;     /* online flag from device list            */
    bool        mqtt_up;        /* broker connection alive                 */
    bool        auth_failed;    /* broker refused credentials -> re-login  */
    bool        got_report;     /* at least one report received            */
    uint32_t    last_report_ms; /* esp_timer ms of last accepted report    */

    /* print status (from device/{id}/report JSON) */
    char        gcode_state[16];
    int         percent;        /* 0..100                                  */
    float       nozzle_temp;
    float       bed_temp;
    int         nozzle_target;
    int         bed_target;
    int         remain_min;     /* estimated remaining minutes             */
    int         layer;
    int         total_layer;
    int         spd_lvl;        /* Bambu spd_level: 1 silent 2 std 3 sport 4 ludicrous */
    bool        has_ams;
    int         tray_now;
    char        tray_type[8];
    int         printer_rssi;   /* dBm as reported by the printer          */
    uint32_t    print_error;    /* non-zero = printer error code           */
} monitor_state_t;

extern monitor_state_t g_state;
extern SemaphoreHandle_t g_state_mux;

void     app_state_init(void);
uint32_t now_ms(void);
