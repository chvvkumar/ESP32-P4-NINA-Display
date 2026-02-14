import json
import requests
import os
from pathlib import Path

# Load the API specification
with open('api-1.json', 'r') as f:
    api_spec = json.load(f)

# Base URL for the device
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
print("=" * 80)
print("ALL GET ENDPOINTS:")
print("=" * 80)
for idx, endpoint in enumerate(get_endpoints, 1):
    print(f"{idx}. {endpoint['path']}")
    print(f"   Summary: {endpoint['summary']}")
    print()

# Define control keywords to exclude
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

print("\n" + "=" * 80)
print(f"STATUS ENDPOINTS TO FETCH ({len(status_endpoints)}):")
print("=" * 80)
for idx, endpoint in enumerate(status_endpoints, 1):
    print(f"{idx}. {endpoint['path']}")
    print(f"   Summary: {endpoint['summary']}")
    print()

# Fetch data from each status endpoint
print("\n" + "=" * 80)
print("FETCHING DATA FROM STATUS ENDPOINTS:")
print("=" * 80)

# Create output directory if it doesn't exist
output_dir = Path('.')
output_dir.mkdir(exist_ok=True)

results = []

for endpoint in status_endpoints:
    path = endpoint['path']
    url = base_url + path
    
    # Create a safe filename from the path
    filename = path.replace('/', '_').strip('_') + '.json'
    filepath = output_dir / filename
    
    print(f"\nFetching: {path}")
    print(f"URL: {url}")
    
    try:
        response = requests.get(url, timeout=10)
        response.raise_for_status()
        
        # Parse the JSON response
        data = response.json()
        
        # Save to file
        with open(filepath, 'w') as f:
            json.dump(data, f, indent=2)
        
        print(f"✓ Success - Saved to: {filename}")
        print(f"  Status Code: {response.status_code}")
        
        results.append({
            'endpoint': path,
            'status': 'success',
            'filename': filename,
            'status_code': response.status_code
        })
        
    except requests.exceptions.RequestException as e:
        print(f"✗ Error: {str(e)}")
        results.append({
            'endpoint': path,
            'status': 'error',
            'error': str(e)
        })

# Save summary
summary = {
    'total_get_endpoints': len(get_endpoints),
    'total_status_endpoints': len(status_endpoints),
    'successful_fetches': sum(1 for r in results if r['status'] == 'success'),
    'failed_fetches': sum(1 for r in results if r['status'] == 'error'),
    'results': results
}

with open('fetch_summary.json', 'w') as f:
    json.dump(summary, f, indent=2)

print("\n" + "=" * 80)
print("SUMMARY:")
print("=" * 80)
print(f"Total GET endpoints: {summary['total_get_endpoints']}")
print(f"Status endpoints identified: {summary['total_status_endpoints']}")
print(f"Successful fetches: {summary['successful_fetches']}")
print(f"Failed fetches: {summary['failed_fetches']}")
print(f"\nSummary saved to: fetch_summary.json")
