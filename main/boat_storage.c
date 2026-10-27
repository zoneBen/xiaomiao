#include "boat_storage.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

static const char* TAG = "boat_storage";

#define NVS_NAMESPACE     "boat_cfg"
#define NVS_KEY_LAST_SSID "last_ssid"

bool boat_storage_load_ssid(char* out, size_t len)
{
    if (out == NULL || len == 0) {
        return false;
    }

    out[0] = '\0';

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    bool loaded = (nvs_get_str(handle, NVS_KEY_LAST_SSID, out, &len) == ESP_OK);
    nvs_close(handle);

    if (!loaded) {
        out[0] = '\0';
    }
    return loaded;
}

bool boat_storage_save_ssid(const char* ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS");
        return false;
    }

    esp_err_t err = nvs_set_str(handle, NVS_KEY_LAST_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot save last SSID: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
