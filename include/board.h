#pragma once

#include <Arduino.h>
#include <pins_arduino_analog.h>  // PA0..PA7 / PB0..PB1 aliases resolve via PIN_Ax

/*
 * Board: 立创·地阔星 LCKFB-DKX-STM32F103C8T6 (Blue Pill compatible)
 *
 * All application pins are defined here.  If your carrier board is wired
 * differently, only this file needs to change.
 *
 * Reserved / do not use for application I/O:
 *   PA11 / PA12  USB FS D- / D+      (Type-C communication port)
 *   PA13 / PA14  SWDIO / SWCLK       (unused; this project flashes over UART)
 *   PA9  / PA10  USART1 TX / RX      (factory UART ISP: firmware upload)
 *   PC13         on-board LED        (heartbeat only)
 *   PB2          BOOT1
 */

namespace brd {

// Analog inputs -------------------------------------------------------------
constexpr uint8_t PIN_VBAT  = PA0;   // battery pack voltage divider
constexpr uint8_t PIN_VIN   = PA1;   // DC input voltage divider
constexpr uint8_t PIN_VOUT  = PA2;   // DC output voltage divider
constexpr uint8_t PIN_NTC   = PA3;   // 10k NTC, 10k pull-up to 3.3V

// Digital outputs -----------------------------------------------------------
constexpr uint8_t PIN_OUT_EN     = PA4;   // load switch / output enable
constexpr uint8_t PIN_BOOST_EN   = PA5;   // boost converter enable
constexpr uint8_t PIN_CHG_EN     = PA6;   // charger enable
constexpr uint8_t PIN_FAN_EN     = PA7;   // optional fan
constexpr uint8_t PIN_BUZZER     = PA8;   // NPN driver, active high
constexpr uint8_t PIN_AC_CUT_EN  = PB8;   // optional relay/MOSFET to cut AC path

// Digital inputs ------------------------------------------------------------
constexpr uint8_t PIN_CHG_STAT   = PB0;   // charger CHRG, open drain, active low
constexpr uint8_t PIN_CHG_DONE   = PB1;   // charger STDBY/DONE, open drain, active low
constexpr uint8_t PIN_BUTTON     = PB5;   // to GND, internal pull-up
constexpr uint8_t PIN_AC_OK      = PB10;  // input present detect (opto/comparator)

// I2C -----------------------------------------------------------------------
constexpr uint8_t PIN_I2C_SCL    = PB6;
constexpr uint8_t PIN_I2C_SDA    = PB7;

// LEDs ----------------------------------------------------------------------
constexpr uint8_t PIN_LED_AC     = PB12;  // green: input present
constexpr uint8_t PIN_LED_BAT    = PB13;  // orange: on battery
constexpr uint8_t PIN_LED_FAULT  = PB14;  // red: warning/fault
constexpr uint8_t PIN_LED_STATUS = PC13;  // on-board LED, heartbeat

// UART debug (optional) -----------------------------------------------------
// USART1 doubles as the factory ROM bootloader port.  ISP runs before the
// application, so enabling debug output does not conflict with UART flashing.
constexpr uint8_t PIN_DEBUG_TX   = PA9;
constexpr uint8_t PIN_DEBUG_RX   = PA10;

}  // namespace brd

// ---------------------------------------------------------------------------
// Hardware capability / polarity configuration
// ---------------------------------------------------------------------------

// AC presence input.  Recommended: optocoupler/comparator output, active high
// when the DC adapter is present.  If no dedicated pin is fitted, the firmware
// falls back to the input-voltage ADC threshold.
#ifndef UPS_HAS_AC_OK_PIN
#define UPS_HAS_AC_OK_PIN 1
#endif
#ifndef UPS_AC_OK_ACTIVE_HIGH
#define UPS_AC_OK_ACTIVE_HIGH 1
#endif

// Charger status pins.  Many CC/CV modules only expose LED pins; if yours has
// no usable status outputs, set these to 0.  Full charge is then detected from
// voltage + tail current.
#ifndef UPS_HAS_CHG_STAT
#define UPS_HAS_CHG_STAT 1
#endif
#ifndef UPS_HAS_CHG_DONE
#define UPS_HAS_CHG_DONE 1
#endif
#ifndef UPS_CHG_STATUS_ACTIVE_LOW
#define UPS_CHG_STATUS_ACTIVE_LOW 1
#endif

