
#pragma once

typedef struct __attribute__((packed)) {
    uint16_t fw;
    float adcThreshold;
    uint8_t mode;
    uint8_t numCycles;
    uint16_t timer;
} app_settings_t;



/**
 * @brief Zapisuje strukturę ustawień aplikacji do pamięci NVS.
 *
 * Funkcja otwiera przestrzeń nazw "storage", zapisuje dane w formacie BLOB 
 * pod kluczem "app_settings" i zatwierdza zmiany (commit).
 *
 * @param[in] settings Wskaźnik do stałej struktury app_settings_t, która ma zostać zapisana.
 * 
 * @return 
 *    - ESP_OK: Jeśli dane zostały pomyślnie zapisane i zatwierdzone.
 *    - ESP_ERR_NVS_NOT_INITIALIZED: Jeśli partycja NVS nie została zainicjalizowana.
 *    - Inne kody błędów esp_err_t (np. brak miejsca, błąd dostępu).
 */
esp_err_t save_settings_to_nvs(const app_settings_t *settings);

/**
 * @brief Odczytuje strukturę ustawień aplikacji z pamięci NVS.
 *
 * Funkcja sprawdza istnienie klucza "app_settings", weryfikuje czy rozmiar danych 
 * zapisanych w NVS jest zgodny z rozmiarem struktury w bieżącym kodzie, 
 * a następnie kopiuje dane do podanego bufora.
 *
 * @param[out] settings Wskaźnik do struktury app_settings_t, do której zostaną wczytane dane.
 * 
 * @return 
 *    - ESP_OK: Jeśli dane zostały pomyślnie odczytane.
 *    - ESP_ERR_NVS_NOT_FOUND: Jeśli klucz nie istnieje (pierwszy start urządzenia).
 *    - ESP_FAIL: Jeśli rozmiar danych w NVS nie zgadza się z rozmiarem struktury.
 *    - Inne kody błędów esp_err_t przy otwieraniu lub odczycie.
 */
esp_err_t load_settings_from_nvs(app_settings_t *settings);

