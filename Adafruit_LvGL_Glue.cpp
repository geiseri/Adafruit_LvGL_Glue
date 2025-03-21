#include "Adafruit_LvGL_Glue.h"
#include <lvgl.h>

// ARCHITECTURE-SPECIFIC TIMER STUFF ---------------------------------------

// Tick interval for LittlevGL internal timekeeping; 1 to 10 ms recommended
static const int lv_tick_interval_ms = 10;

#if defined(ARDUINO_ARCH_SAMD) // --------------------------------------

// Because of the way timer/counters are paired, and because parallel TFT
// uses timer 2 for write strobe, this needs to use timer 4 or above...
#define TIMER_NUM 4
#define TIMER_ISR TC4_Handler

// Interrupt service routine for zerotimer object
void TIMER_ISR(void) { Adafruit_ZeroTimer::timerHandler(TIMER_NUM); }

// Timer compare match 0 callback -- invokes LittlevGL timekeeper.
static void timerCallback0(void) { lv_tick_inc(lv_tick_interval_ms); }

#elif defined(ESP32) // ------------------------------------------------
// The following preprocessor code segments are based around the LVGL example
// project for ESP32:
// https://github.com/lvgl/lv_port_esp32/blob/master/main/main.c

// Semaphore to handle concurrent calls to LVGL
// If you wish to call *any* lvgl function from other threads/tasks
// on ESP32, wrap the lvgl function calls inside of lvgl_acquire() and
// lvgl_release()
static SemaphoreHandle_t xGuiSemaphore = NULL;
static TaskHandle_t g_lvgl_task_handle;

// Periodic timer handler
// NOTE: We use the IRAM_ATTR here to place this code into RAM rather than flash
static void IRAM_ATTR lv_tick_handler(void *arg) {
  (void)arg;
  lv_tick_inc(lv_tick_interval_ms);
}

// Pinned task used to update the GUI, called by FreeRTOS
static void gui_task(void *args) {
  while (1) {
    // Delay 1 tick (follows lv_tick_interval_ms)
    vTaskDelay(pdMS_TO_TICKS(lv_tick_interval_ms));

    // Try to take the semaphore, call lvgl task handler function on success
    if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
      lv_task_handler();
      xSemaphoreGive(xGuiSemaphore);
    }
  }
}

/**
 * @brief Locks LVGL resource to prevent memory corrupton on ESP32.
 * NOTE: This function MUST be called PRIOR to a LVGL function (`lv_`) call.
 */
void Adafruit_LvGL_Glue::lvgl_acquire(void) {
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  if (g_lvgl_task_handle != task) {
    xSemaphoreTake(xGuiSemaphore, portMAX_DELAY);
  }
}

/**
 * @brief Unlocks LVGL resource to prevent memory corrupton on ESP32.
 * NOTE: This function MUST be called in application code AFTER lvgl_acquire()
 */
void Adafruit_LvGL_Glue::lvgl_release(void) {
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  if (g_lvgl_task_handle != task) {
    xSemaphoreGive(xGuiSemaphore);
  }
}

#elif defined(NRF52_SERIES) // -----------------------------------------

#define TIMER_ID NRF_TIMER4
#define TIMER_IRQN TIMER4_IRQn
#define TIMER_ISR TIMER4_IRQHandler
#define TIMER_FREQ 16000000

extern "C" {
// Timer interrupt service routine
void TIMER_ISR(void) {
  if (TIMER_ID->EVENTS_COMPARE[0]) {
    TIMER_ID->EVENTS_COMPARE[0] = 0;
  }
  lv_tick_inc(lv_tick_interval_ms);
}
}

#endif

// TOUCHSCREEN STUFF -------------------------------------------------------

// STMPE610 calibration for raw touch data
#define TS_MINX 100
#define TS_MAXX 3800
#define TS_MINY 100
#define TS_MAXY 3750

// Same, for ADC touchscreen
#define ADC_XMIN 325
#define ADC_XMAX 750
#define ADC_YMIN 240
#define ADC_YMAX 840

