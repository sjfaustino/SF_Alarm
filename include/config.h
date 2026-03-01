#ifndef SF_ALARM_CONFIG_H
#define SF_ALARM_CONFIG_H

#include <Arduino.h>

// ============================================================================
// Hardware Pin Definitions — KC868-A16 v1.6
// ============================================================================

// I2C Bus (ESP32 → PCF8574 expanders)
#define I2C_SDA_PIN         4
#define I2C_SCL_PIN         5
#define I2C_CLOCK_HZ        100000  // 100 kHz standard mode

// PCF8574 I2C Addresses
#define PCF_INPUT_1_ADDR    0x22    // Digital inputs 1–8
#define PCF_INPUT_2_ADDR    0x21    // Digital inputs 9–16
#define PCF_OUTPUT_1_ADDR   0x24    // Relay outputs 1–8
#define PCF_OUTPUT_2_ADDR   0x25    // Relay outputs 9–16

// Analog Inputs (direct ESP32 GPIOs)
#define ANALOG_IN_1_PIN     36      // ADC1_CH0
#define ANALOG_IN_2_PIN     39      // ADC1_CH3
#define ANALOG_IN_3_PIN     34      // ADC1_CH6
#define ANALOG_IN_4_PIN     35      // ADC1_CH7

// RS485 (UART2)
#define RS485_TX_PIN        13
#define RS485_RX_PIN        16
#define RS485_DIR_PIN       14      // Direction control

// Ethernet (LAN8270A) — managed by ETH.h
#define ETH_PHY_ADDR        0
#define ETH_PHY_MDC          23
#define ETH_PHY_MDIO         18
#define ETH_PHY_POWER       -1      // No power pin
#define ETH_PHY_TYPE         ETH_PHY_LAN8720

// On-board LEDs / Buzzer (if available)
// #define STATUS_LED_PIN    2       // Built-in LED (ESP32)

// ============================================================================
// Alarm System Configuration
// ============================================================================

#define MAX_ZONES           16
#define MAX_PHONE_NUMBERS   5
#define MAX_PHONE_LEN       20
#define MAX_ZONE_NAME_LEN   24
#define MAX_PIN_LEN         8

// Input debounce
#define INPUT_DEBOUNCE_MS   50

// Alarm timing (defaults, configurable via NVS)
#define DEFAULT_EXIT_DELAY_S    30
#define DEFAULT_ENTRY_DELAY_S   15
#define DEFAULT_SIREN_DURATION_S 180    // 3 minutes

// SMS polling interval
#define SMS_POLL_INTERVAL_MS     5000   // 5 seconds

// Input scan interval
#define INPUT_SCAN_INTERVAL_MS   20     // 50 Hz scan rate

// ============================================================================
// Network Defaults
// ============================================================================

#define DEFAULT_ROUTER_IP       "192.168.10.1"
#define DEFAULT_ROUTER_USER     "admin"
#define DEFAULT_ROUTER_PASS     "admin"

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RECONNECT_DELAY_MS 5000

// ============================================================================
// Serial CLI
// ============================================================================

#define CLI_BAUD_RATE       115200
#define CLI_MAX_LINE_LEN    128

// ============================================================================
// System
// ============================================================================

#define WATCHDOG_TIMEOUT_S  30
#define NVS_NAMESPACE       "sf_alarm"

// Firmware version
#define FW_VERSION_MAJOR    0
#define FW_VERSION_MINOR    1
#define FW_VERSION_PATCH    0
#define FW_VERSION_STR      "0.1.0"

#endif // SF_ALARM_CONFIG_H
