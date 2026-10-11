#include "babytech_st7796_panel.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <lvgl.h>

#include "babytech_st7796_orientation.h"

namespace babytech::display {
namespace {

constexpr int kScreenWidth = kLandscapeWidth;
constexpr int kScreenHeight = kLandscapeHeight;
// Keep the two DMA draw buffers near their portrait-mode allocation size.
constexpr int kDrawBufferLines = 26;

constexpr uint8_t kExpanderAddress = 0x20;
constexpr uint8_t kExpanderOutputPort0 = 0x02;
constexpr uint8_t kExpanderConfigPort0 = 0x06;
constexpr uint16_t kTouchResetBit = 0x0001;
constexpr uint16_t kLcdResetBit = 0x0002;
constexpr uint16_t kSdChipSelectBit = 0x0004;
constexpr uint16_t kBacklightBit = 0x0008;

constexpr uint8_t kTouchAddress = 0x14;
constexpr uint16_t kTouchProductIdRegister = 0x8140;
constexpr uint16_t kTouchStatusRegister = 0x814E;
constexpr uint16_t kTouchPoint1Register = 0x8150;
constexpr uint16_t kTouchControlRegister = 0x8040;

TwoWire touchWire(1);
esp_lcd_i80_bus_handle_t lcdBus = nullptr;
esp_lcd_panel_io_handle_t lcdIo = nullptr;
uint16_t expanderOutputs = kSdChipSelectBit;
lv_disp_draw_buf_t drawBuffer;
lv_disp_drv_t displayDriver;
lv_indev_drv_t inputDriver;

bool writeExpanderRegister(uint8_t reg, const uint8_t* data, size_t size) {
  Wire.beginTransmission(kExpanderAddress);
  Wire.write(reg);
  Wire.write(data, size);
  return Wire.endTransmission() == 0;
}

bool writeExpanderOutputs() {
  const uint8_t values[] = {
      static_cast<uint8_t>(expanderOutputs & 0xFF),
      static_cast<uint8_t>((expanderOutputs >> 8) & 0xFF),
  };
  return writeExpanderRegister(kExpanderOutputPort0, values, sizeof(values));
}

bool setExpanderOutput(uint16_t bit, bool high) {
  if (high) {
    expanderOutputs |= bit;
  } else {
    expanderOutputs &= ~bit;
  }
  return writeExpanderOutputs();
}

bool initExpander() {
  if (!Wire.begin(2, 1, 400000)) {
    Serial.println("[DisplayPanel] XL9555 I2C bus init failed");
    return false;
  }
  if (!writeExpanderOutputs()) {
    Serial.println("[DisplayPanel] XL9555 not found at 0x20");
    return false;
  }
  const uint8_t configuration[] = {0xF0, 0xFF};
  if (!writeExpanderRegister(kExpanderConfigPort0, configuration,
                             sizeof(configuration))) {
    Serial.println("[DisplayPanel] XL9555 direction setup failed");
    return false;
  }
  setExpanderOutput(kBacklightBit, false);
  setExpanderOutput(kTouchResetBit, false);
  setExpanderOutput(kLcdResetBit, false);
  return true;
}

bool sendLcdCommand(uint8_t command, const uint8_t* params = nullptr,
                    size_t size = 0) {
  return esp_lcd_panel_io_tx_param(lcdIo, command, params, size) == ESP_OK;
}

bool onColorTransferDone(esp_lcd_panel_io_handle_t,
                         esp_lcd_panel_io_event_data_t*, void* userContext) {
  lv_disp_flush_ready(static_cast<lv_disp_drv_t*>(userContext));
  return false;
}

bool initLcdController() {
  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);

  esp_lcd_i80_bus_config_t busConfig = {};
  busConfig.clk_src = LCD_CLK_SRC_PLL160M;
  busConfig.dc_gpio_num = 38;
  busConfig.wr_gpio_num = 45;
  const int dataPins[] = {13, 12, 11, 10, 9, 46, 3, 8,
                          18, 17, 16, 15, 7, 6, 5, 4};
  for (size_t index = 0; index < 16; ++index) {
    busConfig.data_gpio_nums[index] = dataPins[index];
  }
  busConfig.bus_width = 16;
  busConfig.max_transfer_bytes =
      kScreenWidth * kDrawBufferLines * sizeof(lv_color_t);
  busConfig.dma_burst_size = 64;
  if (esp_lcd_new_i80_bus(&busConfig, &lcdBus) != ESP_OK) {
    Serial.println("[DisplayPanel] LCD i80 bus init failed");
    return false;
  }

