#pragma once


// #include "nimble/ble.h"
// #include "host/ble_uuid.h"
// //#include "services/gatt/ble_gatt.h"
// #include "services/gatt/ble_svc_gatt.h"
// #include "services/gap/ble_svc_gap.h"

#define GET_UUID(id) (&uuid_map[id].gatt.u)

// Struktura pomocnicza
typedef struct {
    const char *str;
    ble_uuid_any_t gatt;
} uuid_entry_t;

// 1. Przestrzeń nazw - Enum jako jedyne źródło prawdy o nazwach
typedef enum {
    UUID_STORAGE_SRV,
    UUID_STORAGE_CHR,
    UUID_RELAY_SRV,
    UUID_RELAY_CHR,
    UUID_CURRENT_SRV,
    UUID_CURRENT_CHR,
    UUID_TIMER_CNT_SVR,
    UUID_TIMER_CNT_CHR,
    UUID_MAX_COUNT
} uuid_id_t;

extern const struct ble_gatt_svc_def gatt_svcs[];
extern uint16_t relay_chr_handle;
extern uint16_t current_chr_handle;
extern uint16_t timer_chr_handle;


// Jeśli używasz nowej struktury z enumem (zalecane):
extern uuid_entry_t uuid_map[]; 





// Makro ułatwiające dostęp w tabeli GATT (zwraca wskaźnik na ble_uuid_t)




// Deklaracja tablicy (będzie zdefiniowana w .c)
//extern uuid_entry_t uuid_map[UUID_MAX_COUNT];

// Deklaracja usług (będzie zdefiniowana w .c)
extern const struct ble_gatt_svc_def gatt_svcs[];

// Prototypy funkcji (twoje callbacki)
void ble_uuid_init_all(void);
int gatt_svr_saved_param_rw(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
int gatt_svr_relay_status_rwn(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
int gatt_svr_current_status_rwn(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
int gatt_svr_timer_status_rwn(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
int gatt_svr_dsc_access_name(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);