static void touchscreen_read( lv_indev_t *indev_drv,  lv_indev_data_t *data) {
  static lv_coord_t last_x = 0, last_y = 0;
  static uint8_t release_count = 0;

  // Get pointer to glue object from indev user data
  Adafruit_LvGL_Glue *glue = static_cast<Adafruit_LvGL_Glue*>(lv_indev_get_user_data(indev_drv));
  Adafruit_SPITFT *disp = glue->display;

  if (glue->is_adc_touch) {
    TouchScreen *touch = (TouchScreen *)glue->touchscreen;
    TSPoint p = touch->getPoint();
    // Serial.printf("%d %d %d\r\n", p.x, p.y, p.z);
    // Having an issue with spurious z=0 results from TouchScreen lib.
    // Since touch is polled periodically, workaround is to watch for
    // several successive z=0 results, and only then regard it as
    // a release event (otherwise still touched).
    if (p.z < touch->pressureThreshhold) { // A zero-ish value
      release_count += (release_count < 255);
      if (release_count >= 4) {
        
        data->state = LV_INDEV_STATE_REL; // Is REALLY RELEASED
      } else {
        data->state = LV_INDEV_STATE_PR; // Is STILL PRESSED
      }
    } else {
      release_count = 0;               // Reset release counter
      data->state = LV_INDEV_STATE_PR; // Is PRESSED
      switch (glue->display->getRotation()) {
      case 0:
        last_x = map(p.x, ADC_XMIN, ADC_XMAX, 0, disp->width() - 1);
        last_y = map(p.y, ADC_YMAX, ADC_YMIN, 0, disp->height() - 1);
        break;
      case 1:
        last_x = map(p.y, ADC_YMAX, ADC_YMIN, 0, disp->width() - 1);
        last_y = map(p.x, ADC_XMAX, ADC_XMIN, 0, disp->height() - 1);
        break;
      case 2:
        last_x = map(p.x, ADC_XMAX, ADC_XMIN, 0, disp->width() - 1);
        last_y = map(p.y, ADC_YMIN, ADC_YMAX, 0, disp->height() - 1);
        break;
      case 3:
        last_x = map(p.y, ADC_YMIN, ADC_YMAX, 0, disp->width() - 1);
        last_y = map(p.x, ADC_XMIN, ADC_XMAX, 0, disp->height() - 1);
        break;
      }
    }
    data->point.x = last_x; // Last-pressed coordinates
    data->point.y = last_y;
    data->continue_reading = false; // No buffering of ADC touch data
  } else {
    uint8_t fifo; // Number of points in touchscreen FIFO
    bool more = false;
    Adafruit_STMPE610 *touch = (Adafruit_STMPE610 *)glue->touchscreen;
    // Before accessing SPI touchscreen, wait on any in-progress
    // DMA screen transfer to finish (shared bus).
    disp->dmaWait();
    disp->endWrite();
    if ((fifo = touch->bufferSize())) { // 1 or more points await
      data->state = LV_INDEV_STATE_PR;  // Is PRESSED
      TS_Point p = touch->getPoint();
      // Serial.printf("%d %d %d\r\n", p.x, p.y, p.z);
      // On big TFT FeatherWing, raw X axis is flipped??
      if ((glue->display->width() == 480) || (glue->display->height() == 480)) {
        p.x = (TS_MINX + TS_MAXX) - p.x;
      }
      switch (glue->display->getRotation()) {
      case 0:
        last_x = map(p.x, TS_MAXX, TS_MINX, 0, disp->width() - 1);
        last_y = map(p.y, TS_MINY, TS_MAXY, 0, disp->height() - 1);
        break;
      case 1:
        last_x = map(p.y, TS_MINY, TS_MAXY, 0, disp->width() - 1);
        last_y = map(p.x, TS_MINX, TS_MAXX, 0, disp->height() - 1);
        break;
      case 2:
        last_x = map(p.x, TS_MINX, TS_MAXX, 0, disp->width() - 1);
        last_y = map(p.y, TS_MAXY, TS_MINY, 0, disp->height() - 1);
        break;
      case 3:
        last_x = map(p.y, TS_MAXY, TS_MINY, 0, disp->width() - 1);
        last_y = map(p.x, TS_MAXX, TS_MINX, 0, disp->height() - 1);
        break;
      }
      more = (fifo > 1); // true if more in FIFO, false if last point
#if defined(NRF52_SERIES)
      // Not sure what's up here, but nRF doesn't seem to always poll
      // the FIFO size correctly, causing false release events. If it
      // looks like we've read the last point from the FIFO, pause
      // briefly to allow any more FIFO events to pile up. This
      // doesn't seem to be necessary on SAMD or ESP32. ???
      if (!more) {
        delay(50);
      }
#endif
    } else {                            // FIFO empty
      data->state = LV_INDEV_STATE_REL; // Is RELEASED
    }

    data->point.x = last_x; // Last-pressed coordinates
    data->point.y = last_y;
    data->continue_reading = more;
  }
}