  esp_lcd_panel_io_i80_config_t ioConfig = {};
  ioConfig.cs_gpio_num = 39;
  ioConfig.pclk_hz = 25 * 1000 * 1000;
  ioConfig.trans_queue_depth = 2;
  ioConfig.dc_levels.dc_idle_level = 0;
  ioConfig.dc_levels.dc_cmd_level = 0;
  ioConfig.dc_levels.dc_dummy_level = 0;
  ioConfig.dc_levels.dc_data_level = 1;
  ioConfig.lcd_cmd_bits = 8;
  ioConfig.lcd_param_bits = 8;
  ioConfig.on_color_trans_done = onColorTransferDone;
  ioConfig.user_ctx = &displayDriver;
  if (esp_lcd_new_panel_io_i80(lcdBus, &ioConfig, &lcdIo) != ESP_OK) {
    Serial.println("[DisplayPanel] LCD panel IO init failed");
    return false;
  }

  setExpanderOutput(kLcdResetBit, true);
  delay(10);
  setExpanderOutput(kLcdResetBit, false);
  delay(50);
  setExpanderOutput(kLcdResetBit, true);
  delay(200);

  sendLcdCommand(0x11);
  delay(120);
  const uint8_t madctl[] = {kLandscapeMadctl};
  const uint8_t pixelFormat[] = {0x55};
  const uint8_t commandSet1[] = {0xC3};
  const uint8_t commandSet2[] = {0x96};
  const uint8_t inversionControl[] = {0x01};
  const uint8_t entryMode[] = {0xC6};
  const uint8_t powerControl1[] = {0x80, 0x45};
  const uint8_t powerControl2[] = {0x13};
  const uint8_t powerControl3[] = {0xA7};
  const uint8_t vcomControl[] = {0x0A};
  const uint8_t outputControl[] = {0x40, 0x8A, 0x00, 0x00,
                                   0x29, 0x19, 0xA5, 0x33};
  const uint8_t positiveGamma[] = {0xD0, 0x08, 0x0F, 0x06, 0x06,
                                   0x33, 0x30, 0x33, 0x47, 0x17,
                                   0x13, 0x13, 0x2B, 0x31};
  const uint8_t negativeGamma[] = {0xD0, 0x0A, 0x11, 0x0B, 0x09,
                                   0x07, 0x2F, 0x33, 0x47, 0x38,
                                   0x15, 0x16, 0x2C, 0x32};
  const uint8_t commandSetOff1[] = {0x3C};
  const uint8_t commandSetOff2[] = {0x69};

