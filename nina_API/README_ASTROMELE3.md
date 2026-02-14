# Astromele3 Status Data - Complete Collection

## Summary

Successfully collected comprehensive status data from **astromele3.lan** NINA device.

- **Device**: http://astromele3.lan:1888
- **Collection Date**: February 13, 2026
- **API Version**: 2.2.14
- **NINA Version**: 3.2.0.9001
- **Success Rate**: 50/51 endpoints (98%)

## Files Created

### 1. astromele3_status_combined.json (Main Data File)

A single, comprehensive JSON file containing ALL status data from the device with extensive documentation.

**File Size**: ~7,982 lines  
**Structure**:
```
{
  "_metadata": {
    // Information about the data collection
    // - Device name and URL
    // - Fetch timestamp
    // - API version
    // - Success/failure counts
  },
  
  "_endpoint_categories": {
    // Description of each endpoint category
    // - Application, Equipment, Astronomy, etc.
  },
  
  "endpoints": {
    // All 51 endpoint results
    "endpoint-name": {
      "_endpoint_info": {
        // Complete documentation of the endpoint
        // - Path, URL, Summary, Description
        // - Tags, Method, Purpose
      },
      "_fetch_status": {
        // Result of the fetch operation
        // - Success/failure status
        // - HTTP status code
        // - Timestamp
      },
      "data": {
        // The actual response data
      }
    }
  }
}
```

### 2. astromele3_endpoint_summary.txt (Human-Readable Summary)

A formatted text file with detailed endpoint documentation organized by category.

**File Size**: 473 lines  
**Contents**:
- Header with device info and statistics
- Endpoints grouped by category (Application, Camera, Mount, etc.)
- For each endpoint:
  - Full URL
  - Summary and description
  - Success/failure status
  - HTTP response code

### 3. fetch_astromele3_combined.py (Collection Script)

The Python script used to fetch and document all data.

## Endpoint Categories Collected

### Application Status (6 endpoints)
- ✅ Plugin version and NINA version
- ✅ Plugin settings
- ✅ Current application tab
- ✅ Installed plugins list
- ✅ Application screenshot
- ❌ Application logs (requires parameters)

### Equipment Status (29 endpoints)

#### Camera (4 endpoints)
- Camera information (sensor size, temperature, gain, offset, etc.)
- List of available cameras
- Dew heater status
- USB bandwidth limit

#### Dome (2 endpoints)
- Dome information and connection status
- Available dome devices

#### Filter Wheel (3 endpoints)
- Filter wheel information
- Available filter wheels
- Individual filter information

#### Flat Device (2 endpoints)
- Flat panel information
- Available flat devices

#### Focuser (3 endpoints)
- Focuser position and temperature
- Available focusers
- Last autofocus result

#### Guider (3 endpoints)
- Guider information and status
- Available guiders
- Guiding graph data

#### Mount (3 endpoints)
- Mount position, tracking status
- Available mounts
- Tracking mode information

#### Rotator (2 endpoints)
- Rotator position and angle
- Available rotators

#### Safety Monitor (2 endpoints)
- Safety status
- Available safety monitors

#### Weather (2 endpoints)
- Weather data (temperature, humidity, pressure, etc.)
- Available weather sources

#### Switch (0 endpoints - filtered out as control)

### Astronomy Utilities (1 endpoint)
- ✅ Moon separation calculations

### Framing (1 endpoint)
- ✅ Current framing information

### Imaging (2 endpoints)
- ✅ Prepared image data
- ✅ Image history

### Profile Management (2 endpoints)
- ✅ Current profile settings
- ✅ Horizon data

### Sequence Management (3 endpoints)
- ✅ Sequence JSON structure
- ✅ Complete sequence state
- ✅ List of available sequences

### Events (1 endpoint)
- ✅ Event history

### Livestack (2 endpoints)
- ✅ Livestack status
- ✅ Available stacked images

### Flats Calibration (6 endpoints)
- ✅ Sky flat settings
- ✅ Auto-brightness flat settings
- ✅ Auto-exposure flat settings
- ✅ Trained dark flat configuration
- ✅ Trained flat configuration
- ✅ Overall flats status

## Key Features of This Collection

