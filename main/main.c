/*
 * Story teller for the LilyGO T-Display S3.
 *
 * A tiny language model (3.3M parameters, trained on children's stories)
 * runs on the ESP32-S3 and types each story onto the screen as it writes it.
 *
 *   BOOT button (above the USB port): tell a new story
 *   KEY  button (below the USB port): change the story's theme
 *   Drag a finger up/down on the screen: scroll through the story
 *   Tap the round button on the screen: back to the home screen
 *
 * The model engine (llm8.c) is from Dory's esp32-llm project (MIT licence),
 * itself a port of Andrej Karpathy's llama2.c. This file replaces that
 * project's main.c with a screen and buttons.
 */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
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

#define WORD_DELAY_MS     150  // extra pause after each word, in milliseconds (0 = full speed)
#define SENTENCE_DELAY_MS 400  // extra pause after . ! or ?

#define TOUCH_FLIP 0   // set to 1 if dragging scrolls the wrong way
#define TOUCH_DEBUG 1  // 1 = print where each touch lands (to the Mac terminal)

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

#define PIN_BUTTON_BOOT 0   // above the USB port
#define PIN_BUTTON_KEY  14  // below the USB port

#define PIN_TOUCH_SDA 18  // touch chip's I2C data line
#define PIN_TOUCH_SCL 17  // touch chip's I2C clock line
#define PIN_TOUCH_RST 21  // touch chip's reset line

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
// The page: a header bar plus a 9-line window onto the whole story.
//
// Every line of the story is kept in story_lines[]. The screen shows 9 of
// them at a time, starting at view_top. While the story is being written the
// window follows the newest line; afterwards the buttons move it up and down.
// =====================================================================
#define HEADER_H  20                    // header bar height in pixels
#define TEXT_Y    (HEADER_H + 4)        // where the first text line starts
#define COLS      (LCD_W / FONT_W)      // 40 characters per line
#define ROWS      ((LCD_H - TEXT_Y) / FONT_H)  // 9 lines visible at once
#define MAX_LINES 200                   // a 512-token story is ~60 lines

static char story_lines[MAX_LINES][COLS + 1];  // the whole story, line by line
static int last_line = 0;  // index of the line currently being written
static int cur_col = 0;    // position within that line
static int view_top = 0;   // which story line is shown at the top of the screen
static int theme = 0;

static void draw_header(const char *status) {
  fill_rect(0, 0, LCD_W, HEADER_H, NIGHT);
  draw_text(6, 2, THEMES[theme].name, YELLOW);
  draw_text(LCD_W - 6 - strlen(status) * FONT_W, 2, status, GREY);
  show_rows(0, HEADER_H);
}

// Redraw one story line, if it's inside the visible window.
static void draw_line(int line) {
  int row = line - view_top;
  if (row < 0 || row >= ROWS) return;
  int y = TEXT_Y + row * FONT_H;
  fill_rect(0, y, LCD_W, FONT_H, BLACK);
  draw_text(0, y, story_lines[line], CREAM);
  show_rows(y, y + FONT_H);
}

// Redraw the whole visible window.
static void draw_window(void) {
  fill_rect(0, HEADER_H, LCD_W, LCD_H - HEADER_H, BLACK);
  for (int row = 0; row < ROWS && view_top + row <= last_line; row++)
    draw_text(0, TEXT_Y + row * FONT_H, story_lines[view_top + row], CREAM);
  show_rows(HEADER_H, LCD_H);
}

static void clear_page(void) {
  memset(story_lines, 0, sizeof(story_lines));
  last_line = cur_col = view_top = 0;
  draw_window();
}

// Move the window so the newest line is visible. Returns true if it moved.
static bool follow_newest_line(void) {
  int wanted = last_line - (ROWS - 1);
  if (wanted < 0) wanted = 0;
  if (wanted == view_top) return false;
  view_top = wanted;
  return true;
}

static void new_line(void) {
  cur_col = 0;
  if (last_line < MAX_LINES - 1) {
    last_line++;
  } else {  // story memory full (very unlikely): forget the oldest line
    memmove(story_lines[0], story_lines[1], sizeof(story_lines[0]) * (MAX_LINES - 1));
    memset(story_lines[MAX_LINES - 1], 0, sizeof(story_lines[0]));
  }
}

