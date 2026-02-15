# Advanced API 2.2.13.0 documentation

This is the Websocket documentation for the NINA plugin Advanced API.

## Table of Contents

* [Servers](#servers)
  * [production](#production-server)
* [Operations](#operations)
  * [SUB /socket](#sub-socket-operation)
  * [PUB /tppa](#pub-tppa-operation)
  * [SUB /tppa](#sub-tppa-operation)
  * [PUB /mount](#pub-mount-operation)
  * [SUB /mount](#sub-mount-operation)
  * [PUB /filterwheel](#pub-filterwheel-operation)
  * [SUB /filterwheel](#sub-filterwheel-operation)
  * [PUB /rotator](#pub-rotator-operation)
  * [SUB /rotator](#sub-rotator-operation)

## Servers

### `production` Server

* URL: `ws://localhost:1888/v2`
* Protocol: `ws`

## Operations

### SUB `/socket` Operation

*Subscribe to WebSocket events*

WebSocket channel for events

##### Payload

This is a list of all possible events which come without additional data:

* API-CAPTURE-FINISHED
* AUTOFOCUS-FINISHED
* AUTOFOCUS-STARTING
* CAMERA-CONNECTED
* CAMERA-DISCONNECTED
* CAMERA-DOWNLOAD-TIMEOUT
* DOME-CONNECTED
* DOME-DISCONNECTED
* DOME-SHUTTER-CLOSED
* DOME-SHUTTER-OPENED
* DOME-HOMED
* DOME-PARKED
* DOME-STOPPED
* DOME-SLEWED
* DOME-SYNCED
* FILTERWHEEL-CONNECTED
* FILTERWHEEL-DISCONNECTED
* FLAT-CONNECTED
* FLAT-DISCONNECTED
* FLAT-LIGHT-TOGGLED
* FLAT-COVER-OPENED
* FLAT-COVER-CLOSED
* FOCUSER-CONNECTED
* FOCUSER-DISCONNECTED
* FOCUSER-USER-FOCUSED
* GUIDER-CONNECTED
* GUIDER-DISCONNECTED
* GUIDER-START
* GUIDER-STOP
* GUIDER-DITHER
* IMAGE-PREPARED
* MOUNT-CONNECTED
* MOUNT-DISCONNECTED
* MOUNT-BEFORE-FLIP
* MOUNT-AFTER-FLIP
* MOUNT-HOMED
* MOUNT-PARKED
* MOUNT-UNPARKED
* MOUNT-CENTER
* PROFILE-ADDED
* PROFILE-CHANGED
* PROFILE-REMOVED
* ROTATOR-CONNECTED
* ROTATOR-DISCONNECTED
* ROTATOR-SYNCED
* SAFETY-CONNECTED
* SAFETY-DISCONNECTED
* SEQUENCE-STARTING
* SEQUENCE-FINISHED
* STACK-STATUS
* STACK-UPDATED
* SWITCH-CONNECTED
* SWITCH-DISCONNECTED
* WEATHER-CONNECTED
* WEATHER-DISCONNECTED
* ERROR-AF
* ERROR-PLATESOLVE

Example:

```json
{
  "Response": {
    "Event": "API-CAPTURE-FINISHED"
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Image Save Event

##### Payload

```json
{
  "Response": {
    "Event": "IMAGE-SAVE",
    "ImageStatistics": {
      "ExposureTime": 0,
      "Index": 0,
      "Filter": "string",
      "RmsText": "string",
      "Temperature": 0,
      "CameraName": "string",
      "Gain": 0,
      "Offset": 0,
      "Date": "string",
      "TelescopeName": "string",
      "FocalLength": 0,
      "StDev": 0,
      "Mean": 0,
      "Median": 0,
      "Stars": 0,
      "HFR": 0,
      "IsBayered": true,
      "Min": 0,
      "Max": 0,
      "HFRStDev": 0,
      "TargetName": "string",
      "Filename": "string"
    }
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Autofocus point added

##### Payload

```json
{
  "Response": {
    "Event": "AUTOFOCUS-POINT-ADDED",
    "ImageStatistics": {
      "Position": 0,
      "HFR": 0
    }
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Filter Changed Event

##### Payload

```json
{
  "Response": {
    "Event": "FILTERWHEEL-CHANGED",
    "Previous": {
      "Name": "Filter 1",
      "Íd": 1
    },
    "New": {
      "Name": "Filter 2",
      "Íd": 2
    }
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Flat Brightness Changed Event

##### Payload

```json
{
  "Response": {
    "Event": "FLAT-BRIGHTNESS-CHANGED",
    "Previous": 0,
    "New": 100
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Safety Changed Event `<anonymous-message-6>`

##### Payload

```json
{
  "Response": {
    "Event": "SAFETY-CHANGED",
    "IsSafe": true
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Live stack updated (requires Livestack >= 1.0.0.9)

##### Payload

```json
{
  "Response": {
    "Event": "STACK-UPDATED",
    "Filter": "RGB",
    "Target": "M31",
    "IsMonochrome": true,
    "StackCount": 10,
    "RedStackCount": 10,
    "GreenStackCount": 10,
    "BlueStackCount": 10
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Live stack state update(requires Livestack >= 1.0.1.7)

##### Payload

```json
{
  "Response": {
    "Event": "STACK-STATUS",
    "Status": "running"
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message TS Wait Start Event

##### Payload

```json
{
  "Response": {
    "Event": "TS-WAITSTART",
    "WaitStartTime": "string"
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message TS (New) Target Start Event

##### Payload

```json
{
  "Response": {
    "Event": "TS-NEWTARGETSTART",
    "TargetName": "string",
    "ProjectName": "string",
    "Coordinates": {
      "RA": 0,
      "RAString": "string",
      "RADegrees": 0,
      "Dec": 0,
      "DecString": "string",
      "Epoch": "JNOW",
      "DateTime": {
        "Now": "string",
        "UtcNow": "string"
      }
    },
    "Rotation": 0,
    "TargetEndTime": "string"
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Send WebSocket Event Instruction

##### Payload

```json
{
  "Response": "Test event",
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Send Error to WebSocket Trigger

##### Payload

```json
{
  "Response": {
    "Event": "SEQUENCE-ENTITY-FAILED",
    "Entity": "Dew Heater",
    "Error": "Camera not connected"
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Rotator moved

##### Payload

```json
{
  "Response": {
    "Event": "ROTATOR-MOVED",
    "From": 0,
    "To": 100
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Rotator moved mechanical

##### Payload

```json
{
  "Response": {
    "Event": "ROTATOR-MOVED-MECHANICAL",
    "From": 0,
    "To": 100
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```



### PUB `/tppa` Operation

*Start or Stop TPPA*

TPPA WebSocket channel. To use this channel, TPPA >= 2.2.4.1 must be installed. You can specify the TPPA settings in the json payload.

Accepts **one of** the following messages:

#### Message

##### Payload

```json
{
  "Action": "start-alignment", // Possible values: "start-alignment"`, `"stop-alignment"`, `"pause-alignment"`, `"resume-alignment"
  "ManualMode": true,
  "TargetDistance": 0,
  "MoveRate": 0,
  "EastDirection": true,
  "StartFromCurrentPosition": true,
  "AltDegrees": 0,
  "AltMinutes": 0,
  "AltSeconds": 0.1,
  "AzDegrees": 0,
  "AzMinutes": 0,
  "AzSeconds": 0.1,
  "AlignmentTolerance": 0.1,
  "Filter": "string",
  "ExposureTime": 0.1,
  "Binning": 0,
  "Gain": 0,
  "Offset": 0,
  "SearchRadius": 0.1
}
```



### SUB `/tppa` Operation

*Subscribe to TPPA WebSocket events*

TPPA WebSocket channel. To use this channel, TPPA >= 2.2.4.1 must be installed. You can specify the TPPA settings in the json payload.

Accepts **one of** the following messages:

#### Message Alignment Error Response

##### Payload

```json
{
  "Response": {
    "AzimuthError": 0,
    "AltitudeError": 0,
    "TotalError": 0
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Process Confirmation Response

##### Payload

```json
{
  "Response": "started procedure", // possible values: "started procedure"`, `"stopped procedure"`, `"paused procedure"`, `"resumed procedure"
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```


#### Message Progress Response

##### Payload

```json
{
  "Response": {
    "Status": "Moving to next point",
    "Progress": 0.75
  },
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```



### PUB `/mount` Operation

*Move mount axis*

A websocket channel to move the mount axis manually. You will need to resend the command to move the axis periodically about every second, because the server will automatically stop the movement when it hasn't recieved a command for two seconds as a safety measure.

#### Message

##### Payload


```json
{
  "direction": "east",
  "rate": 0.1
}
```



### SUB `/mount` Operation

*Subscribe to mount axis move events*

A websocket channel to move the mount axis manually. You will need to resend the command to move the axis periodically about every second, because the server will automatically stop the movement when it hasn't recieved a command for two seconds as a safety measure.

Accepts **one of** the following messages:

#### Message Mount Axis Move Response

##### Payload

```json
{
  "Response": "Moving", // possible values: "Moving"`, `"Stopped Move"
  "Error": "string",
  "StatusCode": 200,
  "Success": true,
  "Type": "Socket"
}
```



### PUB `/filterwheel` Operation

A websocket channel to interact with the networked filterwheel. To use this, make sure you connect the networked manual filterwheel, not the normal manual filterwheel. This extends the manual filterwheel with network capabilities, so filter changes can be completed remotely.

Interact with the filterwheel. You may need get-target-filter if the client wasn't connected, when the filter change was issued.

#### Message

##### Payload

```json
"get-target-filter" // possible values: "get-target-filter"`, `"filter-changed"
```


### SUB `/filterwheel` Operation

A websocket channel to interact with the networked filterwheel. To use this, make sure you connect the networked manual filterwheel, not the normal manual filterwheel. This extends the manual filterwheel with network capabilities, so filter changes can be completed remotely.

Subscribe to filter changes. When the filter changes, you will receive a message with the new filter name. If you request the target filter, but there isn't a filter change in progress, N/A will be returned. Once the filter change is completed (either in NINA or via the websocket), Change Complete will be returned.

#### Message

##### Payload

```json
"<one of your filters>" // possible values: "<one of your filters>"`, `"N/A"`, `"Change Complete"
```



### PUB `/rotator` Operation

A websocket channel to interact with the networked rotator. To use this, make sure you connect the networked manual rotator, not the normal manual rotator. This extends the manual rotator with network capabilities, so rotations can be completed remotely.

Interact with the rotator. You may need get-target-position if the client wasn't connected, when the rotation was issued.

#### Message

##### Payload


```json
"get-target-position" // possible values: "get-target-position"`, `"rotation-completed"
```



### SUB `/rotator` Operation

A websocket channel to interact with the networked rotator. To use this, make sure you connect the networked manual rotator, not the normal manual rotator. This extends the manual rotator with network capabilities, so rotations can be completed remotely.

Subscribe to rotation changes. When the rotation changes, you will receive a message containing relevant information, like target position and current position. If you request the target position, but there isn't a rotation in progress, N/A will be returned. Once the rotation is completed (either in NINA or via the websocket), Rotation completed will be returned.

#### Message

##### Payload

```json
{
  "Position": 0.1,
  "TargetPosition": 0.1,
  "Rotation": 0.1
}
```

or

```json
{
  "Message": "Rotation completed"
}
```



