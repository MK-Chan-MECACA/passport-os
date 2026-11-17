// Phone setup service: the only HTTP server while the hotspot is up; profile editing continues after joining a network.
// The phone joins the device hotspot and opens 192.168.4.1 to configure Wi-Fi, set the profile and upload images.
// Image scaling and JPEG compression happen in the browser; the device streams uploads through a 512B buffer.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the setup service (idempotent). Call StartConfigAp(true) first; the hotspot address is available after it returns.
esp_err_t home_upload_start(void);

// Stop the service and reclaim httpd memory (must be called when leaving upload mode or Home).
void home_upload_stop(void);

bool home_upload_active(void);
bool home_upload_busy(void);
bool home_upload_network_verified(void);
bool home_upload_finished(void);

// "http://192.168.x.x" (valid only while the service is running)
const char *home_upload_url(void);

#ifdef __cplusplus
}
#endif
