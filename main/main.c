/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/touch_pad.h"

#include "nvs_flash.h"

#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

//#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"





#include "esp_log.h"
#include "math.h"

#include "ble_config_uuid.h"
#include "main.h"

static const uint16_t fw = 1148; // firmware version
// --- LOGI I TAGI ---
static const char *TAG = "ADC_CONT";
static const char *MEM = "NVS";

// --- KONFIGURACJA SPRZĘTU (GPIO/ADC) ---
#define RELAY               GPIO_NUM_4
 #define CURR_ADC_UNIT       ADC_UNIT_1
 #define CURR_ADC_CHANNEL    ADC_CHANNEL_4
#define ADC_MAX_STORE_BUF_SIZE 4096
#define ADC_CONV_FRAME_SIZE   1024

// // --- PARAMETRY POMIAROWE I FILTRY ---
 #define RMS_WINDOW             2000    // 100ms przy 20kHz
 #define DEADBAND               0.8f//1.8f//0.6f    // minimalny sygnał

  #define RMS_FILTER_ALPHA       0.3f    //0.2
  #define CURRENT_SCALE          0.0501f // 0.38


#define ADC_SAMPLE_FREQ_HZ       20000U
#define RELAY_SETTLE_MS          150U
//#define RELAY_SETTLE_SAMPLES     ((ADC_SAMPLE_FREQ_HZ * RELAY_SETTLE_MS) / 1000U)
#define OFFSET_MIN_SAMPLES       500U


//#define ADC_SAMPLE_FREQ_HZ       20000U
#define RELAY_SETTLE_MS          150U

// Maksymalny zapas na dane oczekujące w buforze ADC
// oraz jedną ramkę przekazywaną do zadania.
#define ADC_QUEUE_LATENCY_MS \
    (((ADC_MAX_STORE_BUF_SIZE + ADC_CONV_FRAME_SIZE) * 1000U + \
      (SOC_ADC_DIGI_RESULT_BYTES * ADC_SAMPLE_FREQ_HZ - 1U)) / \
     (SOC_ADC_DIGI_RESULT_BYTES * ADC_SAMPLE_FREQ_HZ))

// Czas stabilizacji + zapas na zalegające próbki ADC.
#define RELAY_SETTLE_SAMPLES \
    ((ADC_SAMPLE_FREQ_HZ * \
      (RELAY_SETTLE_MS + ADC_QUEUE_LATENCY_MS) + 999U) / 1000U)


// #define OFFSET_MAX_STEP        50.0f
 #define OFFSET_BROADCAST_INTERVAL_MS 500

#define BIT_TIMER (1 << 0)
#define BIT_QUEUE (1 << 1)
#define SETTINGS_CHANGED_BIT BIT0

#define USE_TPAD 1

#if USE_TPAD == 1
    #define TP_BTN                  TOUCH_PAD_NUM6   // Twój zdefiniowany pin (z logu wynika, że to T3)
    #define TOUCH_SAMPLE_PERIOD_MS  50             // Częstsze odpytywanie dla lepszej responsywności
    #define TOUCH_THRESHOLD_PER_MIL 38            // 25/1000 = 3.8% powyżej benchmarku
    #define TOUCH_HYST_PER_MIL      20             // 10/1000 = 2.0% histerezy
#else
    #define SW_BTN GPIO_NUM_6
#endif

/**
 * @brief Typ wyliczeniowy definiujący dostępne operacje na przekaźniku.
 */
typedef enum {
    RL_GET    = -1, /**< Tylko odczyt bieżącego stanu. */
    RL_OFF    =  0, /**< Wyłączenie przekaźnika. */
    RL_ON     =  1, /**< Włączenie przekaźnika. */
    RL_TOGGLE =  2  /**< Przełączenie stanu na przeciwny. */
} relay_action_t;

EventGroupHandle_t s_settings_event_group = NULL;
TimerHandle_t xPwrOnOffTimer = NULL; // timer sekundnik
TaskHandle_t xLogicTaskHandle = NULL;
QueueHandle_t xQueueAdcHandle = NULL;

// --- ZMIENNE STANU POMIARÓW ---
float dynamic_offset = 0.0f;
float sum_sq = 0.0f;
int total_samples = 0;

// --- USTAWIENIA I SYNCHRONIZACJA ---
//app_settings_t g_app_settings;
static portMUX_TYPE relay_mux = portMUX_INITIALIZER_UNLOCKED;