// OTHER LITTLEVGL VITALS --------------------------------------------------

#if LV_COLOR_DEPTH != 16
#pragma error("LV_COLOR_DEPTH must be 16")
#endif
// This isn't necessarily true, don't mention it for now. See notes later.
//#if LV_COLOR_16_SWAP != 0
//  #pragma message("Set LV_COLOR_16_SWAP to 0 for best display performance")
//#endif

// Actual RAM usage will be 2X these figures, since using 2 DMA buffers...
#ifdef _SAMD21_
#define LV_BUFFER_ROWS 4 // Don't hog all the RAM on SAMD21
#else
#define LV_BUFFER_ROWS 8 // Most others have a bit more space
#endif

// This is the tick tracker for lvgl, just needs to return ms elapsed.
static uint32_t lv_tick_callback(void)
{
  return millis();
}


// This is the flush function required for LittlevGL screen updates.
// It receives a bounding rect and an array of pixel data (conveniently
// already in 565 format, so the Earth was lucky there).
static void lv_flush_callback(lv_display_t *display_drv, const lv_area_t *area, unsigned char *data) {
  // Get pointer to glue object from indev user data
  Adafruit_LvGL_Glue *glue = static_cast<Adafruit_LvGL_Glue*>(lv_display_get_user_data(display_drv));

  Adafruit_SPITFT *display = glue->display;

  if (!glue->first_frame) {
    display->dmaWait();  // Wait for prior DMA transfer to complete
    display->endWrite(); // End transaction from any prior call
  } else {
    glue->first_frame = false;
  }
  uint32_t width = lv_area_get_width(area);
  uint32_t height = lv_area_get_height(area);
  display->startWrite();
  display->setAddrWindow(area->x1, area->y1, width, height);
  display->writePixels(reinterpret_cast<uint16_t*>(data), width * height, false, LV_BIG_ENDIAN_SYSTEM);
  lv_disp_flush_ready(display_drv);
}

#if (LV_USE_LOG)
// Optional LittlevGL debug print function, writes to Serial if debug is
// enabled when calling glue begin() function.
static void lv_debug(lv_log_level_t level, const char *buf) { (void) level;  Serial.println(buf); }
#endif

// GLUE LIB FUNCTIONS ------------------------------------------------------

// Constructor
/**
 * @brief Construct a new Adafruit_LvGL_Glue::Adafruit_LvGL_Glue object,
 * initializing minimal variables
 *
 */
Adafruit_LvGL_Glue::Adafruit_LvGL_Glue(void) : first_frame(true) {
#if defined(ARDUINO_ARCH_SAMD)
  zerotimer = NULL;
#endif
}

// Destructor
/**
 * @brief Destroy the Adafruit_LvGL_Glue::Adafruit_LvGL_Glue object, freeing any
 * memory previously allocated within this library.
 *
 */
Adafruit_LvGL_Glue::~Adafruit_LvGL_Glue(void) {
#if defined(ARDUINO_ARCH_SAMD)
  delete zerotimer;
#endif
  // Probably other stuff that could be deallocated here
}

// begin() function is overloaded for STMPE610 touch, ADC touch, or none.

// Pass in POINTERS to ALREADY INITIALIZED display & touch objects (user code
// should have previously called corresponding begin() functions and checked
// return states before invoking this),
// they are NOT initialized here. Debug arg is
// touch arg can be NULL (or left off) if using LittlevGL as a passive widget
// display.

/**
 * @brief Configure the glue layer and the underlying LvGL code to use the given
 * TFT display driver instance and touchscreen controller
 *
 * @param tft Pointer to an **already initialized** display object instance
 * @param touch Pointer to an **already initialized** `Adafruit_STMPE610`
 * touchscreen controller object instance
 * @param debug Debug flag to enable debug messages. Only used if LV_USE_LOG is
 * configured in LittleLVGL's lv_conf.h
 * @return LvGLStatus The status of the initialization:
 * * LVGL_OK : Success
 * * LVGL_ERR_TIMER : Failure to set up timers
 * * LVGL_ERR_ALLOC : Failure to allocate memory
 */
