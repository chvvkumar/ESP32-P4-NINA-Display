You are an expert in embedded UI design and astrophotography. 

Redesign the LVGL pages on this device to a clear nice lay out that show the following information. Current implementation and design should be discarded if required.

Concentrate on the following:
1. Visually striking design
2. Easy readability
3. All the space on the limited 720x720 screen should be utilized to the maximim without wasting empty space
4. Important metrics should be given visual priority
5. Use animations as much as possible
6. No scroll bards are to be used and pages should completely fit into the above screen without needing to scroll. Widgets should also behave like this.
7. Ensure proper padding so that text doesn't get cut off or overflow

Pages layout:
Page1: Summary of the critical metrics for both NINA instances
Page2: Information about NINA2
Page3: Information about NINA3

Page1: should have the following for both NINA instances:
- Name of the NINA instance from api/profile/show IsActive profile name
- Target name
- Total guiding RMS
- Current filter 
- Exposure number for current filter
- Time remaining for this target

Individual NINA Pages should contain:
- Name of the NINA instance from api/profile/show IsActive profile name
- Target name
- Total guiding RMS
- HFR
- Current filter 
- Exposure number for current filter
- Exposure duration 
- Camera temperature and camera cooler power
- Detected stars 
- Saturated pixels
- How long until meridian flip


ImplementationL
1. Create sub steps and progress docs within the folder 'refactor'
2. Follow these steps and update the progress docs as you go along

API Docs can be found in this folder with examples of real world API responses: C:\Users\Kumar\git\AllSky-WaveShare-ESP32-P4-86-Panel\nina_API