// --- ZMIENNE BLUETOOTH ---
#define MAX_BLE_CONNECTIONS 3
uint8_t ble_addr_type;
uint16_t g_active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_notify_states[MAX_BLE_CONNECTIONS] = {false};

// --- UCHWYTY STEROWNIKÓW ---
static adc_continuous_handle_t continuous_adc_handle = NULL;

// --- PROTOTYPY FUNKCJI ---
static void bt_serwer_task(void *arg);
static void ble_app_on_sync();
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int8_t get_set_relay_status(relay_action_t action);


static void adcTask(void * arg);
static void touch_polling_task(void *pvParameters);



// Funkcja zapisu - wywoływana przez task zmieniający dane
esp_err_t settings_update(const app_settings_t *new_cfg) {
    esp_err_t err = save_settings_to_nvs(new_cfg);
    if (err == ESP_OK) {
        // Rozsyła sygnał do wszystkich zainteresowanych tasków
        xEventGroupSetBits(s_settings_event_group, SETTINGS_CHANGED_BIT);
    }
    return err;
}

// Funkcja sprawdzająca dla innych tasków
bool settings_has_changed(void) {
    EventBits_t bits = xEventGroupWaitBits(s_settings_event_group, SETTINGS_CHANGED_BIT, pdTRUE, pdFALSE, 0);
    return (bits & SETTINGS_CHANGED_BIT);
}
// Funkcja konfigurująca i inicjalizująca tryb ciągły
void init_adc_continuous(void)
{
    // 1. Inicjalizacja uchwytu (przydział pamięci i buforów DMA)
    adc_continuous_handle_cfg_t alloc_cfg = {
        .max_store_buf_size = ADC_MAX_STORE_BUF_SIZE,
        .conv_frame_size = ADC_CONV_FRAME_SIZE,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&alloc_cfg, &continuous_adc_handle));

    // 2. Konfiguracja "wzorca" (pattern) - czyli listy kanałów do odczytu
    // My mamy tylko jeden kanał, ale układ pozwala na sekwencyjne czytanie wielu pinów
    adc_digi_pattern_config_t adc_pattern[1] = {0};
    adc_pattern[0].atten = ADC_ATTEN_DB_12;
    adc_pattern[0].channel = CURR_ADC_CHANNEL;
    adc_pattern[0].unit = CURR_ADC_UNIT;
    adc_pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;// 12-bit dla ESP32-S3

    // 3. Główna konfiguracja trybu ciągłego
    adc_continuous_config_t dig_cfg = {
        .pattern_num = 1,
        .adc_pattern = adc_pattern,
       // .sample_freq_hz = 20000, // Częstotliwość próbkowania: 20 kHz
       .sample_freq_hz = ADC_SAMPLE_FREQ_HZ, 
       .conv_mode = ADC_CONV_SINGLE_UNIT_1, // Używamy tylko ADC1
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2, // ESP32-S3 używa formatu danych TYPE2
    };

    ESP_ERROR_CHECK(adc_continuous_config(continuous_adc_handle, &dig_cfg));
}


void broadcast_adc_float(float value) {
    

    
    if (g_active_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
       struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, sizeof(float));
        if (om != NULL) {
            // Wysyła powiadomienie do klienta o uchwycie g_active_conn_handle

              ble_gatts_notify_custom(g_active_conn_handle, current_chr_handle, om);
        }
    }
}

//     bool sw = 0;

void broadcast_relay_status(uint8_t sw)
{
    if (g_active_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
      // struct os_mbuf *om = ble_hs_mbuf_from_flat(&value, sizeof(float));
            struct os_mbuf *om;
           om = ble_hs_mbuf_from_flat(relay_str[sw],strlen(relay_str[sw]));
      
      if (om != NULL) {
            // Wysyła powiadomienie do klienta o uchwycie g_active_conn_handle
          // ble_gatts_notify_custom(g_active_conn_handle, current_chr_handle, om);
              ble_gatts_notify_custom(g_active_conn_handle, relay_chr_handle, om);
        }
    }
}




static void vPwrOnOffTimerCallback(TimerHandle_t xTimer)
{
        if( xLogicTaskHandle != NULL )
    {
        /* xTaskNotifyGive budzi Task zdejmując blokadę */
       // xTaskNotifyGive( xLogicTaskHandle );
        xTaskNotify(xLogicTaskHandle, BIT_TIMER, eSetBits);
    }
}


