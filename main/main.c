/*
 * Story teller for the LilyGO T-Display S3.
 *
 * A tiny language model (3.3M parameters, trained on children's stories)
 * runs on the ESP32-S3 and types each story onto the screen as it writes it.
 *
 *   BOOT button (below the USB port): tell a new story
 *   KEY  button (above the USB port): change the story's theme
 *
 * The model engine (llm8.c) is from Dory's esp32-llm project (MIT licence),
 * itself a port of Andrej Karpathy's llama2.c. This file replaces that
 * project's main.c with a screen and buttons.
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "font8x16.h"
#include "llm8.h"

#define TAG "story"

// =====================================================================
// Story settings: change these to experiment
// =====================================================================
#define TEMPERATURE 0.8f  // lower = safer, more predictable; higher = wilder
#define TOP_P       0.9f  // only pick from the most likely words (90% of probability)
#define MAX_TOKENS  512   // longest possible story (the model's limit)

// Each theme has a name (shown at the top) and an opening line the model continues.
// An empty opening ("") lets the model start completely on its own.
static const struct {
  const char *name;
  const char *opening;
} THEMES[] = {
    {"Surprise me", ""},
    {"Dragon", "Once upon a time, there was a little dragon"},
    {"Lost puppy", "Once upon a time, there was a little puppy who got lost"},
    {"Princess", "Once upon a time, there was a brave princess"},
    {"Sea", "Once upon a time, a little fish lived in the big blue sea"},
    {"Space", "One day, a boy built a rocket to fly to the moon"},
    {"Bunny", "One day, a little bunny found a big carrot"},
};
#define NUM_THEMES (sizeof(THEMES) / sizeof(THEMES[0]))

// =====================================================================
// Pins (from LilyGO's pin_config.h) and screen setup, same as screen_test
// =====================================================================
#define PIN_POWER_ON 15
#define PIN_LCD_BL   38
#define PIN_LCD_RD    9
#define PIN_LCD_WR    8
#define PIN_LCD_DC    7
#define PIN_LCD_CS    6
#define PIN_LCD_RES   5
static const int DATA_PINS[8] = {39, 40, 41, 42, 45, 46, 47, 48};

#define PIN_BUTTON_BOOT 0   // below the USB port
#define PIN_BUTTON_KEY  14  // above the USB port

#define LCD_W 320
#define LCD_H 170
#define PIXEL_CLOCK_HZ (6528000)

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define BLACK    RGB565(0, 0, 0)
#define WHITE    RGB565(255, 255, 255)
#define CREAM    RGB565(255, 244, 214)
#define YELLOW   RGB565(255, 220, 0)
#define GREY     RGB565(140, 140, 140)
#define NIGHT    RGB565(20, 30, 70)  // header background

static uint16_t *framebuffer;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t send_done;

static bool on_send_done(esp_lcd_panel_io_handle_t io,
                         esp_lcd_panel_io_event_data_t *edata, void *ctx) {
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR(send_done, &woke);
  return woke == pdTRUE;
}

static inline void put_pixel(int x, int y, uint16_t colour) {
  if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
  framebuffer[y * LCD_W + x] = (colour >> 8) | (colour << 8);
}

static void fill_rect(int x, int y, int w, int h, uint16_t colour) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) put_pixel(i, j, colour);
}

static void draw_char(int x, int y, char c, uint16_t colour) {
  if (c < 32 || c > 126) c = '?';
  const uint8_t *rows = font8x16[c - 32];
  for (int row = 0; row < FONT_H; row++)
    for (int col = 0; col < FONT_W; col++)
      if (rows[row] & (0x80 >> col)) put_pixel(x + col, y + row, colour);
}

static void draw_text(int x, int y, const char *text, uint16_t colour) {
  for (; *text; text++, x += FONT_W) draw_char(x, y, *text, colour);
}

// Send rows y0..y1 of the framebuffer to the screen. Sending only the rows
// that changed is much faster than sending the whole screen every word.
static void show_rows(int y0, int y1) {
  esp_lcd_panel_draw_bitmap(panel, 0, y0, LCD_W, y1, &framebuffer[y0 * LCD_W]);
  xSemaphoreTake(send_done, portMAX_DELAY);
}

static void screen_init(void) {
  gpio_config_t out = {
      .pin_bit_mask = (1ULL << PIN_POWER_ON) | (1ULL << PIN_LCD_RD) | (1ULL << PIN_LCD_BL),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&out);
  gpio_set_level(PIN_POWER_ON, 1);
  gpio_set_level(PIN_LCD_RD, 1);
  gpio_set_level(PIN_LCD_BL, 0);

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

  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = PIN_LCD_RES,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  esp_lcd_panel_invert_color(panel, true);
  esp_lcd_panel_swap_xy(panel, true);
  esp_lcd_panel_mirror(panel, false, true);
  esp_lcd_panel_set_gap(panel, 0, 35);

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

  // The framebuffer goes in fast internal RAM. We reserve it BEFORE loading
  // the model, so the model can't use up the internal RAM first.
  framebuffer = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
  assert(framebuffer);
  send_done = xSemaphoreCreateBinary();
  memset(framebuffer, 0, LCD_W * LCD_H * sizeof(uint16_t));
}

// =====================================================================
// The "page": a header bar plus 9 lines of word-wrapped story text
// =====================================================================
#define HEADER_H  20                    // header bar height in pixels
#define TEXT_Y    (HEADER_H + 4)        // where the first text line starts
#define COLS      (LCD_W / FONT_W)      // 40 characters per line
#define ROWS      ((LCD_H - TEXT_Y) / FONT_H)  // 9 lines of text

static char lines[ROWS][COLS + 1];  // the text currently on screen
static int cur_row = 0, cur_col = 0;
static int theme = 0;

static void draw_header(const char *status) {
  fill_rect(0, 0, LCD_W, HEADER_H, NIGHT);
  draw_text(6, 2, THEMES[theme].name, YELLOW);
  draw_text(LCD_W - 6 - strlen(status) * FONT_W, 2, status, GREY);
  show_rows(0, HEADER_H);
}

// Redraw one line of text from lines[] and send just that strip.
static void draw_line(int row) {
  int y = TEXT_Y + row * FONT_H;
  fill_rect(0, y, LCD_W, FONT_H, BLACK);
  draw_text(0, y, lines[row], CREAM);
  show_rows(y, y + FONT_H);
}

static void draw_all_lines(void) {
  fill_rect(0, HEADER_H, LCD_W, LCD_H - HEADER_H, BLACK);
  for (int r = 0; r < ROWS; r++) draw_text(0, TEXT_Y + r * FONT_H, lines[r], CREAM);
  show_rows(HEADER_H, LCD_H);
}

static void clear_page(void) {
  memset(lines, 0, sizeof(lines));
  cur_row = cur_col = 0;
  draw_all_lines();
}

// Move to the next line. If we're on the last line, scroll everything up one.
static void new_line(void) {
  cur_col = 0;
  if (cur_row < ROWS - 1) {
    cur_row++;
    return;
  }
  memmove(lines[0], lines[1], sizeof(lines[0]) * (ROWS - 1));
  memset(lines[ROWS - 1], 0, sizeof(lines[0]));
  draw_all_lines();
}

// Add one character to the page, wrapping whole words onto the next line.
static void put_char(char c) {
  if (c == '\n') {
    if (cur_col > 0) new_line();
    return;
  }
  if (c < 32 || c > 126) return;            // skip anything the font can't draw
  if (c == ' ' && cur_col == 0) return;     // no spaces at the start of a line

  if (cur_col == COLS) {
    // The line is full. Find the start of the word we're in the middle of
    // and move that word down to the next line.
    char *line = lines[cur_row];
    char *space = strrchr(line, ' ');
    char word[COLS + 1] = "";
    if (c != ' ' && space != NULL) {
      strcpy(word, space + 1);
      *space = '\0';
      draw_line(cur_row);
    }
    new_line();
    strcpy(lines[cur_row], word);
    cur_col = strlen(word);
    if (c == ' ') return;
  }
  lines[cur_row][cur_col++] = c;
  lines[cur_row][cur_col] = '\0';
}

// The model calls this for every new piece of text (usually one word).
static void on_token(const char *piece, void *user_data) {
  printf("%s", piece);  // still print to the Mac's terminal too
  fflush(stdout);
  int start_row = cur_row;
  for (const char *p = piece; *p; p++) put_char(*p);
  if (cur_row == start_row) draw_line(cur_row);  // (scrolling already redrew everything)
  else { draw_line(start_row); draw_line(cur_row); }
}

static void show_message(const char *const *msg_lines, int n) {
  memset(lines, 0, sizeof(lines));
  for (int i = 0; i < n && i < ROWS; i++) strncpy(lines[i], msg_lines[i], COLS);
  cur_row = cur_col = 0;
  draw_all_lines();
}

static void show_instructions(void) {
  static const char *const msg[] = {
      "",
      "  Press BOOT (below USB)",
      "  to hear a story.",
      "",
      "  Press KEY (above USB)",
      "  to change the theme.",
  };
  show_message(msg, sizeof(msg) / sizeof(msg[0]));
}

// =====================================================================
// Storage and model
// =====================================================================
static void init_spiffs(void) {
  esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false,
  };
  ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

static Transformer transformer;
static Tokenizer tokenizer;
static Sampler sampler;

static void tell_story(void) {
  draw_header("writing...");
  clear_page();

  // A fresh random seed every time, so each story is different.
  sampler.rng_state = ((unsigned long long)esp_random() << 32) | esp_random();
  ESP_LOGI(TAG, "Theme: %s", THEMES[theme].name);
  generate(&transformer, &tokenizer, &sampler, (char *)THEMES[theme].opening,
           MAX_TOKENS, NULL, 0, on_token, NULL);

  draw_header("BOOT: again");
}

// =====================================================================
// Main program
// =====================================================================
void app_main(void) {
  screen_init();
  gpio_set_level(PIN_LCD_BL, 1);
  draw_header("loading...");
  static const char *const loading[] = {"", "  Waking up the storyteller..."};
  show_message(loading, 2);

  init_spiffs();
  build_transformer(&transformer, CONFIG_LLM_MODEL_PATH_INT8);
  build_tokenizer(&tokenizer, CONFIG_LLM_TOKENIZER_PATH_INT8, transformer.config.vocab_size);
  build_sampler(&sampler, transformer.config.vocab_size, TEMPERATURE, TOP_P, esp_random());
  ESP_LOGI(TAG, "Model loaded. Free internal RAM: %u, PSRAM: %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  gpio_config_t in = {
      .pin_bit_mask = (1ULL << PIN_BUTTON_BOOT) | (1ULL << PIN_BUTTON_KEY),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&in);

  draw_header("ready");
  show_instructions();

  int last_boot = 1, last_key = 1;
  while (1) {
    int boot = gpio_get_level(PIN_BUTTON_BOOT);
    int key = gpio_get_level(PIN_BUTTON_KEY);

    if (key == 0 && last_key == 1) {  // KEY just pressed: next theme
      theme = (theme + 1) % NUM_THEMES;
      draw_header("BOOT: start");
    }
    if (boot == 0 && last_boot == 1) {  // BOOT just pressed: tell a story
      tell_story();
      boot = gpio_get_level(PIN_BUTTON_BOOT);  // ignore presses made during the story
      key = gpio_get_level(PIN_BUTTON_KEY);
    }
    last_boot = boot;
    last_key = key;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}
