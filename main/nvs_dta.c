

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "nvs_dta.h"


static const char *TAG = "DATA_NVS";




esp_err_t save_settings_to_nvs(const app_settings_t *settings) {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Otwarcie przestrzeni nazw "storage" w trybie odczyt/zapis
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Błąd podczas otwierania NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Zapis struktury jako BLOB
    err = nvs_set_blob(my_handle, "app_settings", settings, sizeof(app_settings_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Błąd podczas zapisu do NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Pomyślnie zapisano ustawienia do NVS.");
    }

    // Zatwierdzenie zmian (bardzo ważne!)
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Błąd podczas commitu NVS: %s", esp_err_to_name(err));
    }

    // Zamknięcie uchwytu
    nvs_close(my_handle);
    return err;
}


esp_err_t load_settings_from_nvs(app_settings_t *settings) {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Otwarcie przestrzeni nazw "storage" w trybie tylko do odczytu
    err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Błąd podczas otwierania NVS (może nie być jeszcze utworzony): %s", esp_err_to_name(err));
        return err;
    }

    size_t required_size = 0;
    // Najpierw pobieramy rozmiar danych przypisanych do klucza (przekazując NULL jako bufor)
    err = nvs_get_blob(my_handle, "app_settings", NULL, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Błąd podczas sprawdzania rozmiaru BLOB: %s", esp_err_to_name(err));
        nvs_close(my_handle);
        return err;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Brak zapisanych ustawień w NVS (klucz nie istnieje).");
        nvs_close(my_handle);
        return err;
    }

    // Sprawdzamy, czy rozmiar w NVS odpowiada rozmiarowi naszej struktury
    if (required_size != sizeof(app_settings_t)) {
        ESP_LOGE(TAG, "Rozmiar danych w NVS (%d) nie zgadza się z rozmiarem struktury (%d)!", required_size, sizeof(app_settings_t));
        nvs_close(my_handle);
        return ESP_FAIL;
    }

    // Właściwy odczyt danych do struktury
    err = nvs_get_blob(my_handle, "app_settings", settings, &required_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Pomyślnie wczytano ustawienia z NVS.");
    }

    nvs_close(my_handle);
    return err;
}