static void vLogicTask( void *pvParameters )
{
    uint32_t ulNotifiedValue;
    float xAdcReceive = 0.1f;
  //  MyDataStruct_t xReceivedData;

    int8_t sw_last = 0;


      app_settings_t local_cfg;

       load_settings_from_nvs(&local_cfg);
       uint16_t tmr = local_cfg.timer;


    while(1)
    {
        // Czekamy na dowolny bit. 
        // 0x00 - nie czyścimy bitów przy wejściu
        // 0xFFFFFFFF - czyścimy wszystkie bity przy wyjściu (resetujemy flagi)
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &ulNotifiedValue, portMAX_DELAY) == pdTRUE) 
        {
            
            
                   if (settings_has_changed()) {
            load_settings_from_nvs(&local_cfg);
        }
            
            
            
            // 1. Najpierw sprawdź dane (często mają wyższy priorytet niż timer)
            if (ulNotifiedValue & BIT_QUEUE) 
            {
                if(xQueueReceive(xQueueAdcHandle, &xAdcReceive, 0) == pdPASS)
                {
                    broadcast_adc_float(xAdcReceive);
                }
            }

            // 2. Potem sprawdź, czy minęła sekunda
            if (ulNotifiedValue & BIT_TIMER) 
            {
                int8_t rl = get_set_relay_status(RL_GET);
                if(sw_last != rl)
                {
                     sw_last = rl;
                     
                    if( RL_ON == rl) // zalaczenie -> reset timera 
                    {
                        tmr = local_cfg.timer;

                    }
                }

                if( RL_ON == rl )
                {
                    
                    if(xAdcReceive >= local_cfg.adcThreshold)
                    {
                        tmr = local_cfg.timer;//tmrTime;
                    }else{ //if(xAdcReceive < adcThreshold){
                        if(tmr > 0) tmr-=1;
                            if(0 == tmr)
                                {
                                get_set_relay_status(RL_OFF);
                                broadcast_relay_status(RL_OFF);
                                }
                        


                    }
                }

            //    broadcast_timer(tmr);

            }
        }
    }
}


void app_main(void)
{
  
  esp_err_t ret;

  ret = nvs_flash_init();
  if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  
  ESP_ERROR_CHECK(ret);



    // 2. Odczyt ustawień

app_settings_t app_settings;
ret = load_settings_from_nvs(&app_settings);

// Sprawdź najpierw błąd odczytu, a dopiero potem wersję FW
if (ret != ESP_OK || app_settings.fw != fw) {
    
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Brak ustawień.");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Błąd NVS/Rozmiaru.");
    } else {
        ESP_LOGW(TAG, "Nowa wersja FW (%d-> %d). Resetowanie...", app_settings.fw, fw);
    }

    app_settings = (app_settings_t){
        .fw = fw,
        .adcThreshold = 0.5f,
        .mode = 0,
        .numCycles = 0,
        .timer = 60 // minuta
    };
    save_settings_to_nvs(&app_settings);
} else {
    ESP_LOGI(TAG, "Wczytano FW: %d", app_settings.fw);
}

 s_settings_event_group = xEventGroupCreate();


    printf("Hello world!\n");
    
        gpio_set_direction(RELAY,GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY,0);

    xQueueAdcHandle = xQueueCreate(1,sizeof(float));
    
    xTaskCreate(bt_serwer_task,"bt_srv_task",4096,NULL,10,NULL);
    xTaskCreate(adcTask,"ADC",4096,NULL,2,NULL);
    xTaskCreate(vLogicTask,"Logic",4096,NULL,2,&xLogicTaskHandle);
    xTaskCreate(&touch_polling_task, "touch_pad_read_task", 4096, NULL, 5, NULL);
    xPwrOnOffTimer = xTimerCreate("Timer_sek",pdMS_TO_TICKS(1000),pdTRUE,NULL,vPwrOnOffTimerCallback);
    xTimerStart(xPwrOnOffTimer,0);
}  



/*********************************** NAJNOWSZA  ***************** */

static bool adc_update_offset_estimate(
    uint32_t count,
    uint64_t sum,
    uint64_t sum_sq,
    float *offset,
    double *noise_power)
{
    if (count != RMS_WINDOW)
    {
        return false;
    }

    const double n = (double)count;
    const double mean = (double)sum / n;
    const double mean_sq = (double)sum_sq / n;

    // Moc szumu = wariancja próbek przy zerowym prądzie.
    double variance = mean_sq - (mean * mean);

    // Zabezpieczenie przed niewielkim ujemnym błędem numerycznym.
    if (variance < 0.0)
    {
        variance = 0.0;
    }

    *offset = (float)mean;
    *noise_power = variance;

    return true;
}



