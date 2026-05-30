# P-Monitor Firmware

ESP32-S3 firmware dla urządzenia Power-monitor, napisane z użyciem **ESP-IDF**.

## Opis

Firmware obsługujący urządzenie do monitorowania poboru prądu podłączonych urządzeń. Po osiągnięciu ustawionego limitu poboru prądu, urządzenie automatycznie wyłącza zasilanie chroniąc przed przeciążeniem. Działa zarówno z prądami **AC jak i DC**.

## Wymagania

- **ESP32-S3** development board
- **ESP-IDF** (najnowsza wersja LTS lub stable)
- **Python 3.7+**
- Git

## Instalacja i Build

### 1. Klonowanie repozytorium

```bash
git clone https://github.com/Lukasz230V/p-monitor-fw.git
cd p-monitor-fw
```

### 2. Konfiguracja ESP-IDF

Upewnij się, że masz zainstalowany ESP-IDF. Jeśli nie:

```bash
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.bat  # Na Windows
# lub
./install.sh   # Na Linux/macOS
source ./export.sh  # Aktywacja
```

### 3. Konfiguracja projektu

```bash
idf.py menuconfig
```

Tutaj możesz dostosować:
- Parametry urządzenia
- Limity prądu
- Konfigurację BLE
- Inne ustawienia

### 4. Build

```bash
idf.py build
```

### 5. Upload do ESP32-S3

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Na Windows zamiast `/dev/ttyUSB0` użyj portu COM (np. `COM3`).

## Struktura projektu

```
.
├── main/              # Kod główny aplikacji
│   ├── main.c         # Punkt wejścia
│   ├── main.h
│   ├── ble_config_uuid.c    # Konfiguracja BLE
│   ├── ble_config_uuid.h
│   ├── nvs_dta.c      # Obsługa NVS (Non-Volatile Storage)
│   └── nvs_dta.h
├── components/        # Komponenty dodatkowe
├── CMakeLists.txt     # Konfiguracja build
├── sdkconfig          # Aktualne ustawienia projektu
└── README.md          # Ten plik
```

## Powiązane projekty

- **[Power-monitor](https://github.com/Lukasz230V/Power-monitor)** - Interfejs webowy do zarządzania urządzeniem (JavaScript/HTML/CSS)

## Komunikacja

Firmware komunikuje się z interfejsem web poprzez:
- **BLE** (Bluetooth Low Energy)
- **REST API** (jeśli dostępne)

## Troubleshooting

### Błąd uploadu

```
error: espressif/esp32s3 is not supported
```

Sprawdź czy w `menuconfig` masz ustawiony prawidłowy target (ESP32-S3).

### Brak portu seryjnego

Na Windows:
- Zainstaluj sterowniki CH340 lub FTDI
- Sprawdź Device Manager

Na Linux:
```bash
ls /dev/ttyUSB*
sudo chmod 666 /dev/ttyUSB0  # Uprawnienia dostępu
```

## Licencja

MIT

## Wsparcie

W razie problemów:
1. Sprawdź dokumentację [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)
2. Stwórz [GitHub Issue](https://github.com/Lukasz230V/p-monitor-fw/issues)
3. Odwiedź [esp32.com forum](https://esp32.com/)
