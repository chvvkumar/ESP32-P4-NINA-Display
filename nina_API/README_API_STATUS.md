# NINA API Status Endpoint Fetch Results

## Summary

- **Total GET endpoints found**: 154
- **Status endpoints identified**: 51
- **Successfully fetched**: 49
- **Failed fetches**: 2

## Device Information

- **Device URL**: http://astromele2.lan:1888/v2/api/
- **API Version**: 2.2.14

## All GET Endpoints (154 total)

The API contains 154 GET endpoints across the following categories:
- Application (8 endpoints)
- Equipment (Camera, Dome, Filter Wheel, Flat Device, Focuser, Guider, Mount, Rotator, Safety Monitor, Switch, Weather)
- Framing (6 endpoints)
- Image Management (7 endpoints)
- Profile Management (4 endpoints)
- Sequence Management (9 endpoints)
- Livestack (5 endpoints)
- Flats (6 endpoints)
- Astronomy Utilities (1 endpoint)

## Status Endpoints Fetched (49 successful)

### Application Status
- ✅ `version.json` - Plugin version
- ✅ `version_nina.json` - NINA application version
- ✅ `plugin_settings.json` - Plugin settings
- ✅ `application_get-tab.json` - Current tab information
- ✅ `application_plugins.json` - Installed plugins
- ❌ `application/logs` - Failed (400 Bad Request - requires parameters)
- ✅ `application_screenshot.json` - Application screenshot data

### Equipment Status

#### Camera
- ✅ `equipment_camera_info.json` - Camera information
- ✅ `equipment_camera_list-devices.json` - Available cameras
- ✅ `equipment_camera_dew-heater.json` - Dew heater status
- ✅ `equipment_camera_usb-limit.json` - USB bandwidth limit

#### Dome
- ✅ `equipment_dome_info.json` - Dome information
- ✅ `equipment_dome_list-devices.json` - Available domes

#### Filter Wheel
- ✅ `equipment_filterwheel_info.json` - Filter wheel information
- ✅ `equipment_filterwheel_list-devices.json` - Available filter wheels
- ✅ `equipment_filterwheel_filter-info.json` - Filter information

#### Flat Device
- ✅ `equipment_flatdevice_info.json` - Flat device information
- ✅ `equipment_flatdevice_list-devices.json` - Available flat devices

#### Focuser
- ✅ `equipment_focuser_info.json` - Focuser information
- ✅ `equipment_focuser_list-devices.json` - Available focusers
- ✅ `equipment_focuser_last-af.json` - Last autofocus result

#### Guider
- ✅ `equipment_guider_info.json` - Guider information
- ✅ `equipment_guider_list-devices.json` - Available guiders
- ✅ `equipment_guider_graph.json` - Guiding graph data

#### Mount
- ✅ `equipment_mount_info.json` - Mount information
- ✅ `equipment_mount_list-devices.json` - Available mounts
- ✅ `equipment_mount_tracking.json` - Tracking mode status

#### Rotator
- ✅ `equipment_rotator_info.json` - Rotator information
- ✅ `equipment_rotator_list-devices.json` - Available rotators

#### Safety Monitor
- ✅ `equipment_safetymonitor_info.json` - Safety monitor information
- ✅ `equipment_safetymonitor_list-devices.json` - Available safety monitors

#### Weather
- ✅ `equipment_weather_info.json` - Weather information
- ✅ `equipment_weather_list-devices.json` - Available weather sources

### Astronomy Utilities
- ✅ `astro-util_moon-separation.json` - Moon separation data

### Framing
- ✅ `framing_info.json` - Framing information

### Image Management
- ❌ `prepared-image` - Failed (Empty response - no prepared image available)
- ✅ `image-history.json` - Image history

### Profile Management
- ✅ `profile_show.json` - Current profile information
- ✅ `profile_horizon.json` - Horizon data

### Sequence Management
- ✅ `sequence_json.json` - Sequence JSON data
- ✅ `sequence_state.json` - Complete sequence state
- ✅ `sequence_list-available.json` - Available sequences

### Event History
- ✅ `event-history.json` - Event history

### Livestack
- ✅ `livestack_status.json` - Livestack status
- ✅ `livestack_image_available.json` - Available stacked images

### Flats
- ✅ `flats_skyflat.json` - Sky flat settings
- ✅ `flats_auto-brightness.json` - Auto brightness flat settings
- ✅ `flats_auto-exposure.json` - Auto exposure flat settings
- ✅ `flats_trained-dark-flat.json` - Trained dark flat settings
- ✅ `flats_trained-flat.json` - Trained flat settings
- ✅ `flats_status.json` - Flats status

## Failed Endpoints

### 1. /application/logs
- **Error**: 400 Bad Request
- **Reason**: This endpoint likely requires query parameters (e.g., log level, date range)

### 2. /prepared-image
- **Error**: Empty response
- **Reason**: No prepared image is currently available in NINA

## Files Generated

All status data has been saved to individual JSON files in the `nina_API` directory:
- 49 JSON files containing endpoint responses
- `fetch_summary.json` - Detailed fetch results
- `fetch_status_endpoints.py` - Python script used to fetch the data

## Control Endpoints (Not Fetched)

The following types of endpoints were intentionally **NOT** fetched as they are control endpoints that modify equipment state:
- Connect/Disconnect commands
- Set/Change commands
- Move/Slew commands
- Start/Stop commands
- Park/Unpark/Home commands
- Sync commands
- Capture commands
- Auto-focus commands
- And other action-triggering endpoints

## Notes

- All data was fetched from: `http://astromele2.lan:1888/v2/api/`
- Only GET requests were used (no POST/PUT/DELETE)
- Only status/information endpoints were queried
- No control commands were sent to the device
- Timestamps in the responses represent the state at the time of fetch
