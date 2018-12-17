/*
Copyright (c) 2018 Riskable

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

@file main.c
@author Riskable
@brief Entry point for the ESP32 application.
@see https://github.com/riskable/wifi_broadway_sign
@see https://idyl.io
@see https://github.com/tonyp7/esp32-wifi-manager
@see https://github.com/CalinRadoni/esp32_digitalLEDs
*/

// Standard C stuff
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

// FreeRTOS stuff
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// ESP32 stuff
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/touch_pad.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

// TCP/IP stack stuff
#include "lwip/err.h"
#include "lwip/apps/sntp.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

// Protocols/peripherals/built-in helpers
// #include "cJSON.h" // Turns out I don't need this!
#include "mdns.h"
#include "mqtt_client.h"

// Our own stuff
#include "esp32_rmt_dled.h" // WS2811 control
#include "http_server.h" // Wifi manager
#include "wifi_manager.h" // Wifi manager

#define STACK_SIZE (6*1024)
#define LED_TASK_PRIORITY 10
#define NUM_LEDS 112
#define FLOAT_TO_INT(x) ((x)>=0?(int)((x)+0.5):(int)((x)-0.5))

// Touch pad stuff (for controlling basic on/off of the lights)
#define TOUCH_THRESH_NO_USE   (0)
#define TOUCHPAD_FILTER_TOUCH_PERIOD (10)
#define TOUCH_THRESHOLD 500
#define TOUCH0 0
// For some reason touch sensor 1 doesn't work on my esp32 :shrug:
#define TOUCH2 2
#define TOUCH3 3
// Long press is considered this many ms (10 seconds):
#define LONG_PRESS_THRESHOLD 10000

static const char *TAG = "IOT_SIGN";

// NOTE: I assigned these numbers instead of letting the compiler do it so we
//       can save and restore the current effect reliably...
typedef enum led_effect {
    OFF = 0,
    COLOR = 1,
    RAINBOW = 2,
    ENUMERATE = 3,
    MARQUEE = 4,
    TWINKLE = 5,
    RAINBOW_MARQUEE = 6
} led_effect;


TaskHandle_t led_task_handle = NULL; // Used for LED tasks
rmt_pixel_strip_t rps; // LED Stuff
pixel_strip_t strip; // LED Stuff
// These are just the defaults.  You can change them via the MQTT_CONFIG_TOPIC
int strip1_gpio = 16; // NOTE: Using GPIO 16 (aka P16). 0 is the RMT peripheral "channel"
uint8_t led_brightness = 64;
bool led_brightness_up = true; // Used by TOUCH2 to cycle through brightness up/down
uint8_t effect_speed_delay = 100;
uint8_t twinkly = 25; // Controls how much twinkle the led_twinkle() effect will...  Twinkle
    // Lower values == less likely to twinkle any given LED
char led_palette[] = "#ff8200"; // Default to a yellowish orange color (like a real marquee)
enum led_effect current_effect = RAINBOW;
enum led_effect prev_effect = OFF;

struct tm last_press = { 0 };// Used to detect a long press of the power button (e.g. to reset wifi)

const char broadway_nvs_namespace[] = "iot_lights";

static TaskHandle_t task_http_server = NULL;
static TaskHandle_t task_wifi_manager = NULL;
static TaskHandle_t task_time_manager = NULL;

const int WIFI_CONNECTED_BIT = BIT0; // Same as what WIFI_MANAGER uses

// So we don't need a main.h:
static void obtain_time(void);
static void initialize_sntp(void);

/**
* @brief Structure to access various LED strip items from an MQTT context
*
*/
typedef struct {
    rmt_pixel_strip_t *rps;       /*!< LED strip's main struct */
    TaskHandle_t *effectHandle;   /*!< Handle to kill any running effects) */
} effect_context_t;

/* Variable holding number of times ESP32 restarted since first boot.
* It is placed into RTC memory using RTC_DATA_ATTR and
* maintains its value when ESP32 wakes from deep sleep.
*/
// RTC_DATA_ATTR static int boot_count = 0;

void add_mdns_services() {
    //add our services
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    //NOTE: services must be added before their properties can be set
    //use custom instance for the web server
    mdns_service_instance_name_set("_http", "_tcp", "Broadway Thing Web Server");

    mdns_txt_item_t serviceTxtData[2] = {
        {"board","esp32"},
        {"broadway","sign"}
    };
    //set txt data for service (will free and replace current data)
    mdns_service_txt_set("_http", "_tcp", serviceTxtData, 2);
}

