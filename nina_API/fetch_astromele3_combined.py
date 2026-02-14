import json
import requests
from pathlib import Path
from datetime import datetime

# Load the API specification
with open('api-1.json', 'r') as f:
    api_spec = json.load(f)

# Base URL for astromele3 device
base_url = "http://astromele3.lan:1888/v2/api"

# Extract all GET endpoints
get_endpoints = []
for path, methods in api_spec['paths'].items():
    if 'get' in methods:
        endpoint_info = {
            'path': path,
            'summary': methods['get'].get('summary', ''),
            'description': methods['get'].get('description', ''),
            'tags': methods['get'].get('tags', [])
        }
        get_endpoints.append(endpoint_info)

print(f"Found {len(get_endpoints)} GET endpoints\n")

# Define control keywords to exclude (READ-ONLY endpoints only)
control_keywords = [
    'connect', 'disconnect', 'set-', 'change-', 'add-', 'remove-',
    'cool', 'warm', 'abort', 'move', 'slew', 'park', 'unpark', 'home',
    'start', 'stop', 'open', 'close', 'sync', 'flip', 'reverse',
    'clear', 'rescan', 'switch', 'edit', 'reset', 'load', 'capture',
    'solve', 'determine', 'auto-focus'
]

# Filter status endpoints (exclude control endpoints)
status_endpoints = []
for endpoint in get_endpoints:
    path = endpoint['path']
    # Skip endpoints with parameters (like {index}, {target}, {filter})
    if '{' in path:
        continue
    # Skip if it contains control keywords
    is_control = any(keyword in path.lower() for keyword in control_keywords)
    if not is_control:
        status_endpoints.append(endpoint)

print(f"Identified {len(status_endpoints)} status endpoints to fetch (READ-ONLY)\n")
print("=" * 80)
print("FETCHING DATA FROM ASTROMELE3...")
print("=" * 80)

# Combined data structure with extensive documentation
combined_data = {
    "_metadata": {
        "device_name": "astromele3",
        "device_url": base_url,
        "fetch_timestamp": datetime.now().isoformat(),
        "api_version": api_spec['info']['version'],
        "total_endpoints_in_api": len(get_endpoints),
        "status_endpoints_fetched": len(status_endpoints),
        "note": "This file contains READ-ONLY status data. No control commands were executed."
    },
    "_endpoint_categories": {
        "application": "General application information, version, plugins, settings",
        "equipment": "Connected equipment status (camera, mount, focuser, etc.)",
        "astronomy": "Astronomical calculations and utilities",
        "framing": "Field of view and framing information",
        "imaging": "Image history and statistics",
        "profile": "NINA profile settings and horizon data",
        "sequence": "Sequence state and available sequences",
        "events": "Event history and logs",
        "livestack": "Live stacking status and available stacks",
        "flats": "Flat frame configuration and status"
    },
    "endpoints": {}
}

# Fetch data from each status endpoint
successful_count = 0
failed_count = 0

for endpoint in status_endpoints:
    path = endpoint['path']
    url = base_url + path
    
    print(f"\nFetching: {path}")
    
    # Create endpoint entry with documentation
    endpoint_key = path.strip('/')
    combined_data['endpoints'][endpoint_key] = {
        "_endpoint_info": {
            "path": path,
            "full_url": url,
            "summary": endpoint['summary'],
            "description": endpoint['description'],
            "tags": endpoint['tags'],
            "method": "GET (READ-ONLY)",
            "purpose": "Status/Information retrieval - does not modify equipment state"
        }
    }
    
    try:
        response = requests.get(url, timeout=10)
        response.raise_for_status()
        
        # Parse the JSON response
        data = response.json()
        
        # Add the response data
        combined_data['endpoints'][endpoint_key]['_fetch_status'] = {
            "success": True,
            "status_code": response.status_code,
            "fetch_time": datetime.now().isoformat()
        }
        combined_data['endpoints'][endpoint_key]['data'] = data
        
        print(f"  ✓ Success (HTTP {response.status_code})")
        successful_count += 1
        
    except requests.exceptions.RequestException as e:
        combined_data['endpoints'][endpoint_key]['_fetch_status'] = {
            "success": False,
            "error": str(e),
            "fetch_time": datetime.now().isoformat()
        }
        combined_data['endpoints'][endpoint_key]['data'] = None
        
        print(f"  ✗ Failed: {str(e)}")
        failed_count += 1

