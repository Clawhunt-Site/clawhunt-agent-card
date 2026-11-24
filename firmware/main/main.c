// main/main.c -- ClawHunt agent card: bring-up and input wiring.
//
// The card shows one agent. A computer on the USB port feeds it marketplace
// events (see host_link.h); the three keys answer them.
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "card_app.h"
#include "card_model.h"
#include "cloud.h"
#include "host_link.h"
#include "net.h"
#include "sfx.h"

#include "esp_log.h"

static const char *TAG = "main";

// Button callbacks run on the shared esp_timer task: queue and return.
// PRESS rather than CLICK, so grabbing an offer reacts the instant the key goes
// down. Holding a key sends a second event; holding OK opens the settings menu.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev != BSP_BTN_PRESS && ev != BSP_BTN_LONG) return;
    card_app_note_physical_key();
    card_event_t event = { .type = ev == BSP_BTN_LONG ? CARD_EV_KEY_LONG : CARD_EV_KEY };
    switch (btn) {
    case BSP_BTN_UP:   event.key = CARD_KEY_UP; break;
    case BSP_BTN_DOWN: event.key = CARD_KEY_DOWN; break;
    case BSP_BTN_OK:   event.key = CARD_KEY_OK; break;
    default: return;
    }
    card_app_post(&event);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ClawHunt agent card %s", CARD_FW_VERSION);
    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(80);

    // Sound and keys are optional: the card still shows what the computer sends.
    if (sfx_init() != ESP_OK) ESP_LOGW(TAG, "running without sound");
    // Silent unless the holder switched sound on in the card's own menu.
    char sound[4];
    (void)net_store_get("snd", sound, sizeof(sound));
    sfx_set_enabled(sound[0] == '1');

    esp_err_t err = card_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "card app failed to start: %s", esp_err_to_name(err));
        return;
    }
    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "running without keys: %s", esp_err_to_name(err));

    err = host_link_init(card_app_post);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "host link failed to start: %s", esp_err_to_name(err));
        return;
    }
    // The cloud task owns every HTTP request; start it before Wi-Fi can come up.
    err = cloud_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "running without ClawHunt sync: %s", esp_err_to_name(err));
    // Joins the saved Wi-Fi network, if the card was ever given one.
    err = net_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "running without Wi-Fi: %s", esp_err_to_name(err));
    host_link_send_hello();
}