// Add one character to the story, wrapping whole words onto the next line.
static void put_char(char c) {
  if (c == '\n') {
    if (cur_col > 0) new_line();
    return;
  }
  if (c < 32 || c > 126) return;            // skip anything the font can't draw
  if (c == ' ' && cur_col == 0) return;     // no spaces at the start of a line

  if (cur_col == COLS) {
    // The line is full. Move the word we're in the middle of to the next line.
    char *line = story_lines[last_line];
    char *space = strrchr(line, ' ');
    char word[COLS + 1] = "";
    if (c != ' ' && space != NULL) {
      strcpy(word, space + 1);
      *space = '\0';
      draw_line(last_line);
    }
    new_line();
    strcpy(story_lines[last_line], word);
    cur_col = strlen(word);
    if (c == ' ') return;
  }
  story_lines[last_line][cur_col++] = c;
  story_lines[last_line][cur_col] = '\0';
}

// The model calls this for every new piece of text (usually one word).
static void on_token(const char *piece, void *user_data) {
  printf("%s", piece);  // still print to the Mac's terminal too
  fflush(stdout);

  int start_line = last_line;
  for (const char *p = piece; *p; p++) put_char(*p);
  if (follow_newest_line()) {
    draw_window();  // the window moved: redraw everything
  } else {
    for (int line = start_line; line <= last_line; line++) draw_line(line);
  }

  // Pause so the words appear at a reading pace.
  int delay_ms = WORD_DELAY_MS;
  size_t len = strlen(piece);
  if (len > 0 && strchr(".!?", piece[len - 1])) delay_ms += SENTENCE_DELAY_MS;
  if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

// Show the scroll position in the header, e.g. "lines 10-18 of 34".
static void draw_position(void) {
  char status[48];
  int total = (cur_col > 0) ? last_line + 1 : last_line;  // ignore an empty last line
  if (total <= ROWS) {
    snprintf(status, sizeof(status), "BOOT: new story");
  } else {
    int bottom = view_top + ROWS < total ? view_top + ROWS : total;
    snprintf(status, sizeof(status), "lines %d-%d of %d", view_top + 1, bottom, total);
  }
  draw_header(status);
}

// Scroll the window by `lines` (negative = up). Stops at the top and bottom.
static void scroll_by(int lines) {
  int total = (cur_col > 0) ? last_line + 1 : last_line;
  int max_top = total - ROWS;
  if (max_top < 0) max_top = 0;
  int wanted = view_top + lines;
  if (wanted < 0) wanted = 0;
  if (wanted > max_top) wanted = max_top;
  if (wanted == view_top) return;
  view_top = wanted;
  draw_window();
  draw_position();
}

// After a story ends: show its last full page, without a blank line at the bottom.
static void show_last_page(void) {
  int total = (cur_col > 0) ? last_line + 1 : last_line;
  view_top = total > ROWS ? total - ROWS : 0;
  draw_window();
  draw_position();
}

static void show_message(const char *const *msg, int n) {
  memset(story_lines, 0, sizeof(story_lines));
  for (int i = 0; i < n && i < ROWS; i++) strncpy(story_lines[i], msg[i], COLS);
  last_line = n - 1;
  cur_col = view_top = 0;
  draw_window();
}

static void show_instructions(void) {
  static const char *const msg[] = {
      "",
      "  Press BOOT (above USB)",
      "  to hear a story.",
      "",
      "  Press KEY (below USB)",
      "  to change the theme.",
      "",
      "  Drag your finger up and down",
      "  to scroll through a story.",
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

  show_last_page();
}

// =====================================================================
// Touchscreen
//
// The touch chip talks over I2C, a 2-wire bus for small chips. Boards like
// yours have one of two chips, so we try both, as LilyGO's own example does:
//   CST328 at address 0x1A, or CST816 at address 0x15.
// Register details follow the SensorLib library that LilyGO uses.
// =====================================================================
typedef enum { TOUCH_NONE, TOUCH_CST328, TOUCH_CST816 } touch_chip_t;
static touch_chip_t touch_chip = TOUCH_NONE;
static i2c_master_dev_handle_t touch_dev;

static void touch_init(void) {
  // Restart the touch chip: hold its reset line low, then release it.
  gpio_config_t rst = {.pin_bit_mask = 1ULL << PIN_TOUCH_RST, .mode = GPIO_MODE_OUTPUT};
  gpio_config(&rst);
  gpio_set_level(PIN_TOUCH_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_set_level(PIN_TOUCH_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(150));

  i2c_master_bus_handle_t bus;
  i2c_master_bus_config_t bus_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = PIN_TOUCH_SDA,
      .scl_io_num = PIN_TOUCH_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

  uint8_t address;
  if (i2c_master_probe(bus, 0x1A, 50) == ESP_OK) {
    touch_chip = TOUCH_CST328;
    address = 0x1A;
  } else if (i2c_master_probe(bus, 0x15, 50) == ESP_OK) {
    touch_chip = TOUCH_CST816;
    address = 0x15;
  } else {
    ESP_LOGE(TAG, "No touch chip found (tried 0x1A and 0x15). Scrolling disabled.");
    return;
  }

  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = 400000,
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_config, &touch_dev));

  if (touch_chip == TOUCH_CST328) {
    uint8_t normal_mode[2] = {0xD1, 0x09};  // make sure it's reporting touches
    i2c_master_transmit(touch_dev, normal_mode, 2, 50);
  } else {
    uint8_t no_sleep[2] = {0xFE, 0x01};  // stop it dozing off between touches
    i2c_master_transmit(touch_dev, no_sleep, 2, 50);
  }
  ESP_LOGI(TAG, "Touch chip found: %s at 0x%02X",
           touch_chip == TOUCH_CST328 ? "CST328" : "CST816", address);
}

// What the touchscreen is reporting right now.
typedef enum { NO_TOUCH, TOUCH_SCREEN, TOUCH_HOME } touch_t;
static int last_raw_x, last_raw_y;  // the chip's own numbers, for TOUCH_DEBUG

// Read the touchscreen. For a touch on the display, *y is set to its height
// on the screen (0 = top, 169 = bottom).
static touch_t touch_read(int *y) {
  if (touch_chip == TOUCH_NONE) return NO_TOUCH;
  uint8_t reg = 0x00;
  uint8_t buf[28] = {0};
  int raw_x, raw_y;

  if (touch_chip == TOUCH_CST328) {
    if (i2c_master_transmit_receive(touch_dev, &reg, 1, buf, 28, 50) != ESP_OK) return NO_TOUCH;
    if (buf[6] != 0xAB || buf[0] == 0xAB || buf[0] == 0x00 || buf[5] == 0x80) return NO_TOUCH;
    int fingers = buf[5] & 0x7F;
    if (fingers == 0 || fingers > 5) {
      uint8_t clear[2] = {0x00, 0xAB};
      i2c_master_transmit(touch_dev, clear, 2, 50);
      return NO_TOUCH;
    }
    raw_x = (buf[1] << 4) | (buf[3] >> 4);
    raw_y = (buf[2] << 4) | (buf[3] & 0x0F);
  } else {
    if (i2c_master_transmit_receive(touch_dev, &reg, 1, buf, 13, 50) != ESP_OK) return NO_TOUCH;
    int fingers = buf[2] & 0x0F;
    if (buf[2] == 0xFF || fingers == 0) return NO_TOUCH;
    raw_x = ((buf[3] & 0x0F) << 8) | buf[4];
    raw_y = ((buf[5] & 0x0F) << 8) | buf[6];
  }

  // The touch chip measures in portrait (170 wide x 320 tall), but we use the
  // screen in landscape, so the chip's x is the screen's height (y).
  // The round button sits past the end of the display, so the chip reports
  // it as a position beyond 320.
  last_raw_x = raw_x;
  last_raw_y = raw_y;
  if (raw_y >= LCD_W) return TOUCH_HOME;
  *y = TOUCH_FLIP ? (LCD_H - 1 - raw_x) : raw_x;
  return TOUCH_SCREEN;
}

// Drag-to-scroll: the text follows your finger, one line per 16 pixels.
static bool dragging = false;
static int drag_start_y, drag_start_top;

static bool home_down = false;
static touch_t previous = NO_TOUCH;

static void handle_touch(void) {
  int y = 0;
  touch_t t = touch_read(&y);

#if TOUCH_DEBUG
  if (t != NO_TOUCH && previous == NO_TOUCH)  // log once, when a finger lands
    ESP_LOGI(TAG, "Touch: chip x=%d y=%d -> %s", last_raw_x, last_raw_y,
             t == TOUCH_HOME ? "home button" : "screen");
#endif
  previous = t;

  if (t == TOUCH_HOME) {
    if (!home_down) {  // react once per tap, not for as long as it's held
      home_down = true;
      dragging = false;
      ESP_LOGI(TAG, "Home button tapped");
      draw_header("ready");
      show_instructions();
    }
    return;
  }
  home_down = false;

  if (t == NO_TOUCH) {
    dragging = false;
    return;
  }
  if (!dragging) {  // finger just touched down: remember where
    dragging = true;
    drag_start_y = y;
    drag_start_top = view_top;
    return;
  }
  // Finger moved up -> show later lines; moved down -> earlier lines.
  int wanted_top = drag_start_top + (drag_start_y - y) / FONT_H;
  if (wanted_top != view_top) scroll_by(wanted_top - view_top);
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

  touch_init();

  gpio_config_t in = {
      .pin_bit_mask = (1ULL << PIN_BUTTON_BOOT) | (1ULL << PIN_BUTTON_KEY),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&in);

  draw_header(touch_chip == TOUCH_NONE ? "no touch found" : "ready");
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
      dragging = false;
    }
    last_boot = boot;
    last_key = key;

    handle_touch();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}