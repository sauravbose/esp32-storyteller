# ESP32 Storyteller

A tiny language model that writes bedtime stories on a thumb-sized screen, entirely offline.

It runs on a **LilyGO T-Display S3 Touch** (ESP32-S3, 8MB PSRAM). Press a button, and a story types itself out on the screen word by word, written live by a 3-million-parameter model trained on children's stories. No WiFi, no cloud, no computer needed once it's flashed.

> Once upon a time, there was a small bird. The bird was very small, but it was very gentle. One day, the bird saw a big, juicy worm on a vine...

📖 **Full build guide and write-up:** [A Storyteller the Size of My Thumb](LINK-TO-YOUR-SUBSTACK-ARTICLE)

## Features

- Runs a Llama-architecture language model (INT8, ~3M parameters, trained on [TinyStories](https://arxiv.org/abs/2305.07759)) at ~12 tokens/second on the ESP32-S3
- Types stories onto the 1.9" screen at a comfortable reading pace, with word wrap
- 7 story themes, each a different opening line for the model to continue
- Drag a finger up and down on the touchscreen to scroll back through a story
- Works from USB power or a LiPo battery

## Controls

With the screen facing you and the USB port on the **left**:

| Action                  | How                                                    |
| ----------------------- | ------------------------------------------------------ |
| Tell a new story        | Press **BOOT** (the button above the USB port, GPIO 0) |
| Change theme            | Press **KEY** (the button below the USB port, GPIO 14) |
| Scroll the story        | Drag a finger up or down on the screen                 |
| Back to the home screen | Tap the round circle printed on the screen             |

## Hardware

| Item                                                                        | Notes                                                                                                                     |
| --------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| [LilyGO T-Display S3 Touch](https://github.com/Xinyuan-LilyGO/T-Display-S3) | Needs the **16MB flash / 8MB PSRAM** version (ESP32-S3R8).                                                                |
| USB-C cable                                                                 | Must carry data, not just power.                                                                                          |
| 3.7V LiPo battery (optional)                                                | Single cell, **JST 1.25mm** 2-pin plug, with protection circuit. Check polarity against the **+** mark before connecting. |

## Quick start

**1. Install ESP-IDF v5.5.3.** The version matters: ESP-IDF 5.3.x ships GCC 13.2, which crashes with an internal compiler error on the model code.

```bash
brew install cmake ninja dfu-util          # macOS; see Espressif's docs for Linux/Windows
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

If you use conda, run `conda deactivate` first, because ESP-IDF sets up its own Python environment.

**2. Build and flash:**

```bash
. ~/esp/esp-idf/export.sh                  # in every new terminal window
git clone https://github.com/sauravbose/esp32-storyteller.git
cd esp32-storyteller
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

Find your board's port with `ls /dev/cu.*` (macOS) or `ls /dev/ttyACM*` (Linux). Flashing takes a minute or two, because it also writes the 13MB model storage partition. Exit the monitor with `Ctrl-]`.

## Settings

All in the **Story settings** section at the top of [`main/main.c`](main/main.c). Change, then rebuild and flash.

| Setting             | Default | What it does                                                        |
| ------------------- | ------- | ------------------------------------------------------------------- |
| `TEMPERATURE`       | `0.8`   | Lower = safer, more predictable stories; higher = wilder            |
| `TOP_P`             | `0.9`   | Only pick from the most likely next words                           |
| `MAX_TOKENS`        | `512`   | Hard limit on story length (the model's maximum)                    |
| `WORD_DELAY_MS`     | `150`   | Pause after each word, in ms (`0` = full speed)                     |
| `SENTENCE_DELAY_MS` | `400`   | Extra pause after `.` `!` `?`                                       |
| `TOUCH_FLIP`        | `0`     | Set to `1` if dragging scrolls the wrong way                        |
| `TOUCH_DEBUG`       | `1`     | Print each touch's position to the serial monitor (`0` to turn off) |

**Adding a theme:** add a line to the `THEMES` list, a name plus an opening sentence:

```c
{"Robot", "Once upon a time, there was a little robot"},
```

The model can't follow instructions (it only continues text), so a theme is just the start of a story.

## How it works

| Part       | Details                                                                                                                                                                                                     |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Model**  | Llama-architecture transformer: 6 layers, hidden size 192, 6 attention heads (2 KV heads), 4,096-token vocabulary, 512-token context. INT8 weights with one scale per group of 64. Model file: 3.35MB.      |
| **Memory** | The model loads from flash into PSRAM once at power-on (~1.4s). Weights and KV cache live in PSRAM (~4.9MB used); the most-used activations live in fast internal RAM.                                      |
| **Speed**  | ~12.4 tokens/second. Every token reads all the weights from PSRAM, so memory bandwidth, not the CPU, is the limit.                                                                                          |
| **Screen** | 170×320 ST7789 panel on an 8-bit parallel (i80) bus, driven with ESP-IDF's `esp_lcd`. A 109KB RGB565 framebuffer in internal RAM; text uses an 8×16 bitmap font. Only changed lines are sent to the screen. |
| **Touch**  | CST816 or CST328 touch chip over I2C (pins 17/18), auto-detected at startup.                                                                                                                                |

## Repository layout

```
main/
  main.c               The storyteller: screen, text layout, buttons, touch, story settings
  font8x16.h           8x16 bitmap font (generated by tools/make_font.py)
  llm8.c / llm8.h      INT8 inference engine (from esp32-llm, unchanged)
  llm.c  / llm.h       Float32 inference engine (from esp32-llm, not used by default)
spiffs_data/           Model weights and tokenizers, flashed to the storage partition
partitions.csv         Flash layout: 2MB program, 13MB model storage
sdkconfig.defaults     Chip settings: 240MHz, octal PSRAM at 80MHz, 16MB flash
examples/screen_test/  Standalone screen + button test for the T-Display S3
tools/make_font.py     Generates font8x16.h from any monospaced .ttf font
```

### Screen test

A small standalone program that checks the display, colours and buttons without the model. Useful if you're adapting this to a different board:

```bash
cd examples/screen_test
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

You should see "Hello", three colour bars labelled RED, GREEN and BLUE, and the name of each button you press.

### Making a different font

```bash
pip install pillow
python3 tools/make_font.py path/to/AnyMonospaceFont.ttf main/font8x16.h --preview preview.png
```

## Troubleshooting

| Problem                                                     | Fix                                                                                                                       |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `internal compiler error` during build                      | Use ESP-IDF **v5.5.3**, not 5.3.x. Then `idf.py fullclean`, `rm -f sdkconfig sdkconfig.old`, `idf.py set-target esp32s3`. |
| `Failed to connect` when flashing                           | Hold **BOOT**, tap **RST**, release **BOOT**, and flash again. Tap **RST** afterwards.                                    |
| ESP-IDF installer complains about an old Python environment | Run it from a fresh terminal window where `export.sh` hasn't been run.                                                    |
| `idf.py: command not found`                                 | Run `. ~/esp/esp-idf/export.sh` in this terminal window.                                                                  |
| Screen dark on battery                                      | GPIO 15 must be set HIGH at startup (this code does it). Check the battery is charged and the polarity is correct.        |
| Header says "no touch found"                                | The touch chip didn't answer at 0x15 or 0x1A. Paste the serial monitor output into an issue.                              |
| Adapting code from another board                            | Check every pin number against the board's pin list. On this board, GPIO 14 is a button, not a backlight.                 |

## Credits

This project builds on:

- **[esp32-llm](https://github.com/doryiii/esp32-llm)** by Dory: the optimized ESP32-S3 inference engine (`llm8.c`, `llm.c`), the 3.3M model and the build setup. This repository started as a clone of it.
- **[llama2.c](https://github.com/karpathy/llama2.c)** by Andrej Karpathy: the original minimal Llama inference code and the TinyStories models.
- **[TinyStories](https://arxiv.org/abs/2305.07759)** by Ronen Eldan and Yuanzhi Li (Microsoft Research): the dataset.
- **[T-Display-S3](https://github.com/Xinyuan-LilyGO/T-Display-S3)** by LilyGO: the pin map and display setup.
- **[SensorLib](https://github.com/lewisxhe/SensorLib)** by Lewis He: touch chip register details.
- **[DejaVu fonts](https://github.com/dejavu-fonts/dejavu-fonts)**: the source of the bitmap font.

## Licence

The inference engine files from esp32-llm and llama2.c are under the MIT licence; their copyright notices are kept in the source files.