static void adcTask(void *arg)
{
    (void)arg;

    init_adc_continuous();
    ESP_ERROR_CHECK(adc_continuous_start(continuous_adc_handle));

    // Bufor DMA przekazywany do adc_continuous_read().
    static uint8_t result_buf[ADC_CONV_FRAME_SIZE];

    // Bufor kołowy offsetu: 2000 próbek = 100 ms przy 20 kHz.
    static uint16_t offset_buf[RMS_WINDOW];

    uint32_t offset_write = 0;
    uint32_t offset_count = 0;

    // Sumy próbek używane do szybkiego obliczania średniej
    // i wariancji bez ponownego sumowania całego bufora.
    uint64_t offset_sum = 0;
    uint64_t offset_sum_sq = 0;

    // Ostatni poprawny offset oraz oszacowana moc szumu.
    float dynamic_offset = 0.0f;
    double offset_noise_sq = 0.0;
    bool offset_valid = false;

    // Akumulator RMS.
    double sum_sq = 0.0;
    uint32_t total_samples = 0;
    float rms_filtered = 0.0f;

    int prev_relay = -1;
    uint32_t settle_samples_remaining = 0;

    TickType_t last_offset_broadcast = xTaskGetTickCount();
    TickType_t broadcast_interval =
        pdMS_TO_TICKS(OFFSET_BROADCAST_INTERVAL_MS);

    if (broadcast_interval == 0)
    {
        broadcast_interval = 1;
    }

    while (1)
    {
        uint32_t ret_num = 0;

        esp_err_t ret = adc_continuous_read(
            continuous_adc_handle,
            result_buf,
            sizeof(result_buf),
            &ret_num,
            pdMS_TO_TICKS(100)
        );

        if (ret == ESP_ERR_TIMEOUT)
        {
            continue;
        }

        if (ret != ESP_OK)
        {
            ESP_LOGW(
                "ADC",
                "adc_continuous_read: %s",
                esp_err_to_name(ret)
            );

            // Po utracie danych bieżące okno RMS nie jest ciągłe.
            sum_sq = 0.0;
            total_samples = 0;
            rms_filtered = 0.0f;

            // Odrzucamy nieciągłe okno kalibracyjne,
            // zachowując ostatnią poprawną kalibrację.
            offset_write = 0;
            offset_count = 0;
            offset_sum = 0;
            offset_sum_sq = 0;

            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (ret_num == 0 ||
            ret_num > sizeof(result_buf) ||
            (ret_num % SOC_ADC_DIGI_RESULT_BYTES) != 0)
        {
            ESP_LOGW("ADC", "Nieprawidlowy rozmiar ramki ADC");

            sum_sq = 0.0;
            total_samples = 0;
            rms_filtered = 0.0f;

            offset_write = 0;
            offset_count = 0;
            offset_sum = 0;
            offset_sum_sq = 0;

            continue;
        }

        // -----------------------------------------------------
        // 1. ODCZYT STANU PRZEKAŹNIKA
        // -----------------------------------------------------

        int relay = get_set_relay_status(RL_GET);

        if (relay != RL_ON && relay != RL_OFF)
        {
            ESP_LOGW("ADC", "Nieprawidlowy stan przekaznika");
            continue;
        }

        // -----------------------------------------------------
        // 2. OBSŁUGA ZMIANY STANU
        // -----------------------------------------------------

        if (relay != prev_relay)
        {
            // Przy przejściu RL_OFF -> RL_ON wykorzystujemy
            // ostatnie pełne okno zebrane przy zerowym prądzie.
            if (prev_relay == RL_OFF && relay == RL_ON)
            {
                if (adc_update_offset_estimate(
                    offset_count,
                    offset_sum,
                    offset_sum_sq,
                    &dynamic_offset,
                    &offset_noise_sq))
                {
                    offset_valid = true;
                }
            }

            prev_relay = relay;

            // Nowe okno RMS musi rozpocząć się po ustaleniu toru.
            sum_sq = 0.0;
            total_samples = 0;
            rms_filtered = 0.0f;

            // Nowe okno offsetu rozpoczynamy dopiero w RL_OFF.
            offset_write = 0;
            offset_count = 0;
            offset_sum = 0;
            offset_sum_sq = 0;

            settle_samples_remaining = RELAY_SETTLE_SAMPLES;

            // Odrzucamy całą ramkę, w której wykryto przełączenie.
            continue;
        }

        // Zapis stanu kalibracji PRZED przetwarzaniem ramki.
        // Może być potrzebny, jeśli przekaźnik zmieni stan
        // w trakcie przetwarzania tej ramki.
        const uint32_t offset_count_before_frame = offset_count;
        const uint64_t offset_sum_before_frame = offset_sum;
        const uint64_t offset_sum_sq_before_frame = offset_sum_sq;

        const uint32_t count =
            ret_num / SOC_ADC_DIGI_RESULT_BYTES;

        // -----------------------------------------------------
        // 3. PRZETWARZANIE PRÓBEK
        // -----------------------------------------------------

        for (uint32_t i = 0; i < count; i++)
        {
            adc_digi_output_data_t *p =
                (adc_digi_output_data_t *)(
                    &result_buf[i * SOC_ADC_DIGI_RESULT_BYTES]
                );

            const uint16_t raw = (uint16_t)p->type2.data;

            // Odrzucamy próbki z okresu przejściowego.
            if (settle_samples_remaining > 0)
            {
                settle_samples_remaining--;
                continue;
            }

            // Pomijamy nieoczekiwany kanał ADC.
            if (p->type2.channel != CURR_ADC_CHANNEL)
            {
                continue;
            }

            // -------------------------------------------------
            // RL_OFF: BUFOR KOŁOWY OFFSETU I SZUMU
            // -------------------------------------------------

            if (relay == RL_OFF)
            {
                if (offset_count == RMS_WINDOW)
                {
                    // Usuwamy najstarszą próbkę z sum.
                    const uint16_t old = offset_buf[offset_write];

                    offset_sum -= (uint64_t)old;
                    offset_sum_sq -= (uint64_t)old * (uint64_t)old;
                }
                else
                {
                    offset_count++;
                }

                // Wstawiamy najnowszą próbkę.
                offset_buf[offset_write] = raw;

                offset_sum += (uint64_t)raw;
                offset_sum_sq += (uint64_t)raw * (uint64_t)raw;

                offset_write++;

                if (offset_write >= RMS_WINDOW)
                {
                    offset_write = 0;
                }
            }

            // -------------------------------------------------
            // RL_ON: TRUE RMS AC+DC Z KOMPENSACJĄ SZUMU
            // -------------------------------------------------

            else if (relay == RL_ON && offset_valid)
            {
                const double val =
                    (double)raw - (double)dynamic_offset;

                sum_sq += val * val;
                total_samples++;

                if (total_samples >= RMS_WINDOW)
                {
                    // Średnia kwadratów sygnału, łącznie z szumem.
                    const double measured_sq =
                        sum_sq / (double)total_samples;

                    // Odejmujemy estymowaną moc szumu z RL_OFF.
                    // Nie odejmujemy średniej sygnału pomiarowego,
                    // dzięki czemu zachowujemy składową DC.
                    double corrected_sq =
                        measured_sq - offset_noise_sq;

                    if (corrected_sq < 0.0)
                    {
                        corrected_sq = 0.0;
                    }

                    float rms = sqrtf((float)corrected_sq);

                    // Próg w jednostkach kodów ADC.
                    if (rms < DEADBAND)
                    {
                        rms = 0.0f;
                    }

                    // Zachowujemy dotychczasowe wygładzanie wyniku.
                    rms_filtered =
                        (1.0f - RMS_FILTER_ALPHA) * rms_filtered
                        + RMS_FILTER_ALPHA * rms;

                    sum_sq = 0.0;
                    total_samples = 0;
                }
            }
        }

        // -----------------------------------------------------
        // 4. KONTROLA ZMIANY PRZEKAŹNIKA PODCZAS RAMKI
        // -----------------------------------------------------

        const int relay_after = get_set_relay_status(RL_GET);

        if (relay_after != relay)
        {
            // Jeżeli przełączenie RL_OFF -> RL_ON nastąpiło
            // podczas przetwarzania ramki, do kalibracji używamy
            // wyłącznie statystyk sprzed rozpoczęcia tej ramki.
            if (relay == RL_OFF && relay_after == RL_ON)
            {
                if (adc_update_offset_estimate(
                    offset_count_before_frame,
                    offset_sum_before_frame,
                    offset_sum_sq_before_frame,
                    &dynamic_offset,
                    &offset_noise_sq))
            {
             offset_valid = true;
            }
            }

            prev_relay = relay_after;

            sum_sq = 0.0;
            total_samples = 0;
            rms_filtered = 0.0f;

            offset_write = 0;
            offset_count = 0;
            offset_sum = 0;
            offset_sum_sq = 0;

            settle_samples_remaining = RELAY_SETTLE_SAMPLES;

            continue;
        }

        // -----------------------------------------------------
        // 5. PUBLIKOWANIE WYNIKU
        // -----------------------------------------------------

        const TickType_t now = xTaskGetTickCount();

        if ((TickType_t)(now - last_offset_broadcast) >=
            broadcast_interval)
        {
            float final_val = 0.0f;

            // Ponownie sprawdzamy stan przed opublikowaniem wyniku.
            const int relay_for_publish =
                get_set_relay_status(RL_GET);

            if (relay == RL_ON &&
                relay_for_publish == RL_ON &&
                settle_samples_remaining == 0 &&
                offset_valid)
            {
                final_val = rms_filtered * CURRENT_SCALE;
            }

            xQueueOverwrite(xQueueAdcHandle, &final_val);

            xTaskNotify(
                xLogicTaskHandle,
                BIT_QUEUE,
                eSetBits
            );

            last_offset_broadcast = now;
        }

        // Nie dodajemy vTaskDelay(1) po każdej ramce:
        // adc_continuous_read() sam oczekuje na dane,
        // a dodatkowe opóźnienie mogłoby powodować narastanie
        // zaległości w buforze ADC.
    }
}

/**************** END       *********** */

void host_task(void *param)
{
    nimble_port_run();
}




void bt_serwer_task(void * arg) {
    nimble_port_init();
    
    ble_svc_gap_device_name_set("Monitor poboru prądu");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = 0;



    ble_uuid_init_all();
    // 2. Rejestracja usług
    ble_gatts_count_cfg(gatt_svcs);
   // ble_gatts_add_svcs(gatt_svcs);
         rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE("BLE", "Błąd dodawania usług; rc=%d", rc);
    }
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    
    // 3. Start stosu
    nimble_port_freertos_init(host_task);
    
    vTaskDelete(NULL); 
}   
   




