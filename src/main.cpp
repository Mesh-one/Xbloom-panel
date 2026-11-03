// xBloom Panel firmware for the Elecrow CrowPanel Advance 4.3" (V1.1-V1.3).
// Display/touch bring-up follows Elecrow's own PlatformIO example for this board.
#include <Arduino.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

#include "LovyanGFX_Driver.h"
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <mbedtls/platform.h>

#include "ble_link.h"
#include "cloud.h"
#include "panel_net.h"
#include "pins_config.h"
#include "ui.h"

static LGFX gfx;  // used for the GT911 touch controller only
static esp_lcd_panel_handle_t rgb_panel = nullptr;
static SemaphoreHandle_t vsync_sem = nullptr;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(vsync_sem, &woken);
  return woken == pdTRUE;
}

static void disp_flush(lv_display_t *disp, const lv_area_t *, uint8_t *px) {
  if (lv_display_flush_is_last(disp)) {
    xSemaphoreTake(vsync_sem, 0);
    if (esp_lcd_panel_draw_bitmap(rgb_panel, 0, 0, LCD_H_RES, LCD_V_RES, px) == ESP_OK)
      xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(100));
  }
  lv_display_flush_ready(disp);
}

// ---------------------------------------------------------------------------
// Screen sleep: dim after 1 min idle; after 3 min show a dim clock (or go dark
// if the clock is off, the time isn't known yet, or it's night and "Off at
// night" is on). Never while the machine is working (recipe loaded/brewing,
// grinding, pouring, stopwatch, update installing). Wakes on a touch, a cup on
// the scale, or activity on the machine.
// ---------------------------------------------------------------------------
// Set in Settings > Screen > "Sleep after" (minutes, 0 = never). Dims at 1 min,
// or halfway when sleep is 1 min.
static uint32_t off_after_ms() { const int m = net::pref_sleep_min(); return m <= 0 ? UINT32_MAX : (uint32_t)m * 60000; }
static uint32_t dim_after_ms() { return net::pref_sleep_min() == 1 ? 30000 : 60000; }
static const uint8_t BL_ON = 0, BL_DIM = 122, BL_CLOCK = 184, BL_OFF = 245;  // board MCU: 0 brightest .. 245 off
enum class Screen { On, Dim, Clock, Off };
static Screen screen = Screen::On;
static bool swallow_touch = false;  // the touch that wakes the screen does nothing else

static void backlight(uint8_t level) {
  // Same I2C driver the touch controller uses, so the two never fight over the bus.
  lgfx::i2c::transactionWrite(I2C_NUM_0, 0x30, &level, 1, 400000);
}

static void set_screen(Screen s) {
  if (s == screen) return;
  screen = s;
  ui::show_clock(s == Screen::Clock);
  backlight(s == Screen::On ? BL_ON : s == Screen::Dim ? BL_DIM : s == Screen::Clock ? BL_CLOCK : BL_OFF);
  static const char *names[] = {"on", "dim", "clock", "off"};
  Serial.printf("[screen] %s\n", names[(int)s]);
}

static void wake_screen() {
  lv_display_trigger_activity(nullptr);
  set_screen(Screen::On);
}

static void touch_read(lv_indev_t *, lv_indev_data_t *data) {
  lgfx::touch_point_t p;
  const bool pressed = gfx._touch_instance.getTouchRaw(&p, 1);
  if (pressed && screen != Screen::On) {
    swallow_touch = true;
    wake_screen();
  }
  if (!pressed) swallow_touch = false;
  if (pressed && !swallow_touch) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = p.x;
    data->point.y = p.y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void update_screen_power() {
  if (ui::take_wake() || ui::busy()) {
    if (screen != Screen::On) wake_screen();
    else lv_display_trigger_activity(nullptr);
    return;
  }
  const uint32_t idle = lv_display_get_inactive_time(nullptr);
  if (idle < dim_after_ms()) return set_screen(Screen::On);
  if (idle < off_after_ms()) return set_screen(Screen::Dim);
  struct tm t;
  const bool have_time = net::local_time(&t);
  const bool night = have_time && (t.tm_hour >= 23 || t.tm_hour < 6);
  const bool clock = net::pref_clock() && have_time && !(night && net::pref_night_off());
  set_screen(clock ? Screen::Clock : Screen::Off);
}

// ---------------------------------------------------------------------------
// Safe updates: a freshly installed version must run for 30 s before it's
// kept. If it crashes or restarts before that, the bootloader goes back to
// the previous version on its own.
// ---------------------------------------------------------------------------
extern "C" bool verifyRollbackLater() { return true; }  // we confirm it ourselves, below

static void confirm_update_once_healthy() {
  static bool done = false;
  if (done || millis() < 30000) return;
  done = true;
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[update] new version confirmed");
  }
}

