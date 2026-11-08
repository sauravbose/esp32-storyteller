/*
 * Screen test for the LilyGO T-Display S3.
 *
 * What it does:
 *   1. Switches on the screen (power, data bus, backlight).
 *   2. Draws a title, some text, and three colour bars.
 *   3. Shows on screen which button you pressed.
 *
 * Pin numbers come from LilyGO's pin_config.h for this board.
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "font8x16.h"

#define TAG "screen"

// ---------- Pins (from LilyGO's pin_config.h) ----------
#define PIN_POWER_ON 15  // must be HIGH or the screen gets no power on battery
#define PIN_LCD_BL   38  // backlight
#define PIN_LCD_RD    9  // "read" line: we only write, so hold it HIGH
#define PIN_LCD_WR    8  // "write" clock: pulses once per byte sent
#define PIN_LCD_DC    7  // HIGH = this byte is data, LOW = this byte is a command
#define PIN_LCD_CS    6  // "chip select": tells the screen we're talking to it
#define PIN_LCD_RES   5  // reset
static const int DATA_PINS[8] = {39, 40, 41, 42, 45, 46, 47, 48};  // 8 data wires

#define PIN_BUTTON_BOOT 0
#define PIN_BUTTON_KEY  14

// ---------- Screen size (landscape) ----------
#define LCD_W 320
#define LCD_H 170
#define PIXEL_CLOCK_HZ (6528000)  // LilyGO's value: 6.5 million bytes/second

// ---------- Colours ----------
// The screen uses "RGB565": 5 bits red, 6 bits green, 5 bits blue = 16 bits per pixel.
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define BLACK  RGB565(0, 0, 0)
#define WHITE  RGB565(255, 255, 255)
#define RED    RGB565(255, 0, 0)
#define GREEN  RGB565(0, 255, 0)
#define BLUE   RGB565(0, 0, 255)
#define YELLOW RGB565(255, 220, 0)
#define GREY   RGB565(140, 140, 140)

// ---------- The framebuffer ----------
// One 16-bit number per pixel = 320 x 170 x 2 bytes = about 109KB.
// We draw into this memory first, then send the whole thing to the screen.
static uint16_t *framebuffer;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t send_done;  // signals "screen finished receiving the image"

// Called automatically (from an interrupt) when a transfer to the screen finishes.
static bool on_send_done(esp_lcd_panel_io_handle_t io,
                         esp_lcd_panel_io_event_data_t *edata, void *ctx) {
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR(send_done, &woke);
  return woke == pdTRUE;
}

// The screen expects each pixel's two bytes high-byte-first, but the ESP32
// stores numbers low-byte-first, so we swap the bytes when writing a pixel.
static inline void put_pixel(int x, int y, uint16_t colour) {
  if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
  framebuffer[y * LCD_W + x] = (colour >> 8) | (colour << 8);
}

static void fill_rect(int x, int y, int w, int h, uint16_t colour) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) put_pixel(i, j, colour);
}

// Draw one character from the bitmap font. scale=2 makes it twice as big.
static void draw_char(int x, int y, char c, uint16_t colour, int scale) {
  if (c < 32 || c > 126) c = '?';
  const uint8_t *rows = font8x16[c - 32];
  for (int row = 0; row < FONT_H; row++)
    for (int col = 0; col < FONT_W; col++)
      if (rows[row] & (0x80 >> col))
        fill_rect(x + col * scale, y + row * scale, scale, scale, colour);
}

static void draw_text(int x, int y, const char *text, uint16_t colour, int scale) {
  for (; *text; text++, x += FONT_W * scale) draw_char(x, y, *text, colour, scale);
}

// Send the whole framebuffer to the screen and wait until it's done.
static void show(void) {
  esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);
  xSemaphoreTake(send_done, portMAX_DELAY);
}

static void screen_init(void) {
  // 1. Power on the screen, and hold the unused "read" line high.
  gpio_config_t out = {
      .pin_bit_mask = (1ULL << PIN_POWER_ON) | (1ULL << PIN_LCD_RD) | (1ULL << PIN_LCD_BL),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&out);
  gpio_set_level(PIN_POWER_ON, 1);
  gpio_set_level(PIN_LCD_RD, 1);
  gpio_set_level(PIN_LCD_BL, 0);  // backlight off until the image is ready

  // 2. Set up the 8-wire data bus (called "i80" after old Intel 8080 chips).
  esp_lcd_i80_bus_handle_t bus;
  esp_lcd_i80_bus_config_t bus_config = {
      .dc_gpio_num = PIN_LCD_DC,
      .wr_gpio_num = PIN_LCD_WR,
      .clk_src = LCD_CLK_SRC_DEFAULT,
      .bus_width = 8,
      .max_transfer_bytes = LCD_W * LCD_H * sizeof(uint16_t),
  };
  for (int i = 0; i < 8; i++) bus_config.data_gpio_nums[i] = DATA_PINS[i];
  ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &bus));

  // 3. Describe how we talk to the screen over that bus.
  esp_lcd_panel_io_handle_t io;
  esp_lcd_panel_io_i80_config_t io_config = {
      .cs_gpio_num = PIN_LCD_CS,
      .pclk_hz = PIXEL_CLOCK_HZ,
      .trans_queue_depth = 10,
      .on_color_trans_done = on_send_done,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .dc_levels = {.dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1},
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_config, &io));

  // 4. The screen's controller chip is an ST7789. ESP-IDF has a driver for it.
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = PIN_LCD_RES,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);

  // 5. Board-specific settings, copied from LilyGO's example:
  esp_lcd_panel_invert_color(panel, true);   // this panel shows inverted colours otherwise
  esp_lcd_panel_swap_xy(panel, true);        // landscape: 320 wide x 170 tall
  esp_lcd_panel_mirror(panel, false, true);
  esp_lcd_panel_set_gap(panel, 0, 35);       // the 170 visible rows start 35 rows in

  // Extra tuning commands LilyGO sends for newer versions of this screen.
  // Format: command byte, its data bytes, number of data bytes.
  static const struct { uint8_t cmd; uint8_t data[14]; uint8_t len; } tuning[] = {
      {0x3A, {0x05}, 1},
      {0xB2, {0x0B, 0x0B, 0x00, 0x33, 0x33}, 5},
      {0xB7, {0x75}, 1},
      {0xBB, {0x28}, 1},
      {0xC0, {0x2C}, 1},
      {0xC2, {0x01}, 1},
      {0xC3, {0x1F}, 1},
      {0xC6, {0x13}, 1},
      {0xD0, {0xA7}, 1},
      {0xD0, {0xA4, 0xA1}, 2},
      {0xD6, {0xA1}, 1},
      {0xE0, {0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14},
      {0xE1, {0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14},
  };
  for (int i = 0; i < sizeof(tuning) / sizeof(tuning[0]); i++)
    esp_lcd_panel_io_tx_param(io, tuning[i].cmd, tuning[i].data, tuning[i].len);

  esp_lcd_panel_disp_on_off(panel, true);

  // 6. Reserve the framebuffer in the fast internal RAM ("DMA-capable" means the
  //    data bus hardware can read it directly while the CPU does other work).
  framebuffer = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
  assert(framebuffer);
  send_done = xSemaphoreCreateBinary();
}

static void draw_page(const char *button_msg) {
  fill_rect(0, 0, LCD_W, LCD_H, BLACK);
  draw_text(8, 6, "Hello!", YELLOW, 2);  // big title (16x32 per letter)
  draw_text(8, 46, "The screen works.", WHITE, 1);
  draw_text(8, 64, "320 x 170 pixels, 40 x 10 chars", GREY, 1);

  // Colour bars: if these don't say what they show, the colour order is wrong.
  fill_rect(8, 90, 96, 24, RED);
  fill_rect(112, 90, 96, 24, GREEN);
  fill_rect(216, 90, 96, 24, BLUE);
  draw_text(8 + 32, 94, "RED", WHITE, 1);
  draw_text(112 + 28, 94, "GREEN", BLACK, 1);
  draw_text(216 + 28, 94, "BLUE", WHITE, 1);

  draw_text(8, 128, "Press a button:", GREY, 1);
  draw_text(8, 146, button_msg, WHITE, 1);
  show();
}

void app_main(void) {
  screen_init();
  draw_page("(waiting...)");
  gpio_set_level(PIN_LCD_BL, 1);  // image is ready: switch the backlight on
  ESP_LOGI(TAG, "Screen ready. Free internal RAM: %u bytes",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  // Buttons: inputs with an internal "pull-up", so they read 1 normally
  // and 0 while pressed (pressing connects the pin to ground).
  gpio_config_t in = {
      .pin_bit_mask = (1ULL << PIN_BUTTON_BOOT) | (1ULL << PIN_BUTTON_KEY),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&in);

  int last_boot = 1, last_key = 1;
  while (1) {
    int boot = gpio_get_level(PIN_BUTTON_BOOT);
    int key = gpio_get_level(PIN_BUTTON_KEY);
    if (boot == 0 && last_boot == 1) {
      ESP_LOGI(TAG, "BOOT button (GPIO 0) pressed");
      draw_page("BOOT button (GPIO 0)");
    }
    if (key == 0 && last_key == 1) {
      ESP_LOGI(TAG, "KEY button (GPIO 14) pressed");
      draw_page("KEY button (GPIO 14)");
    }
    last_boot = boot;
    last_key = key;
    vTaskDelay(pdMS_TO_TICKS(30));  // check the buttons ~30 times a second
  }
}