static void ble_app_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields scan_rsp_fields;
    int rc;

    // 1. Sprawdźmy, czy advertising już przypadkiem nie działa
    // (Opcjonalnie można tu dodać flagę pomocniczą, ale NimBLE sam nam powie rc=2)

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &uuid_map[UUID_CURRENT_SRV].gatt.u128;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Błąd pól adv; rc=%d", rc);
        return;
    }

    memset(&scan_rsp_fields, 0, sizeof(scan_rsp_fields));
    const char *device_name = ble_svc_gap_device_name();
    if (device_name) {
        scan_rsp_fields.name = (uint8_t *)device_name;
        scan_rsp_fields.name_len = strlen(device_name);
        scan_rsp_fields.name_is_complete = 1;
        ble_gap_adv_rsp_set_fields(&scan_rsp_fields);
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    
    if (rc == 0) {
        ESP_LOGI(TAG, "Advertising wystartował.");
    } else if (rc == BLE_HS_EALREADY) {
        // Ignorujemy ten błąd - oznacza, że urządzenie już się rozgłasza
        ESP_LOGW(TAG, "Advertising był już uruchomiony (rc=2).");
    } else {
        ESP_LOGE(TAG, "Błąd startu advertisingu; rc=%d", rc);
    }
}


   


// --- Implementacje Funkcji (Callbacków) ---

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI("GAP", "BLE GAP EVENT CONNECT %s", event->connect.status == 0 ? "OK!" : "FAILED!");
            if (event->connect.status == 0) {
                g_active_conn_handle = event->connect.conn_handle;
            }
            ble_app_advertise();
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI("GAP", "BLE GAP EVENT DISCONNECTED");
            g_active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (event->disconnect.conn.conn_handle < MAX_BLE_CONNECTIONS) {
                g_notify_states[event->disconnect.conn.conn_handle] = false;
            }
            ble_app_advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ble_app_advertise();
            break;
        default:
            break;
    }
    return 0;
}