static uint32_t tick_ms() { return millis(); }

static bool i2c_present(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static void board_cmd(uint8_t c) {  // the board's helper MCU at 0x30 (backlight etc.)
  Wire.beginTransmission(0x30);
  Wire.write(c);
  Wire.endTransmission();
}

static void bring_up_power_and_touch() {
  Wire.begin(15, 16);
  delay(50);
  for (int tries = 0; tries < 40; ++tries) {
    if (i2c_present(0x30) && i2c_present(0x5D)) break;
    board_cmd(250);  // wake touch
    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
    delay(120);
    pinMode(1, INPUT);
    delay(100);
  }
  board_cmd(0);  // backlight: 0 = brightest ... 245 = off
  if (!gfx._touch_instance.init()) Serial.println("GT911 touch init failed");
}

static void bring_up_display() {
  vsync_sem = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  // 15 MHz (~37 frames/s) instead of Elecrow's 18 MHz: leaves memory bandwidth for Wi-Fi and redraws.
  cfg.timings.pclk_hz = 15000000;
  cfg.timings.h_res = LCD_H_RES;
  cfg.timings.v_res = LCD_V_RES;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 8;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.timings.flags.pclk_idle_high = 1;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 2;
  // 10 lines (as in Espressif's RGB example): the two bounce buffers live in the small
  // internal RAM, which Wi-Fi + TLS also need to reach xBloom.
  cfg.bounce_buffer_size_px = LCD_H_RES * 10;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = GPIO_NUM_40;
  cfg.vsync_gpio_num = GPIO_NUM_41;
  cfg.de_gpio_num = GPIO_NUM_42;
  cfg.pclk_gpio_num = GPIO_NUM_39;
  cfg.disp_gpio_num = -1;
  const int pins[16] = {21, 47, 48, 45, 38, 9, 10, 11, 12, 13, 14, 7, 17, 18, 3, 46};
  for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = pins[i];
  cfg.flags.fb_in_psram = 1;
  cfg.flags.bb_invalidate_cache = 1;

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &rgb_panel));
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = on_vsync;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(rgb_panel, &cbs, nullptr));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(rgb_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(rgb_panel));

  lv_init();
  lv_tick_set_cb(tick_ms);

  const size_t fb_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
  void *fb1 = nullptr, *fb2 = nullptr;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(rgb_panel, 2, &fb1, &fb2));
  memset(fb1, 0, fb_size);
  memset(fb2, 0, fb_size);

  lv_display_t *d = lv_display_create(LCD_H_RES, LCD_V_RES);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(d, disp_flush);
  lv_display_set_buffers(d, fb1, fb2, fb_size, LV_DISPLAY_RENDER_MODE_DIRECT);

  lv_indev_t *in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, touch_read);
}

// TLS (signing in to xBloom) needs ~40 KB of buffers. By default they come from
// the small internal RAM, which the display, Bluetooth and Wi-Fi already fill,
// so the connection failed ("can't reach xBloom"). Put them in the 8 MB PSRAM.
static void *tls_calloc(size_t n, size_t size) {
  return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tls_free(void *p) { heap_caps_free(p); }

void setup() {
  mbedtls_platform_set_calloc_free(tls_calloc, tls_free);  // before anything opens a TLS connection
  Serial.begin(115200);
  Serial.printf("\nxBloom Panel %s starting\n", FW_VERSION);
  bring_up_power_and_touch();
  bring_up_display();
  net::begin();  // before the UI: it loads the saved settings the Settings tab shows
  cloud::begin();
  ui::begin();
  ble_link::begin();
  Serial.printf("[mem] internal free %u, largest block %u, PSRAM free %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("ready");
}

void loop() {
  ui::tick();
  update_screen_power();
  confirm_update_once_healthy();
  lv_timer_handler();
  delay(2);
}
