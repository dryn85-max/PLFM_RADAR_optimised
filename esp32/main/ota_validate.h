/* OTA image validation: a freshly updated image boots as "pending verify" and is marked valid
 * once Wi-Fi and the HTTP server have been up for 30 s; after 120 s without that the bootloader
 * is told to roll back to the previous image (decision logic: core/ota_check.h). */
#ifndef OTA_VALIDATE_H
#define OTA_VALIDATE_H

/* Call at the end of app_main (after Wi-Fi and the HTTP server were started). Images that are not
 * pending (first USB flash, already valid) are only logged and left alone. */
void ota_validate_start(void);

#endif