void ble_app_on_sync(void)
{

    ble_hs_id_infer_auto(0, &ble_addr_type);
   /// ble_gatts_start(); // To teraz przejdzie, bo UUID są stałe w pamięci
    ble_app_advertise();
}

int gatt_svr_dsc_access_name(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{

    int rc = os_mbuf_append(ctxt->om, (char *)arg, strlen((char * )arg));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}





int gatt_svr_relay_status_rwn(uint16_t conn_handle,
                              uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt,
                              void *arg)
{
   // const char *state;

    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_READ_CHR:

    //    state = sw ? "ON" : "OFF";
      uint8_t sw = get_set_relay_status(-1);
    //    os_mbuf_append(ctxt->om, state, strlen(state));
    os_mbuf_append(ctxt -> om,relay_str[sw],strlen(relay_str[sw]));  
    return 0;


    case BLE_GATT_ACCESS_OP_WRITE_CHR: {

        int ln = ctxt->om->om_len;
        char buf[4] = {0};

        if (ln > 0 && ln <= 3) {
           // uint8_t sw  = 0;    
            memcpy(buf, ctxt->om->om_data, ln);

            if (strcmp(buf, relay_str[1]) == 0) {
              get_set_relay_status(1);
            }
            else if (strcmp(buf, relay_str[0]) == 0) {
               get_set_relay_status(0);
            }

           ble_gatts_chr_updated(attr_handle); // notyfikacja zwrotna
        }

        return 0;
    }

    default:
        return 0;
    }
}           

