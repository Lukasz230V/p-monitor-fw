
#include <stdio.h>
#include <inttypes.h>

#include "nimble/ble.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"




#include "ble_config_uuid.h"


// Definicje uchwytów (handles) - teraz w jednym miejscu
uint16_t relay_chr_handle;
uint16_t current_chr_handle;
uint16_t timer_chr_handle;

// 1. Definicja Twojej "Mapy"
uuid_entry_t uuid_map[UUID_MAX_COUNT] = {
    [UUID_STORAGE_SRV] = { .str = "cd9c5081-afd3-4cd5-89f0-e87b649bafe2" },
    [UUID_STORAGE_CHR] = { .str = "4e2cb81c-48e6-4ae3-9ecf-f6ab7f651883" },
    [UUID_RELAY_SRV]   = { .str = "dd630002-58fb-4b83-a7fc-21e10256eec7" },
    [UUID_RELAY_CHR]   = { .str = "dd630012-58fb-4b83-a7fc-21e10256eec7" },
    [UUID_CURRENT_SRV] = { .str = "dd630001-58fb-4b83-a7fc-21e10256eec7" },
    [UUID_CURRENT_CHR] = { .str = "dd630011-58fb-4b83-a7fc-21e10256eec7" },
    [UUID_TIMER_CNT_SVR] = { .str = "35d298a1-fb68-4189-b948-ee0c3fc3b7f6"},
    [UUID_TIMER_CNT_CHR] = { .str = "a9a41b86-4a68-4228-97fa-2ac8347d55b4"},
};

// 2. Funkcja inicjalizująca - wywołaj ją w app_main przed startem NimBLE
void ble_uuid_init_all(void) {
    for (int i = 0; i < UUID_MAX_COUNT; i++) {
        ble_uuid_from_str( &uuid_map[i].gatt,uuid_map[i].str);
    }
}

// 3. Główna definicja usług - czysta i czytelna dzięki enumom
const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = GET_UUID(UUID_STORAGE_SRV),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = GET_UUID(UUID_STORAGE_CHR),
                .access_cb = gatt_svr_saved_param_rw,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = BLE_UUID16_DECLARE(0x2901), .att_flags = BLE_ATT_F_READ, 
                      .access_cb = gatt_svr_dsc_access_name, .arg = "SAVED" },
                    { 0 }
                }, 
            },
            { 0 }
        }
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = GET_UUID(UUID_RELAY_SRV),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = GET_UUID(UUID_RELAY_CHR),
                .access_cb = gatt_svr_relay_status_rwn,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &relay_chr_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = BLE_UUID16_DECLARE(0x2901), .att_flags = BLE_ATT_F_READ, 
                      .access_cb = gatt_svr_dsc_access_name, .arg = "RELAY" },
                    { 0 }
                }, 
            },
            { 0 }
        }
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = GET_UUID(UUID_CURRENT_SRV),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = GET_UUID(UUID_CURRENT_CHR),
                .access_cb = gatt_svr_timer_status_rwn,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &current_chr_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = BLE_UUID16_DECLARE(0x2901), .att_flags = BLE_ATT_F_READ, 
                      .access_cb = gatt_svr_dsc_access_name, .arg = "CURRENT" },
                    { 0 }
                }, 
            },
            { 0 }
        } 
    },
        {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = GET_UUID(UUID_TIMER_CNT_SVR),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = GET_UUID(UUID_TIMER_CNT_CHR),
                .access_cb = gatt_svr_current_status_rwn,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &timer_chr_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = BLE_UUID16_DECLARE(0x2901), .att_flags = BLE_ATT_F_READ, 
                      .access_cb = gatt_svr_dsc_access_name, .arg = "TIMER" },
                    { 0 }
                }, 
            },
            { 0 }
        } 
    },









    { 0 } // Koniec usług
};