### 1. Extensive Documentation
Every endpoint includes:
- **Full URL**: Complete HTTP endpoint
- **Summary**: Brief description
- **Detailed Description**: What the endpoint does
- **Category Tags**: Equipment type or function
- **Method**: HTTP method (all GET)
- **Purpose**: Clarifies it's READ-ONLY
- **Fetch Status**: Success/failure with timestamp
- **Response Data**: Complete JSON response

### 2. Safety & Read-Only Operations
- ✅ Only GET requests used
- ✅ No POST/PUT/DELETE operations
- ✅ No control commands executed
- ✅ No equipment state modifications
- ✅ Filtered out all control endpoints:
  - connect/disconnect
  - set/change/add/remove
  - move/slew/park/home
  - start/stop/open/close
  - capture/solve/sync
  
### 3. Single File Convenience
All data in one JSON file makes it easy to:
- Load entire device state at once
- Search across all endpoints
- Compare states over time
- Archive complete snapshots
- Parse programmatically

## Example Data Structure

```json
"equipment/camera/info": {
  "_endpoint_info": {
    "path": "/equipment/camera/info",
    "full_url": "http://astromele3.lan:1888/v2/api/equipment/camera/info",
    "summary": "Information",
    "description": "This endpoint returns relevant information about the camera.",
    "tags": ["Camera"],
    "method": "GET (READ-ONLY)",
    "purpose": "Status/Information retrieval - does not modify equipment state"
  },
  "_fetch_status": {
    "success": true,
    "status_code": 200,
    "fetch_time": "2026-02-13T19:25:57.258902"
  },
  "data": {
    "Response": {
      "Temperature": 0,
      "Gain": 110,
      "XSize": 6248,
      "YSize": 4176,
      "PixelSize": 3.76,
      "CoolerOn": true,
      "CoolerPower": 37,
      // ... complete camera status
    }
  }
}
```

## Failed Endpoint

Only 1 endpoint failed:

**`/application/logs`**
- Error: 400 Bad Request
- Reason: Requires query parameters (log level, number of entries, etc.)
- Not critical - logs can be accessed with proper parameters if needed

## Sample Equipment Data Collected

From **astromele3.lan** we captured:

### Camera
- Sensor: 6248 x 4176 pixels, 3.76µm pixel size
- Type: Monochrome
- Cooling: Active (37% power, at target temp)
- Gain: 110 (range -25 to 700)
- Offset: 50 (range 0 to 240)
- Binning: 1x1, 2x2, 3x3, 4x4 modes available
- State: Idle

### Mount
- Connection status
- Tracking mode
- Current coordinates
- Available mount devices

### Weather
- Temperature, humidity, pressure
- Cloud coverage
- Rain sensor
- Safety status

### And Much More
- Filter wheel position
- Focuser position
- Guiding data
- Sequence progress
- Profile settings
- Event history

## Usage

### Loading the Data in Python
```python
import json

with open('astromele3_status_combined.json', 'r') as f:
    device_data = json.load(f)

# Access metadata
print(f"Device: {device_data['_metadata']['device_name']}")
print(f"Fetch time: {device_data['_metadata']['fetch_timestamp']}")

# Access specific endpoint data
camera_info = device_data['endpoints']['equipment/camera/info']['data']
print(f"Camera temp: {camera_info['Response']['Temperature']}°C")
print(f"Sensor size: {camera_info['Response']['XSize']} x {camera_info['Response']['YSize']}")
```

### Searching Endpoints
```python
# Find all successful endpoints
successful = [
    ep for ep, data in device_data['endpoints'].items()
    if data['_fetch_status']['success']
]

# Find equipment-related endpoints
equipment = [
    ep for ep in device_data['endpoints'].keys()
    if ep.startswith('equipment/')
]
```

## Notes

1. **Timestamp Accuracy**: All data represents a snapshot at the fetch time
2. **Dynamic Data**: Some values (temperatures, positions) change continuously
3. **Re-running**: Can re-run the script anytime to get fresh data
4. **Comparison**: Save multiple snapshots to track changes over time
5. **Safety**: No risk of equipment damage - all operations are read-only

## Re-running the Collection

To fetch fresh data:

```bash
cd nina_API
python fetch_astromele3_combined.py
```

This will overwrite the existing files with new data.

## Device Information

- **Name**: astromele3
- **Type**: NINA Astrophotography Control System
- **API**: Advanced API Plugin v2.2.14
- **NINA**: Version 3.2.0.9001
- **Network**: astromele3.lan:1888

---

**Collection completed successfully with 98% success rate (50/51 endpoints)**