int gatt_svr_saved_param_rw(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // 'arg' to wskaźnik do &g_app_settings przekazany w tablicy GATT
    app_settings_t settings;
    load_settings_from_nvs(&settings);

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            // Wysyłamy całą strukturę jako paczkę bajtów
         //   printf("Odczyt\n");
         ESP_LOGE("BLE", "ODCZYT");  
         return os_mbuf_append(ctxt->om, &settings, sizeof(app_settings_t));

        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            // Odbieramy dane od klienta (Web Bluetooth)
            ESP_LOGE("BLE", "Zapis charakterystyki");
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            
            // Walidacja: czy klient przysłał poprawną liczbę bajtów?
            if (len != sizeof(app_settings_t)) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            // Kopiujemy dane prosto do struktury w pamięci
            int rc = ble_hs_mbuf_to_flat(ctxt->om, &settings, sizeof(app_settings_t), NULL);
            if (rc == 0) {
                // Po udanym zapisie przez BT, od razu utrwalamy w NVS
                //save_settings_to_nvs(settings);
                //settings_notify_changed(); 
                
                settings_update(&settings); 
                
                ESP_LOGI("BLE", "Ustawienia zaktualizowane i zapisane w NVS");
                
                
                // Powiadamiamy stos NimBLE, że wartość pod tym uchwytem się zmieniła.
                // NimBLE automatycznie sprawdzi, kto zasubskrybował notyfikacje (CCCD)
                // i roześle je do WSZYSTKICH uprawnionych klientów.
                ble_gatts_chr_updated(attr_handle);
            }
            return rc;

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}




int gatt_svr_current_status_rwn(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
return ESP_OK;
}

int gatt_svr_timer_status_rwn(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return ESP_OK;
}





/**
 * @brief Pobiera, ustawia lub przełącza stan przekaźnika.
 *
 * Funkcja zarządza stanem przekaźnika, przechowując jego aktualny 
 * stan w zmiennej statycznej. W przypadku żądania zmiany (ON, OFF lub TOGGLE), 
 * aktualizuje również fizyczny stan wyjścia GPIO.
 *
 * @param action Akcja do wykonania (typ relay_action_t):
 *               - RL_GET:    Zwraca bieżący stan bez jego zmiany.
 *               - RL_OFF:    Wyłącza przekaźnik.
 *               - RL_ON:     Włącza przekaźnik.
 *               - RL_TOGGLE: Zmienia stan na przeciwny.
 *
 * @return int8_t Aktualny stan przekaźnika po wykonaniu operacji (0 lub 1). 
 *                W przypadku nieprawidłowej akcji zwraca -1.
 */
static int8_t get_set_relay_status(relay_action_t action)
{
    static int8_t rstate = 0;
    
    // Walidacja zakresu enuma
    if(action < RL_GET || action > RL_TOGGLE) return -1; //err

    // --- SEKCJA KRYTYCZNA ---
    portENTER_CRITICAL(&relay_mux);
    
    if (action != RL_GET) {
        if (action == RL_TOGGLE) {
            rstate = !rstate; // Przełącz stan
        } else {
            rstate = (int8_t)action; // Ustaw RL_ON lub RL_OFF
        }
        gpio_set_level(RELAY, rstate);
    }
    
    int8_t current_state = rstate; 
    portEXIT_CRITICAL(&relay_mux);
    
    return current_state;
}