LvGLStatus Adafruit_LvGL_Glue::begin(Adafruit_SPITFT *tft,
                                     Adafruit_STMPE610 *touch, bool debug) {
  is_adc_touch = false;
  return begin(tft, (void *)touch, debug);
}
/**
 * @brief Configure the glue layer and the underlying LvGL code to use the given
 * TFT display driver and touchscreen controller instances
 *
 * @param tft Pointer to an **already initialized** display object instance
 * @param touch Pointer to an **already initialized** `TouchScreen` touchscreen
 * controller object instance
 * @param debug Debug flag to enable debug messages. Only used if LV_USE_LOG is
 * configured in LittleLVGL's lv_conf.h
 * @return LvGLStatus The status of the initialization:
 * * LVGL_OK : Success
 * * LVGL_ERR_TIMER : Failure to set up timers
 * * LVGL_ERR_ALLOC : Failure to allocate memory
 */
LvGLStatus Adafruit_LvGL_Glue::begin(Adafruit_SPITFT *tft, TouchScreen *touch,
                                     bool debug) {
  is_adc_touch = true;
  return begin(tft, (void *)touch, debug);
}
/**
 * @brief Configure the glue layer and the underlying LvGL code to use the given
 * TFT display driver and touchscreen controller instances
 *
 * @param tft Pointer to an **already initialized** display object instance
 * @param debug Debug flag to enable debug messages. Only used if LV_USE_LOG is
 * configured in LittleLVGL's lv_conf.h
 * @return LvGLStatus The status of the initialization:
 * * LVGL_OK : Success
 * * LVGL_ERR_TIMER : Failure to set up timers
 * * LVGL_ERR_ALLOC : Failure to allocate memory
 */
LvGLStatus Adafruit_LvGL_Glue::begin(Adafruit_SPITFT *tft, bool debug) {
  return begin(tft, (void *)NULL, debug);
}