void start_mdns_service() {
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) {
        printf("MDNS Init failed: %d\n", err);
        return;
    }

    //set hostname
    mdns_hostname_set(CONFIG_HOSTNAME);
    //set default instance
    mdns_instance_name_set(CONFIG_MDNS_INSTANCE_NAME);
    // Set it up:
    add_mdns_services();
}

int random(int min, int max) {
    int out = min + esp_random() / (RAND_MAX / (max - min + 1) + 1);
    return out;
}

char* substring(const char* str, size_t begin, size_t len) {
    if (str == 0 || strlen(str) == 0 || strlen(str) < begin || strlen(str) < (begin+len))
        return 0;

    return strndup(str + begin, len);
}

void delay_ms(uint32_t ms) {
    if (ms == 0) return;
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

void rotate_pixels(pixel_t *pixels, int length) {
    /* Store last element of pixels */
    pixel_t last = pixels[length - 1];

    for (int i=length-1; i>0; i--) {
        /* Move each pixel to its right */
        pixels[i] = pixels[i - 1];
    }

    /* Copy last element of array to first */
    pixels[0] = last;
}
void rotate_pixels_reverse(pixel_t *pixels, int length) {
    /* Store last element of pixels */
    pixel_t first = pixels[0];

    for (int i=0; i<length-1; i++) {
        /* Move each pixel to its left */
        pixels[i] = pixels[i + 1];
    }

    /* Copy first element of array to last */
    pixels[length-1] = first;
}

// Initialize TOUCH0
static void tp_init() {
    ESP_LOGI(TAG, "Initializing touch sensors");
    touch_pad_config(TOUCH0, TOUCH_THRESH_NO_USE);
    touch_pad_config(TOUCH2, TOUCH_THRESH_NO_USE);
    touch_pad_config(TOUCH3, TOUCH_THRESH_NO_USE);
}

static void initialize_leds(rmt_pixel_strip_t *rps, pixel_strip_t *strip) {
    esp_err_t err;

    dled_strip_init(strip);
    dled_strip_create(strip, DLED_WS281x, NUM_LEDS, led_brightness); // NOTE: DLED_WS281x works with WS2811 12mm pixels

    rmt_dled_init(rps);

    err = rmt_dled_create(rps, strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[0x%x] rmt_dled_init failed", err);
        while(true) { }
    }
    // TODO: Change this to accept a passed-in GPIO
    err = rmt_dled_config(rps, strip1_gpio, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[0x%x] rmt_dled_config failed", err);
        while(true) { }
    }

    err = rmt_dled_send(rps);
    if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
    else               { ESP_LOGI(TAG, "LEDs initialized and turned off"); }

    uint16_t step;

    // Blank the LEDs on startup
    step = 0;
    while (step < strip->length) {
        dled_pixel_move_pixel(strip->pixels, strip->length, 0, step);
        dled_strip_fill_buffer(strip);
        rmt_dled_send(rps);
        step++;
    }
}

void led_rainbow(void *event_ctx) {
    uint16_t step;
    esp_err_t err;
    step = 0;
    while (true) {
        while (step < UINT16_MAX) {
            dled_pixel_rainbow_step(strip.pixels, strip.length, led_brightness, step);
            dled_strip_fill_buffer(&strip);
            err = rmt_dled_send(&rps);
            if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
            step++;
            delay_ms(effect_speed_delay);
        }
        step = 0;
    }
}

void led_rainbow_marquee(void *event_ctx) {
    uint16_t step;
    esp_err_t err;
    step = 0;
    // For this effect we let the previous effect get overwritten gradually (because it looks cool)
    while (true) {
        while (step < UINT16_MAX) {
            // Rainbow all the pixels that are divisible by 3 and bove the pixels to the left by one
            if (step % 3 == 0) {
                strip.pixels[strip.length-1] = dled_pixel_get_color_by_index(led_brightness, step);
            } else {
                dled_pixel_set(&strip.pixels[strip.length-1], 0, 0, 0); // WS2811 are GRB
            }
            rotate_pixels(strip.pixels, strip.length);
            dled_strip_fill_buffer(&strip);
            err = rmt_dled_send(&rps);
            if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
            step++;
            delay_ms(effect_speed_delay);
        }
        step = 0;
    }
}

void set_strip_color(int r, int b, int g, int speed) {
    esp_err_t err;
    uint16_t step = 0;
    while (step < strip.length) {
        dled_pixel_set(&strip.pixels[step], g, r, b); // WS2811 are GRB
        dled_strip_fill_buffer(rps.strip); // Do them one at a time to make it smooooooth and cool
        err = rmt_dled_send(&rps);
        if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
        step++;
        delay_ms(speed);
    }
}

void led_blank(void *event_ctx) {
    while (true) {
        set_strip_color(0,0,0,50); // All black (off)
        delay_ms(50); // This one doesn't need an adjustable delay
    }
}

void led_set_brightness(pixel_t *pixel, int max_cc_val) {
    // Brightness value is 0-255 so we need to figure out the percentage to fade it in or out for each color based on the number of steps
    float percent = (float)max_cc_val/255.0;
    pixel->r = FLOAT_TO_INT((float)pixel->r*percent);
    pixel->g = FLOAT_TO_INT((float)pixel->g*percent);
    pixel->b = FLOAT_TO_INT((float)pixel->b*percent);
}

void led_color(void *event_ctx) {
    // Convert the hex to r, g, and b codes we can send to the strip...
    char *nohash = substring(led_palette, 1, 6); // Remove the leading #
    int r, g, b;
    sscanf(nohash, "%02x%02x%02x", &r, &g, &b);
//     printf("r, g, b = %d, %d, %d\n", r, g, b);
    uint16_t step = 0;
    esp_err_t err;
    while (true) { // infinite loop because that's how tasks work
        while (step < strip.length) {
            dled_pixel_set(&strip.pixels[step], g, r, b); // WS2811 are GRB
            led_set_brightness(&strip.pixels[step], led_brightness);
            dled_strip_fill_buffer(rps.strip); // Do them one at a time to make it smooooooth and cool
            err = rmt_dled_send(&rps);
            if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
            step++;
        }
        delay_ms(effect_speed_delay);
        step = 0;
    }
}

// Enumerate the LEDs forwards and backwards using solid color mode
void led_enumerate(void *event_ctx) {
    // Convert the hex to r, g, and b codes we can send to the strip...
    char *nohash = substring(led_palette, 1, 6); // Remove the leading #
    int r, g, b;
    sscanf(nohash, "%02x%02x%02x", &r, &g, &b);
    uint16_t step = 0;
    bool step_reverse = false;
    esp_err_t err;
    // Start by setting the first pixel of the array to green and all others off
    dled_pixel_set(&strip.pixels[0], 255, 0, 0);
    led_set_brightness(&strip.pixels[0], led_brightness);
    for (int i = 1; i < strip.length; i++) {
        dled_pixel_set(&strip.pixels[i], 0, 0, 0); // WS2811 are GRB
    }
    while (true) { // infinite loop because that's how tasks work
        while (step < strip.length) {
            dled_strip_fill_buffer(rps.strip); // Do them one at a time to make it smooooooth and cool
            err = rmt_dled_send(&rps);
            if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
            if (step_reverse) {
                rotate_pixels_reverse(strip.pixels, strip.length);
            } else {
                rotate_pixels(strip.pixels, strip.length);
            }
            step++;
            delay_ms(effect_speed_delay);
        }
        if (step_reverse) {
            step_reverse = false;
        } else {
            step_reverse = true;
        }
        step = 0;
        if (strip.pixels[0].r > 0) { // Red mode, switch to green
            dled_pixel_set(&strip.pixels[0], 0, 255, 0);
        } else if (strip.pixels[0].g > 0) { // Green mode, switch to blue
            dled_pixel_set(&strip.pixels[0], 0, 0, 255);
        } else if (strip.pixels[0].b > 0) { // Blue mode, switch to red
            dled_pixel_set(&strip.pixels[0], 255, 0, 0);
        }
    }
}

// Uses the current palette to twinkle random LEDs on and off
void led_twinkle(void *event_ctx) {
    uint16_t step = 0;
    esp_err_t err;
    while (true) { // infinite loop because that's how tasks work
        while (step < strip.length) {
            // Convert the hex to r, g, and b codes we can send to the strip...
            char *nohash = substring(led_palette, 1, 6); // Remove the leading #
            int r, g, b;
            sscanf(nohash, "%02x%02x%02x", &r, &g, &b);
            uint16_t val = random(1, 100);
            if (val < twinkly) {
                dled_pixel_set(&strip.pixels[step], g, r, b); // WS2811 are GRB
            } else {
                dled_pixel_set(&strip.pixels[step], 0, 0, 0); // Turn this pixel off
            }
            led_set_brightness(&strip.pixels[step], led_brightness);
            err = rmt_dled_send(&rps);
            if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
            dled_strip_fill_buffer(rps.strip);
            step++;
        }
        delay_ms(effect_speed_delay*4);
        step = 0;
    }
}

void led_marquee(void *event_ctx) {
    uint16_t step = 0;
    esp_err_t err;
    char *nohash = substring(led_palette, 1, 6); // Remove the leading #
    int r, g, b;
    sscanf(nohash, "%02x%02x%02x", &r, &g, &b); // Converts strings like "FF00FF" to rgb values from 0-255
    // Start by filling the pixels array with our marquee sequence (every 3rd pixel off)
    // Every 3rd pixel is turned on
    for (int i = 0; i < strip.length; i++) {
        if (i % 3 == 0) {
            dled_pixel_set(&strip.pixels[i], g, r, b); // WS2811 are GRB
            led_set_brightness(&strip.pixels[i], led_brightness);
        } else {
            dled_pixel_set(&strip.pixels[i], 0, 0, 0); // WS2811 are GRB
        }
    }
    while (true) {
        while (step < strip.length) {
            rotate_pixels(strip.pixels, strip.length);
            // Broken:
//             dled_pixel_chase_pixels(strip.pixels, strip.length, led_brightness, step, leds_at_a_time);
            dled_strip_fill_buffer(rps.strip); // Do them one at a time to make it smooooooth and cool
            err = rmt_dled_send(&rps);
            if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
            step++;
            delay_ms(effect_speed_delay);
        }
        step = 0;
    }
}

// TODO:
// void led_palette_chaser(void *event_ctx) {
//     uint16_t step = 0;
//     esp_err_t err;
//     char *nohash = substring(led_palette, 1, 6); // Remove the leading #
//     int r, g, b;
//     sscanf(nohash, "%02x%02x%02x", &r, &g, &b); // Converts strings like "FF00FF" to rgb values from 0-255
//     // Start by filling the pixels array with our marquee sequence (every 3rd pixel off)
//     // Every 3rd pixel is turned on
//     for (int i = 0; i < strip.length; i++) {
//         if (i % 3 == 0) {
//             dled_pixel_set(&strip.pixels[i], g, r, b); // WS2811 are GRB
//             led_set_brightness(&strip.pixels[i], led_brightness);
//         } else {
//             dled_pixel_set(&strip.pixels[i], 0, 0, 0); // WS2811 are GRB
//         }
//     }
//     while (true) {
//         while (step < strip.length) {
//             rotate_pixels(strip.pixels, strip.length); // Rotate
//             // Broken:
// //             dled_pixel_chase_pixels(strip.pixels, strip.length, led_brightness, step, leds_at_a_time);
//             dled_strip_fill_buffer(rps.strip); // Do them one at a time to make it smooooooth and cool
//             err = rmt_dled_send(&rps);
//             if (err != ESP_OK) { ESP_LOGE(TAG, "[0x%x] rmt_dled_send failed", err); }
//             step++;
//             delay_ms(effect_speed_delay);
//         }
//         step = 0;
//     }
// }

void read_flash_settings() {
    ESP_LOGI(TAG, "Reading settings from NVS flash...");
    nvs_handle storage_handle;
    esp_err_t err;
    err = nvs_open(broadway_nvs_namespace, NVS_READONLY, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        // Read palette
        size_t string_size;
        err = nvs_get_str(storage_handle, "palette", NULL, &string_size); // Just gets the size
        if (err != ESP_OK) { ESP_LOGE(TAG, "Reading palette setting failed: [0x%x]", err); }
        char* palette_val = malloc(string_size);
        err = nvs_get_str(storage_handle, "palette", palette_val, &string_size);
        if (err != ESP_OK) { ESP_LOGE(TAG, "Reading palette setting failed: [0x%x]", err); }
        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "Setting palette... %.*s", string_size, palette_val);
                strcpy(led_palette, palette_val); // Apply the setting
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initalized yet; that's OK
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        free(palette_val); // Clean up
        uint8_t speed_val = 0;
        err = nvs_get_u8(storage_handle, "speed", &speed_val);
        switch (err) {
            case ESP_OK:
                effect_speed_delay = speed_val; // Apply the setting
                ESP_LOGI(TAG, "Setting speed... %d", effect_speed_delay);
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initalized yet; that's OK
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        uint8_t brightness_val = 0;
        err = nvs_get_u8(storage_handle, "brightness", &brightness_val);
        switch (err) {
            case ESP_OK:
                led_brightness = brightness_val; // Apply the setting
                ESP_LOGI(TAG, "Setting brightness... %d", led_brightness);
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initalized yet; that's OK
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        uint8_t effect_val = 0;
        err = nvs_get_u8(storage_handle, "effect", &effect_val);
        ESP_LOGI(TAG, "Stored effect val: %d", effect_val);
        switch (err) {
            case ESP_OK:
                current_effect = (led_effect)effect_val; // Apply the setting
                ESP_LOGI(TAG, "Starting effect: %d", current_effect);
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initalized yet; that's OK
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        // Close/cleanup
        nvs_close(storage_handle);
    }
    ESP_LOGI(TAG, "Settings loaded!");
}

void store_palette() {
    nvs_handle storage_handle;
    esp_err_t err;
    err = nvs_open(broadway_nvs_namespace, NVS_READWRITE, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        // Read
        size_t string_size;
        bool needs_write = false;
        err = nvs_get_str(storage_handle, "palette", NULL, &string_size);
        char* palette_val = malloc(string_size);
        err = nvs_get_str(storage_handle, "palette", palette_val, &string_size);
        switch (err) {
            case ESP_OK:
                needs_write = true;
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initialized yet (that's OK)
                needs_write = true;
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        if (needs_write) {
            // Write
            if (strcmp(led_palette, palette_val) != 0) {
                err = nvs_set_str(storage_handle, "palette", led_palette);
                err = nvs_commit(storage_handle);
            }
        }
        // Close
        free(palette_val);
        nvs_close(storage_handle);
    }
}

void store_speed() {
    nvs_handle storage_handle;
    esp_err_t err;
    err = nvs_open(broadway_nvs_namespace, NVS_READWRITE, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        // Read
        bool needs_write = false;
        uint8_t speed_val = 0;
        err = nvs_get_u8(storage_handle, "speed", &speed_val);
        switch (err) {
            case ESP_OK:
                needs_write = true;
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initialized yet (that's OK)
                needs_write = true;
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        if (needs_write) {
            // Write
            if (speed_val != effect_speed_delay) {
                err = nvs_set_u8(storage_handle, "speed", effect_speed_delay);
                err = nvs_commit(storage_handle);
            }
        }
        // Close
        nvs_close(storage_handle);
    }
}

void store_brightness() {
    nvs_handle storage_handle;
    esp_err_t err;
    err = nvs_open(broadway_nvs_namespace, NVS_READWRITE, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        // Read
        bool needs_write = false;
        uint8_t brightness_val = 0;
        err = nvs_get_u8(storage_handle, "brightness", &brightness_val);
        switch (err) {
            case ESP_OK:
                needs_write = true;
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initialized yet (that's OK)
                needs_write = true;
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        if (needs_write) {
            // Write
            if (brightness_val != led_brightness) {
                err = nvs_set_u8(storage_handle, "brightness", led_brightness);
                err = nvs_commit(storage_handle);
            }
        }
        // Close
        nvs_close(storage_handle);
    }
}

void store_effect() {
    nvs_handle storage_handle;
    esp_err_t err;
    err = nvs_open(broadway_nvs_namespace, NVS_READWRITE, &storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        // Read
        bool needs_write = false;
        uint8_t effect_val = 0;
        err = nvs_get_u8(storage_handle, "effect", &effect_val);
        switch (err) {
            case ESP_OK:
                needs_write = true;
                break;
            case ESP_ERR_NVS_NOT_FOUND: // Not initialized yet (that's OK)
                needs_write = true;
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
        if (needs_write) {
            // Write
            if (effect_val != current_effect) {
                err = nvs_set_u8(storage_handle, "effect", (uint8_t)current_effect);
                err = nvs_commit(storage_handle);
            }
        }
        // Close
        nvs_close(storage_handle);
    }
}

void showtime() {
//     ESP_LOGI(TAG, "Showtime!");
    // End any running effect task
    if (led_task_handle) {
//         printf("Calling vTaskDelete on the existing task.\n");
        vTaskDelete(led_task_handle);
        led_task_handle = NULL;
    }
    // (Re)start any effects
    if (current_effect == OFF) {
        ESP_LOGI(TAG, "Turning the lights off...");
        xTaskCreate(led_blank, "blank", STACK_SIZE, NULL, LED_TASK_PRIORITY, &led_task_handle);
    } else if (current_effect == ENUMERATE) {
        ESP_LOGI(TAG, "Creating 'enumerate' task...");
        xTaskCreate(led_enumerate, "enumerate", STACK_SIZE, NULL, LED_TASK_PRIORITY, &led_task_handle);
    } else if (current_effect == COLOR) {
        ESP_LOGI(TAG, "Creating 'color' task...");
        xTaskCreate(led_color, "color", STACK_SIZE, NULL, LED_TASK_PRIORITY, &led_task_handle);
    } else if (current_effect == RAINBOW) {
        ESP_LOGI(TAG, "Creating 'rainbow' task...");
        xTaskCreate(led_rainbow, "rainbow", STACK_SIZE, NULL, LED_TASK_PRIORITY, &led_task_handle);
    } else if (current_effect == TWINKLE) {
        ESP_LOGI(TAG, "Creating 'twinkle' task...");
        xTaskCreate(led_twinkle, "twinkle", STACK_SIZE, NULL, LED_TASK_PRIORITY, &led_task_handle);
    } else if (current_effect == MARQUEE) {
        ESP_LOGI(TAG, "Creating 'marquee' task...");
        xTaskCreate(led_marquee, "marquee", STACK_SIZE, NULL, LED_TASK_PRIORITY, &led_task_handle);
    } else if (current_effect == RAINBOW_MARQUEE) {
        ESP_LOGI(TAG, "Creating 'rainbow_marquee' task...");
        xTaskCreate(led_rainbow_marquee, "rainbow_marquee", STACK_SIZE, NULL, LED_TASK_PRIORITY, &led_task_handle);
    }
    if (current_effect != OFF) {
        prev_effect = current_effect;
    }
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    esp_mqtt_client_handle_t client = event->client;
    bool new_effect = false;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe(client, CONFIG_MQTT_TOPIC_CONTROL, 1);
            esp_mqtt_client_subscribe(client, CONFIG_MQTT_TOPIC_COLOR, 1);
            esp_mqtt_client_subscribe(client, CONFIG_MQTT_TOPIC_MODE, 1);
            esp_mqtt_client_subscribe(client, CONFIG_MQTT_TOPIC_SPEED, 1);
            esp_mqtt_client_subscribe(client, CONFIG_MQTT_TOPIC_BRIGHTNESS, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            // End any running effect task (if this is a re-sub situation)
            if (led_task_handle) {
                printf("Calling vTaskDelete on the existing task.\n");
                vTaskDelete(led_task_handle);
                led_task_handle = NULL;
            }
            new_effect = true; // Make sure everything starts properly
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\n", event->topic_len, event->topic);
            printf("DATA=%.*s\n", event->data_len, event->data);
            printf("DATA length=%d\n", event->data_len);
            // Set the mode
            if (strncmp(event->topic, CONFIG_MQTT_TOPIC_MODE, strlen(CONFIG_MQTT_TOPIC_MODE)) == 0) {
                // Start the new requested effect
                if (strncmp(event->data, "rainbow", 7) == 0) {
                    current_effect = RAINBOW;
                } else if (strncmp(event->data, "color", 5) == 0) {
                    current_effect = COLOR;
                }  else if (strncmp(event->data, "enumerate", 4) == 0) {
                    current_effect = ENUMERATE;
                } else if (strncmp(event->data, "twinkle", 7) == 0) {
                    current_effect = TWINKLE;
                } else if (strncmp(event->data, "marquee", 7) == 0) {
                    current_effect = MARQUEE;
                } else if (strncmp(event->data, "rmarquee", 8) == 0) {
                    current_effect = RAINBOW_MARQUEE;
                }
                store_effect(); // Save the running effect to NVS flash
                new_effect = true;
            // Set the lights on or off (it's actually just a different "effect"):
            } else if (strncmp(event->topic, CONFIG_MQTT_TOPIC_CONTROL, strlen(CONFIG_MQTT_TOPIC_CONTROL)) == 0) {
                if (strncmp(event->data, "OFF", 3) == 0) {
                    current_effect = OFF;
                } else if (strncmp(event->data, "ON", 2) == 0) {
                    current_effect = prev_effect;
                }
                store_effect(); // Save the running effect to NVS flash
                new_effect = true;
            } else if (strncmp(event->topic, CONFIG_MQTT_TOPIC_COLOR, strlen(CONFIG_MQTT_TOPIC_COLOR)) == 0) {
                strncpy(led_palette, event->data, event->data_len); // Set the palette
                store_palette(); // Save the new palette to NVS flash
                new_effect = true;
            } else if (strncmp(event->topic, CONFIG_MQTT_TOPIC_SPEED, strlen(CONFIG_MQTT_TOPIC_SPEED)) == 0) {
                int i = atoi(event->data);
                if (i >= 0 && i <= 255) {
                    i = 255 - i; // Convert speed to ms delay
                    if (i == 0) {
                        effect_speed_delay = 10; // Make it at least ten
                    } else {
                        effect_speed_delay = i;
                    }
                }
                store_speed(); // Save the current speed to NVS flash
                new_effect = true;
            } else if (strncmp(event->topic, CONFIG_MQTT_TOPIC_BRIGHTNESS, strlen(CONFIG_MQTT_TOPIC_BRIGHTNESS)) == 0) {
                char *temp = substring(event->data, 0, event->data_len);
                int i = atoi(temp);
                if (i > 0 && i <= 255) {
                    printf("Setting led_brightness (i)=%d\n", i);
                    led_brightness = i;
                    strip.max_cc_val = led_brightness;
                }
                store_brightness(); // Store the current brightness level to NVS flash
                new_effect = true;
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
    }
    if (new_effect) {
        showtime(); // Start or restart the show!
    }
    return ESP_OK;
}

static void mqtt_app_start(void) {
    ESP_LOGI(TAG, "Waiting for Wifi before starting MQTT client...");
    xEventGroupWaitBits(
        wifi_manager_event_group,
        WIFI_CONNECTED_BIT,
        false,
        true,
        portMAX_DELAY);
    ESP_LOGI(TAG, "Starting MQTT client");
    // Setup our mDNS stuff
    start_mdns_service();
    // Start the MQTT task
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
        .event_handle = mqtt_event_handler,
        .username = CONFIG_MQTT_USERNAME,
        .password = CONFIG_MQTT_PASSWORD,
        .client_id = CONFIG_HOSTNAME
    };

#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_LOGI(TAG, "MQTT Connecting to broker: [%s]", CONFIG_BROKER_URL);
    esp_mqtt_client_start(client);
}

/*
  Read values sensed at touch sensors 0, 2, and 3 (TOUCH0, TOUCH2, TOUCH3) and
  do one of three things:
      * Turn the lights on/off
      * Change the current mode/effect
      * Turn the brightness up/down
 */
static void tp_read_task(void *pvParameter) {
    uint16_t touch_value;
    bool touched = false;
    uint16_t delay = 200; // ms delay between loops/checks
    uint16_t long_press0 = 0; // Used to detect a long press on the power touch button (and to de-bounce)
    uint16_t long_press2 = 0; // Really just used to de-bounce this touch pad
    uint16_t long_press3 = 0; // Only used to detect when brightness adjustment is done
    // NOTE: We don't bother detecting long press on TOUCH3 (brightness) because it doesn't make (much) sense
    while (true) {
        touch_pad_read_raw_data(TOUCH0, &touch_value);
        // NOTE: Filter was too slow in my testing.  Makes more sense with constant-touch situations:
//         touch_pad_read_filtered(i, &touch_value);
//         printf(" T0:[%4d] ", touch_value);
        if (touch_value && touch_value < TOUCH_THRESHOLD) {
        // Turn the lights on or off
            long_press0 += delay; // Increment
            if (long_press0 < delay*2) { // De-bounce (and don't go nuts changing modes while the user presses a touch pad)
                touched = true;
                if (current_effect == OFF) {
                    current_effect = prev_effect;
                    if (current_effect == OFF) { // Previous effect *was* OFF
                        current_effect = RAINBOW; // Start anew
                    }
                    prev_effect = OFF;
                } else {
                    prev_effect = current_effect;
                    current_effect = OFF;
                }
                store_effect(); // Save the running effect (ON or OFF) make to NVS flash
            }
        } else {
            long_press0 = 0; // No longer touching...  Reset the long press timer
        }
        touch_pad_read_raw_data(TOUCH2, &touch_value);
//         printf(" T2:[%4d] ", touch_value);
        if (touch_value && touch_value < TOUCH_THRESHOLD) {
        // Cycle through the effects/modes (skipping OFF)
            long_press2 += delay; // Increment
            if (long_press2 < delay*2) { // De-bounce (and don't go nuts changing modes while the user presses a touch pad)
                touched = true;
                current_effect++;
                if (current_effect == ENUMERATE) {
                    // This one is special; skip it
                    current_effect++;
                }
                if (current_effect > 6) {
                    current_effect = 1;
                }
                store_effect(); // Save the running effect to NVS flash
            }
        } else {
            long_press2 = 0; // No longer touching...  Reset the long press timer
        }
        touch_pad_read_raw_data(TOUCH3, &touch_value);
//         printf(" T3:[%4d] ", touch_value);
        if (touch_value && touch_value < TOUCH_THRESHOLD) {
        // Cycle brightness up until max then down until min
            touched = true;
            long_press3 += delay;
            ESP_LOGI(TAG, "Adjusting brightness (%d) %s", led_brightness, led_brightness_up ? "up" : "down");
            if (led_brightness_up) {
                led_brightness += 10;
                if (led_brightness > 245) {
                    led_brightness = 255;
                    led_brightness_up = false;
                }
            } else {
                led_brightness -= 10;
                if (led_brightness < 10) {
                    led_brightness = 10;
                    led_brightness_up = true;
                }
            }
        } else {
            if (long_press3) {
                store_brightness();
            }
            long_press3 = 0;
        }
        // Handle the long-press situation (reset the wifi preferences)
        if (long_press0 > LONG_PRESS_THRESHOLD && long_press0 < LONG_PRESS_THRESHOLD + (delay*2)) {
            ESP_LOGI(TAG, "Long press of power button detected.  Resetting wifi_manager...");
            long_press0 = LONG_PRESS_THRESHOLD + (delay*2) + 1; // Keep it stuck at threshold + delay*2 + 1 until touch state changes
            wifi_manager_disconnect_async(); // This disconnects the wifi and starts the AP back up (also erases the wifi_manager flash stuff)
            strcpy(led_palette, "#FF0000");; // Set it to red to indicate something just happened
            current_effect = ENUMERATE; // Set to enumerate mode to indicate what just happened
            showtime();
        } else if (touched) {
            showtime(); // Start/stop the LEDs
            touched = false;
        }
        delay_ms(delay);
    }
}

static void obtain_time(void) {
    xEventGroupWaitBits(
        wifi_manager_event_group,
        WIFI_CONNECTED_BIT,
        false,
        true,
        portMAX_DELAY);
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}

static void initialize_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL); // NOTE: By default it synchronizes the time every hour
    sntp_setservername(0, CONFIG_NTP_SERVER);
    sntp_init();
}

// TODO: This will be our clock/hourly time scheduler thing:
void time_task(void *pvParameter) {
    ESP_LOGI(TAG, "Waiting for wifi before starting the time setter/scheduler...");
    xEventGroupWaitBits(
        wifi_manager_event_group,
        WIFI_CONNECTED_BIT,
        false,
        true,
        portMAX_DELAY);
    ESP_LOGI(TAG, "Starting time setter/scheduler");
    time_t now = 0;
    bool time_set = false;
    struct tm timeinfo = { 0 };
    char strftime_buf[64];
    ESP_LOGI(TAG, "Setting the time");
    obtain_time(); // Start by setting the time (initializing SNTP)
    // Set timezone to Mountain Standard Time (Phoenix, where my Secret Santa lives!)
    setenv("TZ", "MST7", 1); // TODO: Make this a configuration item
    tzset();
    // NOTE: The list of timezone strings...
    //       https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
    while (true) {
        if (!time_set) {
            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            if (timeinfo.tm_year > (2017 - 1900)) { // Time has been set
                time_set = true;
                ESP_LOGI(TAG, "The current date/time in America/Phoenix is: %s", strftime_buf);
            }
        }
        delay_ms(1000); // Every second should be OK for a task scheduler (I hope)
    }
}

void app_main() {
    /* disable the default wifi logging */
    esp_log_level_set("wifi", ESP_LOG_NONE);

    /* initialize flash memory */
    nvs_flash_init();

    // Read in settings from NVS
    read_flash_settings();

    /* start the HTTP Server task */
    xTaskCreate(&http_server, "http_server", 2048, NULL, 5, &task_http_server);

    /* start the wifi manager task */
    xTaskCreate(&wifi_manager, "wifi_manager", 4096, NULL, 4, &task_wifi_manager);

    /* your code should go here. In debug mode we create a simple task on core 2 that monitors free heap memory */
    // Start our clock-setting and time management task
    xTaskCreate(&time_task, "time_task", 2048, NULL, 20, &task_time_manager);

    // Initialize touch pad peripheral.
    // The default fsm mode is software trigger mode.
    touch_pad_init();
    // Set reference voltage for charging/discharging
    // In this case, the high reference valtage will be 2.7V - 1V = 1.7V
    // The low reference voltage will be 0.5
    // The larger the range, the larger the pulse count value.
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    tp_init();
    touch_pad_filter_start(TOUCHPAD_FILTER_TOUCH_PERIOD);

    // Start task to read values sensed by pads
    xTaskCreate(&tp_read_task, "touch_pad_read_task", 2048, NULL, 5, NULL);

    // Setup WS2811 pixel strip
    initialize_leds(&rps, &strip);

    // Start the light show immediately (so we don't NEED Internet before we start working)
    showtime();

    // Start up the MQTT listener (most important bit!)
    mqtt_app_start();
}