void touch_polling_task(void *pvParameters) {

#if USE_TPAD == 1
    touch_pad_init();
    touch_pad_config(TP_BTN);

    /* 1. Denoise setting z Twojego przykładu */
    touch_pad_denoise_t denoise = {
        .grade = TOUCH_PAD_DENOISE_BIT4,
        .cap_level = TOUCH_PAD_DENOISE_CAP_L4,
    };
    touch_pad_denoise_set_config(&denoise);
    touch_pad_denoise_enable();
    ESP_LOGI("TP", "Denoise function init");


   
       touch_filter_config_t filter_cfg = {
        .mode = TOUCH_PAD_FILTER_IIR_16,        // Silne filtrowanie sygnału szybkiego (Raw)
        .debounce_cnt = 3,                      // Sprzętowy debounce (bez zmian)
        .noise_thr = 0,                         // Minimalny próg szumu dla filtracji tła
        .jitter_step = 4,                       // Wymagany krok dla trybu jitter
        .smh_lvl = TOUCH_PAD_SMOOTH_IIR_4,      // ZWIĘKSZONO: Wyjściowe wygładzanie sygnału Raw
    };
    ESP_ERROR_CHECK(touch_pad_filter_set_config(&filter_cfg));
    ESP_ERROR_CHECK(touch_pad_filter_enable());
   



   
    ESP_LOGI("TP", "Hardware filter enabled");

    /* 3. Włączenie taktowania i start automatu pomiarowego FSM */
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_fsm_start();
    
   

    /* Odczekaj na zapełnienie buforów sprzętowych filtra */
    vTaskDelay(150 / portTICK_PERIOD_MS);

  //  uint32_t touch_raw_value = 0;
  
       uint32_t raw = 0;
    uint32_t bench = 0;
     bool is_pressed = false;
#else
    gpio_set_direction(SW_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SW_BTN, GPIO_PULLUP_ONLY);
    uint8_t task_timer = 0;
    int8_t last_state = -1;

#endif

ESP_LOGI("TP", "Touchpad filter init");

    while (1) {
#if USE_TPAD == 1
        if (touch_pad_read_raw_data(TP_BTN, &raw) == ESP_OK &&
            touch_pad_read_benchmark(TP_BTN, &bench) == ESP_OK) {
            
            // 1. Obliczamy próg dynamicznie (Benchmark + X%)
            // Używamy 64-bitów przy mnożeniu, żeby uniknąć overflow, choć przy 32-bit też powinno być OK
            uint32_t threshold = bench + (bench * TOUCH_THRESHOLD_PER_MIL) / 1000;
            
            // 2. Obliczamy wartość powrotu (Threshold - Histereza %)
            uint32_t release_level = threshold - (bench * TOUCH_HYST_PER_MIL) / 1000;

            if (!is_pressed) {
                // Detekcja naciśnięcia
                if (raw > threshold) {
                    is_pressed = true;
                    ESP_LOGI("TP", "DOTYK! Raw: %lu | Bench: %lu | Próg: %lu", raw, bench, threshold);
                    
                    // Akcja
                 //  get_set_relay_status(RL_TOGGLE);
                 //  broadcast_relay_status(get_set_relay_status(RL_GET));
                    broadcast_relay_status(get_set_relay_status(RL_TOGGLE));
                }
            } else {
                // Detekcja puszczenia (z uwzględnieniem procentowej histerezy)
                if (raw < release_level) {
                    is_pressed = false;
                    ESP_LOGW("TP", "ZWOLNIONO");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_SAMPLE_PERIOD_MS));
#else
        if (0 == gpio_get_level(SW_BTN) && task_timer == 0) {
            // klik

            last_state = get_set_relay_status(RL_GET);

        }

        if (0 == gpio_get_level(SW_BTN) && last_state != -1) {
                task_timer +=1;
                    if (3 == task_timer && last_state == RL_OFF) {
                        get_set_relay_status(RL_ON);
                        broadcast_relay_status(get_set_relay_status(RL_ON));
                        last_state = -1;
                    }else if (9 == task_timer && last_state == RL_ON) {
                        get_set_relay_status(RL_OFF);
                        broadcast_relay_status(get_set_relay_status(RL_OFF));
                        last_state = -1;
            }
        }else if (1 == gpio_get_level(SW_BTN) && task_timer) {
            task_timer = 0;
        }



        vTaskDelay(pdMS_TO_TICKS(50));
#endif
    }
}