// Optional AC-cut relay/MOSFET, required for a real battery transfer self-test
// (Megatec T / TL commands).  Default off: T commands are refused rather than
// pretending to test the battery.
#ifndef UPS_HAS_AC_CUT
#define UPS_HAS_AC_CUT 0
#endif

// Optional output-side INA219 at 0x41.  Without it, load current is estimated.
#ifndef UPS_HAS_OUTPUT_INA219
#define UPS_HAS_OUTPUT_INA219 0
#endif

// Optional fan.
#ifndef UPS_HAS_FAN
#define UPS_HAS_FAN 0
#endif

// Output driver polarity.  All are "active high" by default.  The hardware
// should pull OUT_EN high by default so the load stays powered while the MCU
// is reset or unpowered.
#ifndef UPS_OUT_EN_ACTIVE_HIGH
#define UPS_OUT_EN_ACTIVE_HIGH 1
#endif
#ifndef UPS_BOOST_EN_ACTIVE_HIGH
#define UPS_BOOST_EN_ACTIVE_HIGH 1
#endif
#ifndef UPS_CHG_EN_ACTIVE_HIGH
#define UPS_CHG_EN_ACTIVE_HIGH 1
#endif
#ifndef UPS_AC_CUT_ACTIVE_HIGH
#define UPS_AC_CUT_ACTIVE_HIGH 1
#endif
#ifndef UPS_BUZZER_ACTIVE_HIGH
#define UPS_BUZZER_ACTIVE_HIGH 1
#endif

// LED wiring: 3.3V -> resistor -> LED -> GPIO (active low) or GPIO -> LED ->
// resistor -> GND (active high).
#ifndef UPS_EXT_LED_ACTIVE_LOW
#define UPS_EXT_LED_ACTIVE_LOW 1
#endif
// 地阔星 revisions differ here; Blue Pill style is active low.  If the
// on-board LED works inverted, flip this one macro.
#ifndef UPS_ONBOARD_LED_ACTIVE_LOW
#define UPS_ONBOARD_LED_ACTIVE_LOW 1
#endif

// Debug UART.  Off by default; enabling it costs a little flash and prints on
// USART1 (PA9/PA10, 115200 8N1).  It never carries the control protocol.
#ifndef UPS_DEBUG_UART
#define UPS_DEBUG_UART 0
#endif
#ifndef UPS_DEBUG_BAUD
#define UPS_DEBUG_BAUD 115200
#endif

// Independent watchdog: 2 s.  Set to 0 only for debugging with a debugger.
#ifndef UPS_WATCHDOG
#define UPS_WATCHDOG 1
#endif

// ---------------------------------------------------------------------------
// Voltage-divider scaling
//
// voltage_at_pin = ADC_mV
// voltage_rail   = ADC_mV * NUM / DEN
//
// Defaults:
//   VBAT : 100k / 33k   -> 133/33   (0..12.6 V -> 0..3.13 V)
//   VIN  : 100k / 15k   -> 115/15   (0..24 V   -> 0..3.13 V)
//   VOUT : 100k / 30k   -> 130/30   (0..13.5 V -> 0..3.12 V)
//
// Use 1% resistors.  Fine calibration is available at runtime with
// XSET vbat_scale=... / vbat_offset=..., etc.
// ---------------------------------------------------------------------------
#ifndef UPS_VBAT_DIV_NUM
#define UPS_VBAT_DIV_NUM 133
#endif
#ifndef UPS_VBAT_DIV_DEN
#define UPS_VBAT_DIV_DEN 33
#endif
#ifndef UPS_VIN_DIV_NUM
#define UPS_VIN_DIV_NUM 115
#endif
#ifndef UPS_VIN_DIV_DEN
#define UPS_VIN_DIV_DEN 15
#endif
#ifndef UPS_VOUT_DIV_NUM
#define UPS_VOUT_DIV_NUM 130
#endif
#ifndef UPS_VOUT_DIV_DEN
#define UPS_VOUT_DIV_DEN 30
#endif

// ADC / NTC constants
constexpr uint32_t ADC_VREF_MV       = 3300;
constexpr uint16_t ADC_MAX_COUNTS    = 4095;
constexpr uint16_t NTC_R25_OHM       = 10000;
constexpr uint16_t NTC_BETA          = 3950;
constexpr uint16_t NTC_SERIES_OHM    = 10000;
// TEMP_INVALID_C_X10 lives in settings.h so pure formatting code can use it.