  const bool ok =
      sendLcdCommand(0x36, madctl, sizeof(madctl)) &&
      sendLcdCommand(0x3A, pixelFormat, sizeof(pixelFormat)) &&
      sendLcdCommand(0xF0, commandSet1, sizeof(commandSet1)) &&
      sendLcdCommand(0xF0, commandSet2, sizeof(commandSet2)) &&
      sendLcdCommand(0xB4, inversionControl, sizeof(inversionControl)) &&
      sendLcdCommand(0xB7, entryMode, sizeof(entryMode)) &&
      sendLcdCommand(0xC0, powerControl1, sizeof(powerControl1)) &&
      sendLcdCommand(0xC1, powerControl2, sizeof(powerControl2)) &&
      sendLcdCommand(0xC2, powerControl3, sizeof(powerControl3)) &&
      sendLcdCommand(0xC5, vcomControl, sizeof(vcomControl)) &&
      sendLcdCommand(0xE8, outputControl, sizeof(outputControl)) &&
      sendLcdCommand(0xE0, positiveGamma, sizeof(positiveGamma)) &&
      sendLcdCommand(0xE1, negativeGamma, sizeof(negativeGamma)) &&
      sendLcdCommand(0xF0, commandSetOff1, sizeof(commandSetOff1)) &&
      sendLcdCommand(0xF0, commandSetOff2, sizeof(commandSetOff2)) &&
      sendLcdCommand(0x21) && sendLcdCommand(0x29);
  if (!ok) {
    Serial.println("[DisplayPanel] ST7796 initialization failed");
    return false;
  }
  setExpanderOutput(kBacklightBit, true);
  return true;
}

bool touchWrite(uint16_t reg, const uint8_t* data, size_t size) {
  touchWire.beginTransmission(kTouchAddress);
  touchWire.write(static_cast<uint8_t>(reg >> 8));
  touchWire.write(static_cast<uint8_t>(reg & 0xFF));
  touchWire.write(data, size);
  return touchWire.endTransmission() == 0;
}

bool touchRead(uint16_t reg, uint8_t* data, size_t size) {
  touchWire.beginTransmission(kTouchAddress);
  touchWire.write(static_cast<uint8_t>(reg >> 8));
  touchWire.write(static_cast<uint8_t>(reg & 0xFF));
  if (touchWire.endTransmission(false) != 0) return false;
  if (touchWire.requestFrom(kTouchAddress, static_cast<uint8_t>(size)) != size) {
    return false;
  }
  for (size_t index = 0; index < size; ++index) data[index] = touchWire.read();
  return true;
}

bool initTouchController() {
  if (!touchWire.begin(41, 40, 400000)) {
    Serial.println("[DisplayPanel] GT1151 I2C bus init failed");
    return false;
  }
  pinMode(42, INPUT_PULLUP);
  setExpanderOutput(kTouchResetBit, false);
  delay(200);
  setExpanderOutput(kTouchResetBit, true);
  delay(300);

  uint8_t productId[5] = {};
  if (!touchRead(kTouchProductIdRegister, productId, 4)) {
    Serial.println("[DisplayPanel] GT1151 not found at 0x14");
    return false;
  }
  Serial.printf("[DisplayPanel] Touch controller: %.4s\n", productId);
  const uint8_t resetCommand[] = {0x02};
  const uint8_t normalCommand[] = {0x00};
  touchWrite(kTouchControlRegister, resetCommand, sizeof(resetCommand));
  delay(10);
  touchWrite(kTouchControlRegister, normalCommand, sizeof(normalCommand));
  return true;
}

bool readTouchPoint(uint16_t& x, uint16_t& y) {
  uint8_t status = 0;
  if (!touchRead(kTouchStatusRegister, &status, 1)) return false;
  const uint8_t pointCount = status & 0x0F;
  if ((status & 0x80) == 0 || pointCount == 0 || pointCount > 5) {
    if (status & 0x80) {
      const uint8_t clear = 0;
      touchWrite(kTouchStatusRegister, &clear, 1);
    }
    return false;
  }

  uint8_t point[4] = {};
  const bool readOk = touchRead(kTouchPoint1Register, point, sizeof(point));
  const uint8_t clear = 0;
  touchWrite(kTouchStatusRegister, &clear, 1);
  if (!readOk) return false;
  const uint16_t rawX = static_cast<uint16_t>(point[0] | (point[1] << 8));
  const uint16_t rawY = static_cast<uint16_t>(point[2] | (point[3] << 8));
  return portraitTouchToLandscape(rawX, rawY, x, y);
}

void displayFlush(lv_disp_drv_t* driver, const lv_area_t* area,
                  lv_color_t* colors) {
  const uint8_t columns[] = {
      static_cast<uint8_t>(area->x1 >> 8), static_cast<uint8_t>(area->x1),
      static_cast<uint8_t>(area->x2 >> 8), static_cast<uint8_t>(area->x2),
  };
  const uint8_t rows[] = {
      static_cast<uint8_t>(area->y1 >> 8), static_cast<uint8_t>(area->y1),
      static_cast<uint8_t>(area->y2 >> 8), static_cast<uint8_t>(area->y2),
  };
  if (!sendLcdCommand(0x2A, columns, sizeof(columns)) ||
      !sendLcdCommand(0x2B, rows, sizeof(rows))) {
    lv_disp_flush_ready(driver);
    return;
  }
  const size_t pixelCount = static_cast<size_t>(area->x2 - area->x1 + 1) *
                            static_cast<size_t>(area->y2 - area->y1 + 1);
  if (esp_lcd_panel_io_tx_color(lcdIo, 0x2C, colors,
                                pixelCount * sizeof(lv_color_t)) != ESP_OK) {
    lv_disp_flush_ready(driver);
  }
}

void touchReadCallback(lv_indev_drv_t*, lv_indev_data_t* data) {
  static uint16_t lastX = 0;
  static uint16_t lastY = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  if (readTouchPoint(x, y)) {
    lastX = x;
    lastY = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = lastX;
  data->point.y = lastY;
}

}  // namespace

bool BabytechSt7796Panel::begin() {
  lv_init();
  lv_disp_drv_init(&displayDriver);
  if (!initExpander() || !initLcdController() || !initTouchController()) {
    Serial.println("[DisplayPanel] Initialization failed");
    return false;
  }

  auto* buffer1 = static_cast<lv_color_t*>(heap_caps_malloc(
      kScreenWidth * kDrawBufferLines * sizeof(lv_color_t), MALLOC_CAP_DMA));
  auto* buffer2 = static_cast<lv_color_t*>(heap_caps_malloc(
      kScreenWidth * kDrawBufferLines * sizeof(lv_color_t), MALLOC_CAP_DMA));
  if (buffer1 == nullptr || buffer2 == nullptr) {
    Serial.println("[DisplayPanel] LVGL draw buffer allocation failed");
    return false;
  }

  lv_disp_draw_buf_init(&drawBuffer, buffer1, buffer2,
                        kScreenWidth * kDrawBufferLines);
  displayDriver.hor_res = kScreenWidth;
  displayDriver.ver_res = kScreenHeight;
  displayDriver.flush_cb = displayFlush;
  displayDriver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&displayDriver);

  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = touchReadCallback;
  lv_indev_drv_register(&inputDriver);
  return true;
}

}  // namespace babytech::display