LvGLStatus Adafruit_LvGL_Glue::begin(Adafruit_SPITFT *tft, void *touch,
                                     bool debug) {

  lv_init();
#if (LV_USE_LOG)
  if (debug) {
    lv_log_register_print_cb(lv_debug); // Register debug print function
  }
#endif
  lv_tick_set_cb(lv_tick_callback);
  // Allocate LvGL display buffer (x2 because DMA double buffering)
  LvGLStatus status = LVGL_ERR_ALLOC;
#if defined(USE_SPI_DMA)
  lv_pixel_buf.resize(tft->width() * LV_BUFFER_ROWS * 2);
  if(true) {
#else
  lv_pixel_buf.resize(tft->width() * LV_BUFFER_ROWS);
  if(true) {
#endif

    display = tft;
    touchscreen = (void *)touch;

#if defined(ARDUINO_NRF52840_CLUE) || defined(ARDUINO_NRF52840_CIRCUITPLAY) || \
    defined(ARDUINO_SAMD_CIRCUITPLAYGROUND_EXPRESS)
    // ST7789 library (used by CLUE and TFT Gizmo for Circuit Playground
    // Express/Bluefruit) is sort of low-level rigged to a 240x320
    // screen, so this needs to work around that manually...
    lv_display = lv_display_create(240, 240);
#else
    lv_display = lv_display_create(tft->width(), tft->width());
#endif

    lv_display_set_flush_cb(lv_display, lv_flush_callback);
    lv_display_set_user_data(lv_display, this);
    // Initialize LvGL display buffers. The "second half" buffer is only
    // used if USE_SPI_DMA is enabled in Adafruit_GFX.
    lv_display_set_buffers(lv_display, lv_pixel_buf.data(),
#if defined(USE_SPI_DMA)
                          lv_pixel_buf.size(),
#else
                          NULL, // No double-buffering
#endif
                          lv_pixel_buf.size(), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Initialize LvGL input device (touchscreen already started)
    if ((touch)) { // Can also pass NULL if passive widget display
      lv_touchscreen = lv_indev_create();        // Basic init
      lv_indev_set_type(lv_touchscreen, LV_INDEV_TYPE_POINTER); // Is pointer dev
      lv_indev_set_read_cb(lv_touchscreen, touchscreen_read); // Read callback
      lv_indev_set_user_data(lv_touchscreen, this);
    }

    // TIMER SETUP is architecture-specific ----------------------------

#if defined(ARDUINO_ARCH_SAMD) // --------------------------------------

    // status is still ERR_ALLOC until proven otherwise...
    if ((zerotimer = new Adafruit_ZeroTimer(TIMER_NUM))) {
      uint16_t divider = 1;
      uint16_t compare = 0;
      tc_clock_prescaler prescaler = TC_CLOCK_PRESCALER_DIV1;

      status = LVGL_OK; // We're prob good now, but one more test...

      int freq = 1000 / lv_tick_interval_ms;

      if ((freq < (48000000 / 2)) && (freq > (48000000 / 65536))) {
        divider = 1;
        prescaler = TC_CLOCK_PRESCALER_DIV1;
      } else if (freq > (48000000 / 65536 / 2)) {
        divider = 2;
        prescaler = TC_CLOCK_PRESCALER_DIV2;
      } else if (freq > (48000000 / 65536 / 4)) {
        divider = 4;
        prescaler = TC_CLOCK_PRESCALER_DIV4;
      } else if (freq > (48000000 / 65536 / 8)) {
        divider = 8;
        prescaler = TC_CLOCK_PRESCALER_DIV8;
      } else if (freq > (48000000 / 65536 / 16)) {
        divider = 16;
        prescaler = TC_CLOCK_PRESCALER_DIV16;
      } else if (freq > (48000000 / 65536 / 64)) {
        divider = 64;
        prescaler = TC_CLOCK_PRESCALER_DIV64;
      } else if (freq > (48000000 / 65536 / 256)) {
        divider = 256;
        prescaler = TC_CLOCK_PRESCALER_DIV256;
      } else {
        status = LVGL_ERR_TIMER; // Invalid frequency
      }

      if (status == LVGL_OK) {
        compare = (48000000 / divider) / freq;
        // Initialize timer
        zerotimer->configure(prescaler, TC_COUNTER_SIZE_16BIT,
                             TC_WAVE_GENERATION_MATCH_PWM);
        zerotimer->setCompare(0, compare);
        zerotimer->setCallback(true, TC_CALLBACK_CC_CHANNEL0, timerCallback0);
        zerotimer->enable(true);
      }
    }

#elif defined(ESP32) // ------------------------------------------------
    // Create a periodic timer to call `lv_tick_handler`
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_handler, 
        .name = "lv_tick_handler"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));

    // Create a new mutex
    xGuiSemaphore = xSemaphoreCreateMutex();
    if (xGuiSemaphore == NULL) {
      return LVGL_ERR_MUTEX; // failure
    }

    // Pin the LVGL gui task to core 1
    // TODO: For ESP32-S2/C3, this will need to be pined to core 0

#ifdef CONFIG_IDF_TARGET_ESP32C3
    // For unicore ESP32-x, pin GUI task to core 0
    if (xTaskCreatePinnedToCore(gui_task, "lvgl_gui", 1024 * 8, NULL, 5,
                                &g_lvgl_task_handle, 0) != pdPASS)
      return LVGL_ERR_TASK; // failure
#else
    // For multicore ESP32-x, pin GUI task to core 1 to allow WiFi on core 0
    if (xTaskCreatePinnedToCore(gui_task, "lvgl_gui", 1024 * 8, NULL, 5,
                                &g_lvgl_task_handle, 1) != pdPASS)
      return LVGL_ERR_TASK; // failure
#endif

    // Start timer
    ESP_ERROR_CHECK(
        esp_timer_start_periodic(periodic_timer, lv_tick_interval_ms * 1000));
    status = LVGL_OK;

#elif defined(NRF52_SERIES) // -----------------------------------------

  TIMER_ID->TASKS_STOP = 1;               // Stop timer
  TIMER_ID->MODE = TIMER_MODE_MODE_Timer; // Not counter mode
  TIMER_ID->TASKS_CLEAR = 1;
  TIMER_ID->BITMODE = TIMER_BITMODE_BITMODE_16Bit << TIMER_BITMODE_BITMODE_Pos;
  TIMER_ID->PRESCALER = 0; // 1:1 prescale (16 MHz)
  TIMER_ID->INTENSET = TIMER_INTENSET_COMPARE0_Enabled
                       << TIMER_INTENSET_COMPARE0_Pos; // Event 0 int
  TIMER_ID->CC[0] = TIMER_FREQ / (lv_tick_interval_ms * 1000);

  NVIC_DisableIRQ(TIMER_IRQN);
  NVIC_ClearPendingIRQ(TIMER_IRQN);
  NVIC_SetPriority(TIMER_IRQN, 2); // Lower priority than soft device
  NVIC_EnableIRQ(TIMER_IRQN);

  TIMER_ID->TASKS_START = 1; // Start timer

  status = LVGL_OK;

#endif // end timer setup --------------------------------------------------
  }

  if (status != LVGL_OK) {
    lv_pixel_buf.clear();
#if defined(ARDUINO_ARCH_SAMD)
    delete zerotimer;
    zerotimer = NULL;
#endif
  }

  return status;
}