# Update metadata with results
combined_data['_metadata']['successful_fetches'] = successful_count
combined_data['_metadata']['failed_fetches'] = failed_count

# Save combined data to file
output_file = Path('astromele3_status_combined.json')
with open(output_file, 'w') as f:
    json.dump(combined_data, f, indent=2)

print("\n" + "=" * 80)
print("SUMMARY:")
print("=" * 80)
print(f"Device: astromele3.lan")
print(f"Total GET endpoints in API: {len(get_endpoints)}")
print(f"Status endpoints fetched: {len(status_endpoints)}")
print(f"Successful: {successful_count}")
print(f"Failed: {failed_count}")
print(f"\nAll data saved to: {output_file}")
print("\n" + "=" * 80)
print("FILE STRUCTURE:")
print("=" * 80)
print("""
The output file contains:
  - _metadata: Information about this data collection
  - _endpoint_categories: Description of endpoint categories
  - endpoints: All fetched data, organized by endpoint path
  
Each endpoint entry includes:
  - _endpoint_info: Full documentation of the endpoint
    * path: API endpoint path
    * full_url: Complete URL used
    * summary: Brief description
    * description: Detailed description
    * tags: Category tags
    * method: HTTP method (GET)
    * purpose: What this endpoint does
  - _fetch_status: Result of the fetch operation
    * success: Whether fetch succeeded
    * status_code: HTTP response code (if successful)
    * error: Error message (if failed)
    * fetch_time: When this was fetched
  - data: The actual response data from the endpoint
""")

# Also create a human-readable summary
summary_file = Path('astromele3_endpoint_summary.txt')
with open(summary_file, 'w', encoding='utf-8') as f:
    f.write("=" * 80 + "\n")
    f.write("ASTROMELE3 STATUS ENDPOINTS - DETAILED SUMMARY\n")
    f.write("=" * 80 + "\n")
    f.write(f"Device: astromele3.lan:1888\n")
    f.write(f"Fetch Time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
    f.write(f"Total Endpoints: {len(status_endpoints)}\n")
    f.write(f"Successful: {successful_count}\n")
    f.write(f"Failed: {failed_count}\n")
    f.write("=" * 80 + "\n\n")
    
    # Group by category
    by_category = {}
    for endpoint in status_endpoints:
        tags = endpoint.get('tags', ['Other'])
        category = tags[0] if tags else 'Other'
        if category not in by_category:
            by_category[category] = []
        by_category[category].append(endpoint)
    
    for category, endpoints in sorted(by_category.items()):
        f.write(f"\n{'=' * 80}\n")
        f.write(f"CATEGORY: {category.upper()}\n")
        f.write(f"{'=' * 80}\n\n")
        
        for ep in sorted(endpoints, key=lambda x: x['path']):
            f.write(f"Endpoint: {ep['path']}\n")
            f.write(f"  Full URL: {base_url}{ep['path']}\n")
            f.write(f"  Summary: {ep['summary']}\n")
            f.write(f"  Description: {ep['description']}\n")
            f.write(f"  Method: GET (READ-ONLY - Status/Information Only)\n")
            
            # Check if it was successful
            endpoint_key = ep['path'].strip('/')
            if endpoint_key in combined_data['endpoints']:
                status = combined_data['endpoints'][endpoint_key]['_fetch_status']
                if status['success']:
                    f.write(f"  Status: ✓ SUCCESS (HTTP {status['status_code']})\n")
                else:
                    f.write(f"  Status: ✗ FAILED ({status.get('error', 'Unknown error')})\n")
            f.write("\n")
    
    f.write("\n" + "=" * 80 + "\n")
    f.write("IMPORTANT NOTES:\n")
    f.write("=" * 80 + "\n")
    f.write("1. All endpoints listed are READ-ONLY GET requests\n")
    f.write("2. No control commands were executed\n")
    f.write("3. No equipment state was modified\n")
    f.write("4. Endpoints with parameters (e.g., {index}) were excluded\n")
    f.write("5. Control endpoints (connect, move, set, etc.) were excluded\n")
    f.write("6. This represents a snapshot of the system state at fetch time\n")

print(f"Human-readable summary saved to: {summary_file}")
print("\n✅ Data collection complete!")
