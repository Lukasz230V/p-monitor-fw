# Analiza Refaktoryzacji Projektu p-monitor-fw

**Data**: 2026-06-01  
**Plik analizy**: main/main.c  
**Commit**: 634de801df7b4f8b56ec500bf2a00d0d678319d5  

---

## 📋 Spis treści

1. [Obecna Struktura](#obecna-struktura)
2. [Proponowany Podział na Pliki](#proponowany-podział-na-pliki)
3. [Private Components do Wyodrębnienia](#private-components-do-wyodrębnienia)
4. [Szczegółowe Opisy Komponentów](#szczegółowe-opisy-komponentów)
5. [Plan Implementacji](#plan-implementacji)

---

## Obecna Struktura

### Problem

Cały kod aplikacji (prawie 840 linii) znajduje się w jednym pliku `main/main.c`:

- **ADC pomiary** - inicjalizacja i przetwarzanie
- **Logika sterowania** - timer, threshold, relay
- **BLE/GATT** - callbacki, notyfikacje
- **Touch pad** - obsługa przycisków dotykowych
- **Ustawienia** - NVS, synchronizacja
- **Relay** - sterowanie GPIO

### Wyzwania

```
✗ Trudna testowałność - brak oddzielenia logiki od interfejsów
✗ Brak możliwości ponownego użytku - kod przywiązany do konkretnego projektu
✗ Utrudniona konserwacja - trudno znaleźć kod odpowiedzialny za daną funkcjonalność
✗ Ryzyko regresu - zmiana w jednym miejscu może wpłynąć na inne
✗ Długie testowanie - brak możliwości jednostkowych testów
```

---

## Proponowany Podział na Pliki

### Krok 1: Podstawowy podział `main.c`

```
main/
├── main.c                    # app_main() + orchestration
├── CMakeLists.txt
└── include/
    ├── config.h              # Wszystkie #define
    ├── relay.h               # Obsługa GPIO relay
    ├── adc.h                 # ADC continuous + RMS filter
    ├── logic.h               # Logika sterowania
    ├── settings.h            # NVS + synchronizacja
    ├── ble_handlers.h        # BLE/GATT callbacki
    └── touch.h               # Touch pad polling
```

### Zawartość plików

#### **config.h** - Centralizacja konfiguracji

```c
#ifndef CONFIG_H
#define CONFIG_H

// Firmware version
#define FW_VERSION 1148

// GPIO/ADC Configuration
#define RELAY               GPIO_NUM_4
#define CURR_ADC_UNIT       ADC_UNIT_1
#define CURR_ADC_CHANNEL    ADC_CHANNEL_4
#define ADC_MAX_STORE_BUF_SIZE 1024
#define ADC_CONV_FRAME_SIZE    1024

// Measurement parameters
#define RMS_WINDOW             2000    // 100ms przy 20kHz
#define DEADBAND               3.0f    // minimalny sygnał
#define OFFSET_FILTER_WEIGHT   0.001f
#define RMS_FILTER_ALPHA       0.2f
#define CURRENT_SCALE          0.0501f
#define OFFSET_BROADCAST_INTERVAL_MS 500

// Event bits
#define BIT_TIMER (1 << 0)
#define BIT_QUEUE (1 << 1)
#define SETTINGS_CHANGED_BIT BIT0

// Touch pad
#define TP_BTN                  TOUCH_PAD_NUM6
#define TOUCH_SAMPLE_PERIOD_MS  50
#define TOUCH_THRESHOLD_PER_MIL 38
#define TOUCH_HYST_PER_MIL      20

// BLE
#define MAX_BLE_CONNECTIONS 3

#endif // CONFIG_H
```

#### **relay.h** - Obsługa przekaźnika

```c
#ifndef RELAY_H
#define RELAY_H

#include <stdint.h>

typedef enum {
    RL_GET    = -1,
    RL_OFF    =  0,
    RL_ON     =  1,
    RL_TOGGLE =  2
} relay_action_t;

void relay_init(void);
int8_t relay_get_set(relay_action_t action);

#endif // RELAY_H
```

**Odpowiedzialność**:
- Inicjalizacja GPIO (RELAY pin)
- Zarządzanie stanem z mutex protection
- Thread-safe operacje ON/OFF/TOGGLE/GET

---

#### **adc.h** - Pomiary ADC i RMS filter

```c
#ifndef ADC_H
#define ADC_H

#include <stdint.h>

void adc_init_continuous(void);
void adc_task(void *arg);
float adc_get_last_rms(void);
float adc_get_dynamic_offset(void);

#endif // ADC_H
```

**Odpowiedzialność**:
- Konfiguracja ADC continuous mode
- Przetwarzanie ramek danych
- Filtrowanie RMS (suma kwadratów + średnia)
- Kalibracja offsetu (IIR filter)
- Broadcast do queue'u co 500ms

**Lokalne zmienne stanu**:
```c
static float dynamic_offset = 0.0f;
static float rms_filtered = 0.0f;
static double sum_sq = 0.0;
static int total_samples = 0;
static int prev_relay = -1;
```

---

#### **logic.h** - Logika sterowania

```c
#ifndef LOGIC_H
#define LOGIC_H

void logic_init_event_group(void);
void logic_task(void *pvParameters);
bool logic_settings_has_changed(void);
esp_err_t logic_settings_notify_changed(void);

#endif // LOGIC_H
```

**Odpowiedzialność**:
- Obsługa notyfikacji z ADC
- Timer-based turn-off logic
- Threshold comparison
- Ustawianie/resetowanie timerów

**Logika działania**:
```
Timer co 1 sekunda:
├── Czy zmienił się stan relay?
│   └── Tak → reset timer
├── Czy relay jest ON?
│   ├── ADC >= threshold? → reset timer
│   └── ADC < threshold? → zmniejsz timer
│       └── timer == 0? → relay OFF
└── Broadcast stanu
```

---

#### **settings.h** - Zarządzanie ustawieniami

```c
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t fw;
    float adcThreshold;
    uint8_t mode;
    uint8_t numCycles;
    uint16_t timer;
} app_settings_t;

esp_err_t settings_init_nvs(void);
esp_err_t settings_load(app_settings_t *settings);
esp_err_t settings_save(const app_settings_t *settings);
esp_err_t settings_update(const app_settings_t *settings);

#endif // SETTINGS_H
```

**Odpowiedzialność**:
- Inicjalizacja NVS flash
- Load/save struktury do NVS
- Walidacja wersji FW
- Event Group do synchronizacji tasków

---

#### **ble_handlers.h** - BLE/GATT callbacki

```c
#ifndef BLE_HANDLERS_H
#define BLE_HANDLERS_H

#include <stdint.h>

extern uint16_t current_chr_handle;
extern uint16_t relay_chr_handle;
extern const char *relay_str[2];

void ble_init(void);
void ble_notify_current(float value);
void ble_notify_relay_status(uint8_t status);
void ble_set_active_conn_handle(uint16_t handle);
uint16_t ble_get_active_conn_handle(void);

// GATT callbacks
int gatt_svr_relay_status_rwn(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
int gatt_svr_saved_param_rw(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);

#endif // BLE_HANDLERS_H
```

**Odpowiedzialność**:
- Inicjalizacja NimBLE stack
- GAP/GATT setup
- Advertising
- Callbacks dla READ/WRITE charakterystyk
- Wysyłanie notyfikacji

---

#### **touch.h** - Touch pad obsługa

```c
#ifndef TOUCH_H
#define TOUCH_H

void touch_init(void);
void touch_polling_task(void *pvParameters);

#endif // TOUCH_H
```

**Odpowiedzialność**:
- Inicjalizacja touch pad hardware
- Denoise i filter configuration
- Threshold + hystereza detekcja
- Callback na press/release

---

## Private Components do Wyodrębnienia

### ⚠️ Różnica między `main/` a `components/`

| Aspekt | main/ | components/ |
|--------|-------|------------|
| **Scope** | Aplikacyjne, project-specific | Reusable, domain-specific |
| **Importy** | Używane w głównej aplikacji | Mogą być używane w wielu projektach |
| **CMakeLists.txt** | `idf_component_register(...)` | `idf_component_register(...)` |
| **Przykład** | `app_main()`, logika biznesowa | Sterowniki, abstrakcje, utility |

### Rekomendowane Private Components

Następujące moduły nadają się na **wielokrotnie reusable komponenty**:

---

### **1. `components/adc_rms_filter/`**

**Cel**: Uniwersalny moduł do pomiaru RMS z kalibracją offsetu

**Przypadki użycia**:
- Liczniki energii (kWh)
- Mierniki mocy
- Systemy monitorowania prądu
- Woltomierze RMS

**Struktura**:
```
components/adc_rms_filter/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── adc_rms_filter.h
└── adc_rms_filter.c
```

**API**:
```c
typedef struct {
    adc_unit_t adc_unit;
    adc_channel_t adc_channel;
    uint32_t sample_freq_hz;
    uint16_t rms_window_samples;
    float offset_filter_weight;
    float rms_filter_alpha;
    float current_scale;
    uint32_t broadcast_interval_ms;
    uint32_t buffer_size;
} adc_rms_config_t;

typedef struct {
    float rms_value;
    float offset;
    uint32_t sample_count;
    int is_ready;
} adc_rms_result_t;

esp_err_t adc_rms_init(const adc_rms_config_t *config);
esp_err_t adc_rms_start(void);
esp_err_t adc_rms_stop(void);
void adc_rms_process_frame(uint8_t *data, uint32_t len, int relay_state);
adc_rms_result_t adc_rms_get_result(void);
void adc_rms_reset_on_relay_change(void);
```

**Zależności**: `esp_adc`, `freertos`

**Porównanie z current implementation**:
```
Before: 95 linii w adcTask()
After:  3-4 linie call API
```

---

### **2. `components/gpio_relay/`**

**Cel**: Thread-safe sterowanie przekaźnikami GPIO

**Przypadki użycia**:
- Sterowanie grzałkami
- Sterowanie pompami
- Kontrola zasilaczy
- Sygnalizatory LED

**Struktura**:
```
components/gpio_relay/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── gpio_relay.h
└── gpio_relay.c
```

**API**:
```c
typedef enum {
    GPIO_RELAY_GET    = -1,
    GPIO_RELAY_OFF    = 0,
    GPIO_RELAY_ON     = 1,
    GPIO_RELAY_TOGGLE = 2,
} gpio_relay_action_t;

typedef struct {
    gpio_num_t pin;
    uint8_t active_high;  // 1 = HIGH to ON, 0 = LOW to ON
    const char *tag;      // dla logowania
} gpio_relay_config_t;

esp_err_t gpio_relay_init(const gpio_relay_config_t *config);
int8_t gpio_relay_control(gpio_relay_action_t action);
int8_t gpio_relay_get_state(void);
esp_err_t gpio_relay_toggle(void);
```

**Zależności**: `driver/gpio`, `freertos`

**Zmiany w main.c**:
```c
// Before: 24 linie
static int8_t get_set_relay_status(relay_action_t action) { ... }

// After:
gpio_relay_control(GPIO_RELAY_ON);
```

---

### **3. `components/touch_button/`**

**Cel**: Generyczna obsługa touchpada z filtrowaniem

**Przypadki użycia**:
- Przyciski dotykowe
- Interfejsy dotykowe
- Czujniki zbliżeniowe (capacitive)

**Struktura**:
```
components/touch_button/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── touch_button.h
└── touch_button.c
```

**API**:
```c
typedef enum {
    TOUCH_EVENT_PRESSED,
    TOUCH_EVENT_RELEASED,
} touch_event_t;

typedef struct {
    touch_pad_t pad;
    uint32_t threshold_per_mil;   // % powyżej benchmarku
    uint32_t hysteresis_per_mil;  // % dla powrotu
    uint16_t sample_period_ms;
    uint16_t denoise_grade;
    void (*on_event)(touch_event_t event);
} touch_button_config_t;

esp_err_t touch_button_init(const touch_button_config_t *config);
esp_err_t touch_button_start_task(void);
bool touch_button_is_pressed(void);
```

**Zależności**: `driver/touch_pad`, `freertos`

**Zmiana**:
```c
// Before: 85 linii touch_polling_task()
// After: Jeden callback na event
static void on_touch_event(touch_event_t event) {
    if (event == TOUCH_EVENT_PRESSED) {
        relay_control(GPIO_RELAY_TOGGLE);
    }
}
```

---

### **4. `components/ble_notify/`**

**Cel**: Abstrakcja dla NimBLE notyfikacji

**Przypadki użycia**:
- Dowolny projekt z BLE notyfikacjami
- Wysyłanie danych sensor → client
- Thread-safe buffering

**Struktura**:
```
components/ble_notify/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── ble_notify.h
└── ble_notify.c
```

**API**:
```c
esp_err_t ble_notify_init(void);
esp_err_t ble_notify_set_connection(uint16_t conn_handle);
esp_err_t ble_notify_send_float(uint16_t char_handle, float value);
esp_err_t ble_notify_send_string(uint16_t char_handle, const char *str);
esp_err_t ble_notify_send_raw(uint16_t char_handle, const void *data, size_t len);
uint16_t ble_notify_get_connection(void);
```

**Zależności**: `nimble`

---

### **5. `components/app_settings/`**

**Cel**: NVS abstraction + event-driven synchronizacja

**Przypadki użycia**:
- Prawie każdy projekt z persistentną konfiguracją
- Weryfikacja wersji FW
- Settings synchronizacja między taskami

**Struktura**:
```
components/app_settings/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── app_settings.h
└── app_settings.c
```

**API** (generic):
```c
#define SETTINGS_STRUCT_SIZE 32  // Konfigurowalne

typedef void (*settings_change_callback_t)(void);

esp_err_t app_settings_init(const char *namespace, uint16_t expected_fw_version);
esp_err_t app_settings_load(void *settings_struct, size_t size);
esp_err_t app_settings_save(const void *settings_struct, size_t size);
esp_err_t app_settings_update(const void *settings_struct, size_t size);
bool app_settings_has_changed(void);
esp_err_t app_settings_register_callback(settings_change_callback_t cb);
```

**Zależności**: `nvs_flash`, `freertos`

---

### **6. `components/control_logic/`**

**Cel**: Timer-based control logic (turn-off z threshold'ami)

**Przypadki użycia**:
- Grzałki z auto turn-off
- Pompy z threshold'ami
- Systemy wentylacji z regulacją

**Struktura**:
```
components/control_logic/
├── CMakeLists.txt
├── Kconfig
├── include/
│   └── control_logic.h
└── control_logic.c
```

**API**:
```c
typedef struct {
    float threshold;
    uint16_t timeout_sec;
    void (*on_output_change)(int state);
} control_logic_config_t;

esp_err_t control_logic_init(const control_logic_config_t *config);
void control_logic_update(float sensor_value, int current_output_state);
int control_logic_get_state(void);
void control_logic_reset_timeout(void);
```

**Zależności**: none (pure logic)

---

## Plan Implementacji

### Phase 1: Podstawowy podział main.c (1-2 dni)

✅ **Wydzielić do osobnych plików** (wszystko w `main/`):
1. `relay.h/c` - super proste, niezależne
2. `config.h` - centralizacja makr
3. `adc.h/c` - logika filtrowania
4. `logic.h/c` - timer logic
5. `settings.h/c` - NVS wrapper
6. `ble_handlers.h/c` - BLE callbacki
7. `touch.h/c` - touch polling

**Rezultat**: Clean `main.c` z głównie `app_main()`

---

### Phase 2: Wyrybienie komponentów (2-3 dni)

✅ **Przenieść do `components/`** (w kolejności):

1. **`gpio_relay`** (najniezależniejszy)
   - Brak zależności, niezależne API
   - Łatwo testować

2. **`adc_rms_filter`** (następny niezależny)
   - Zależy od: esp_adc, freertos
   - Czysty kod przetwarzania sygnału

3. **`touch_button`** (niezależny)
   - Zależy od: driver/touch_pad, freertos
   - Generic callback interface

4. **`app_settings`** (fundament)
   - Zależy od: nvs_flash, freertos
   - Używany przez inne komponenty

5. **`ble_notify`** (niski priorytet)
   - Zależy od: nimble
   - Wrapper, nie core logic

6. **`control_logic`** (ostatni)
   - Zależy od: logic z Phase 1

---

### Phase 3: Integracja i testy (1-2 dni)

✅ **Zaktualizować main.c** do używania komponentów
✅ **Testy jednostkowe** dla każdego komponentu
✅ **Integracyjne testy** systemu

---

## Nowa Struktura Projektu

```
p-monitor-fw/
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── main.c                      # app_main() + orchestration
│   └── include/
│       ├── config.h                # Centrale #define
│       ├── relay.h
│       ├── adc.h
│       ├── logic.h
│       ├── settings.h
│       ├── ble_handlers.h
│       └── touch.h
│
├── components/
│   ├── adc_rms_filter/
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── include/
│   │   │   └── adc_rms_filter.h
│   │   ├── adc_rms_filter.c
│   │   └── README.md
│   │
│   ├── gpio_relay/
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── include/
│   │   │   └── gpio_relay.h
│   │   ├── gpio_relay.c
│   │   └── README.md
│   │
│   ├── touch_button/
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── include/
│   │   │   └── touch_button.h
│   │   ├── touch_button.c
│   │   └── README.md
│   │
│   ├── ble_notify/
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── include/
│   │   │   └── ble_notify.h
│   │   ├── ble_notify.c
│   │   └── README.md
│   │
│   ├── app_settings/
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── include/
│   │   │   └── app_settings.h
│   │   ├── app_settings.c
│   │   └── README.md
│   │
│   └── control_logic/
│       ├── CMakeLists.txt
│       ├── Kconfig
│       ├── include/
│       │   └── control_logic.h
│       ├── control_logic.c
│       └── README.md
│
├── CMakeLists.txt
├── REFACTORING_ANALYSIS.md         # TEN PLIK
├── README.md
└── sdkconfig
```

---

## Przykład: Refaktoryzacja main.c po Phase 1+2

### Przed:

```c
void app_main(void) {
    // NVS init + FW check (70+ linii)
    // GPIO setup (3 linie)
    // Queue create (1 linia)
    // 7x xTaskCreate (30+ linii)
    // Timer setup (5 linii)
}

static void adcTask(void * arg) {
    // 95 linii całej logiki filtrowania
}

static void vLogicTask(void *pvParameters) {
    // 80 linii logiki sterowania
}

void touch_polling_task(void *pvParameters) {
    // 85 linii touch'a
}

static int8_t get_set_relay_status(relay_action_t action) {
    // 24 linie relay control
}
```

### Po:

```c
#include "config.h"
#include "gpio_relay.h"
#include "adc_rms_filter.h"
#include "control_logic.h"
#include "app_settings.h"
#include "ble_handlers.h"
#include "touch_button.h"

// Global handles
QueueHandle_t xQueueAdcHandle = NULL;
TaskHandle_t xLogicTaskHandle = NULL;
TimerHandle_t xPwrOnOffTimer = NULL;

static void timer_callback(TimerHandle_t xTimer) {
    if (xLogicTaskHandle != NULL) {
        xTaskNotify(xLogicTaskHandle, BIT_TIMER, eSetBits);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting p-monitor-fw v%d", FW_VERSION);
    
    // Initialize components
    app_settings_init("monitor", FW_VERSION);
    gpio_relay_init((gpio_relay_config_t) {
        .pin = RELAY,
        .active_high = 1,
        .tag = "RELAY"
    });
    
    adc_rms_config_t adc_cfg = {
        .adc_unit = CURR_ADC_UNIT,
        .adc_channel = CURR_ADC_CHANNEL,
        .sample_freq_hz = 20000,
        .rms_window_samples = RMS_WINDOW,
        .offset_filter_weight = OFFSET_FILTER_WEIGHT,
        .rms_filter_alpha = RMS_FILTER_ALPHA,
        .current_scale = CURRENT_SCALE,
        .broadcast_interval_ms = OFFSET_BROADCAST_INTERVAL_MS,
    };
    adc_rms_init(&adc_cfg);
    
    // Setup BLE
    ble_init();
    
    // Setup Touch
    touch_button_config_t touch_cfg = {
        .pad = TP_BTN,
        .threshold_per_mil = TOUCH_THRESHOLD_PER_MIL,
        .hysteresis_per_mil = TOUCH_HYST_PER_MIL,
        .sample_period_ms = TOUCH_SAMPLE_PERIOD_MS,
        .on_event = on_touch_event,
    };
    touch_button_init(&touch_cfg);
    
    // Create queue and tasks
    xQueueAdcHandle = xQueueCreate(1, sizeof(float));
    
    xTaskCreate(adc_rms_task, "adc_rms", 4096, NULL, 2, NULL);
    xTaskCreate(logic_task, "logic", 4096, NULL, 2, &xLogicTaskHandle);
    xTaskCreate(touch_button_task, "touch", 4096, NULL, 5, NULL);
    
    // Create and start timer
    xPwrOnOffTimer = xTimerCreate(
        "timer_1sec",
        pdMS_TO_TICKS(1000),
        pdTRUE,
        NULL,
        timer_callback
    );
    xTimerStart(xPwrOnOffTimer, 0);
    
    ESP_LOGI(TAG, "Initialization complete");
}
```

**Porównanie**:
| | Przed | Po |
|---|-------|-----|
| **Linii w main.c** | 840 | ~80 |
| **Łatwość testów** | 🔴 Niemożliwe | 🟢 Jednostkowe testy |
| **Ponowne użytko** | 🔴 Żaden kod | 🟢 6 komponentów |
| **Czytelność** | 🟡 Niska | 🟢 Wysoka |

---

## Podsumowanie

### ✅ Korzyści Refaktoryzacji

1. **Modularność** - każdy komponent ma jedną odpowiedzialność
2. **Testowałność** - unit testy dla każdego modułu
3. **Reusability** - komponenty mogą być używane w innych projektach
4. **Maintainability** - łatwiej znaleźć i naprawić buggi
5. **Skalowałność** - łatwo dodawać nowe funkcjonalności

### 📊 Potencjał Ponownego Użytku

| Komponent | Reusability | Projekty |
|-----------|-------------|----------|
| `gpio_relay` | 🔴 Bardzo wysoka | Grzałki, pompy, zasilacze, LED |
| `adc_rms_filter` | 🔴 Bardzo wysoka | Liczniki energii, mierniki mocy |
| `touch_button` | 🔴 Bardzo wysoka | Interfejsy dotykowe, przyciski |
| `app_settings` | 🟡 Wysoka | Prawie każdy projekt z config |
| `ble_notify` | 🟡 Wysoka | Dowolny projekt BLE |
| `control_logic` | 🟡 Wysoka | Regulatory, automaty |

### 🎯 Następne Kroki

1. **Przeczytaj tę analizę** i zastanów się nad każdym komponentem
2. **Zaplanuj Phase 1** - podział main.c na pliki
3. **Zaplanuj Phase 2** - wyrybienie komponentów
4. **Zaplanuj Phase 3** - integracja i testy

---

**Drukowanie**: Możesz zapisać ten plik jako PDF dla offline przeglądu

