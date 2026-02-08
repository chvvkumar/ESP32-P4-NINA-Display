# AllSky WaveShare ESP32-P4-86-Panel

This project provides an ESPHome configuration for the WaveShare ESP32-P4-86-Panel (ESP32-P4 with 4" Touch LCD). It creates a comprehensive dashboard for monitoring an AllSky camera system, NINA imaging sessions, and plant sensors via Home Assistant.

## Features

*   **System Monitoring**: CPU/SSD temperature, Power usage (Voltage, Current, Watts).
*   **Sky Quality**: SQM readings, Star count, Cloud status.
*   **Environment**: Ambient temperature, Humidity, Dew point.
*   **NINA Integration**: Monitor multiple NINA instances (Status, Exposure, HFR, RMS, Stars, Camera Temp).
*   **Plant Monitoring**: Status and moisture levels for various plants.
*   **Theming**: Multiple built-in color themes selectable from Home Assistant.
*   **Touch Interface**: Swipe gestures to switch between pages (Main, NINA 2, NINA 3, Plants).

## Prerequisites

*   **Hardware**: WaveShare ESP32-P4-86-Panel.
*   **Software**:
    *   [ESPHome](https://esphome.io/) (Version 2024.6.0 or newer recommended).
    *   [Home Assistant](https://www.home-assistant.io/).

## Installation

1.  **Clone the repository**:
    ```bash
    git clone https://github.com/your-username/AllSky-WaveShare-ESP32-P4-86-Panel.git
    cd AllSky-WaveShare-ESP32-P4-86-Panel
    ```

2.  **Setup Secrets**:
    Create a `secrets.yaml` file in the root directory with your WiFi and API credentials:
    ```yaml
    wifi_ssid: "YOUR_WIFI_SSID"
    wifi_password: "YOUR_WIFI_PASSWORD"
    api_encryption_key: "YOUR_API_ENCRYPTION_KEY"
    ```

3.  **Customize Entities**:
    Open `common/ha_entity_config.yaml`. This file maps the dashboard's internal IDs to your Home Assistant entity IDs.
    *   Update `entity_cpu_temp`, `entity_sqm`, etc., to match your sensor names in Home Assistant.
    *   Update the NINA and Plant sensor mappings as required.

4.  **Build and Upload**:
    Run ESPHome to compile and upload the firmware to your device.
    ```bash
    esphome run allsky-panel.yaml
    ```
    *(Note: Replace `allsky-panel.yaml` with the actual name of your main configuration file if different).*

## Customization

### Adjusting Ranges and Colors
You can adjust the minimum and maximum values for gauges (like CPU temp, SQM, etc.) in `common/controls.yaml`. These are defined as `number` components and can also be adjusted dynamically via Home Assistant.

### Themes
The dashboard supports several themes (Modern Slate, Midnight Black, Deep Ocean, etc.). You can change the default theme in `common/controls.yaml` under the `theme_selector` select component, or change it at runtime using the exposed Select entity in Home Assistant.

### Adding/Removing Pages
The display logic is handled in `common/display_lvgl.yaml`.
*   **Pages**: Defined under `lvgl: pages:`.
*   **Navigation**: Swipe gestures are handled in `common/hardware_io.yaml` under `touchscreen: on_release:`. If you add or remove pages, update the page index logic there.

## Directory Structure

*   `common/`: Contains all the modular configuration files.
    *   `base_config.yaml`: Core ESPHome settings (WiFi, API, Logger).
    *   `ha_entity_config.yaml`: **User Configurable** - Maps HA entities.
    *   `display_lvgl.yaml`: LVGL UI layout and styles.
    *   `sensors_ha.yaml`: Sensors that pull data from Home Assistant.
    *   `controls.yaml`: Input helpers (numbers, switches) for customization.
    *   `hardware_io.yaml`: Pin definitions for Display, Touch, and Backlight.