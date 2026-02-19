#!/bin/bash

# OTA Updater Script for ESP Recovery
# This script manages the OTA update process:
# 1. Connects to WiFi access point
# 2. Resets device to OTA updater mode
# 3. Uploads firmware image to specified partition
# 4. Sets the boot partition

# Set strict mode
set -o pipefail

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to handle cleanup and reconnect to original WiFi on error
cleanup_on_error() {
    if [[ $SKIP_WIFI -eq 0 ]] && [[ -n "$ORIGINAL_SSID" ]] && [[ "$CONNECTED_TO_ESP" == "1" ]]; then
        log_info ""
        log_warning "Reconnecting to original WiFi before exit: $ORIGINAL_SSID"
        wifi_connect "$ORIGINAL_SSID" "" "$WIFI_INTERFACE"
        if [[ $? -eq 0 ]]; then
            log_success "Reconnected to original WiFi"
        else
            log_warning "Failed to reconnect to original WiFi"
        fi
    fi
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to safely connect to WiFi with retries
wifi_connect() {
    local ssid="$1"
    local password="$2"
    local interface="$3"
    local max_retries=3
    local retry_count=0
    
    while [[ $retry_count -lt $max_retries ]]; do
        if [[ -z "$password" || "$password" == "" ]]; then
            nmcli device wifi connect "$ssid" ifname "$interface" 2>&1 | grep -v "Error\|error" >/dev/null 2>&1
            local result=$?
        else
            nmcli device wifi connect "$ssid" password "$password" ifname "$interface" 2>&1 | grep -v "Error\|error" >/dev/null 2>&1
            local result=$?
        fi
        
        # Check if connected successfully
        if nmcli -t -f active,ssid,in-use dev wifi | grep "^yes:$ssid:" >/dev/null 2>&1; then
            return 0
        fi
        
        retry_count=$((retry_count + 1))
        if [[ $retry_count -lt $max_retries ]]; then
            log_warning "WiFi connection attempt $retry_count failed, retrying in 2 seconds..."
            sleep 2
        fi
    done
    
    return 1
}

# Function to print usage
print_usage() {
    cat << EOF
Usage: $0 -i <interface> -f <firmware_file> [-s <ssid>] [-p <password>] [-a <ip_address>] [-o <ota_partition>] [--skip-wifi]

Required Arguments:
  -i, --interface      WiFi interface name (e.g., wlan0)
  -f, --file           Path to firmware file to upload

Optional Arguments (with defaults):
  -s, --ssid           WiFi SSID (default: ESP-Recovery)
  -p, --password       WiFi password (default: none)
  -a, --address        IP address of ESP device (default: 192.168.4.1)
  -o, --ota-partition  OTA partition name (default: ota_0)
  --skip-wifi          Skip WiFi connection (for debugging/testing)
  -h, --help           Show this help message

Example:
  $0 -i wlan0 -f firmware.bin
  $0 -i wlan0 -s "MyNetwork" -p "password123" -a 192.168.4.100 -o ota_1 -f firmware.bin
EOF
}

# Set default values
WIFI_SSID="ESP-Recovery"
WIFI_PASSWORD=""
IP_ADDRESS="192.168.4.1"
OTA_PARTITION="ota_0"
SKIP_WIFI=0
CONNECTED_TO_ESP=0
ORIGINAL_SSID=""

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--interface)
            WIFI_INTERFACE="$2"
            shift 2
            ;;
        -s|--ssid)
            WIFI_SSID="$2"
            shift 2
            ;;
        -p|--password)
            WIFI_PASSWORD="$2"
            shift 2
            ;;
        -a|--address)
            IP_ADDRESS="$2"
            shift 2
            ;;
        -o|--ota-partition)
            OTA_PARTITION="$2"
            shift 2
            ;;
        -f|--file)
            FIRMWARE_FILE="$2"
            shift 2
            ;;
        --skip-wifi)
            SKIP_WIFI=1
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Validate required arguments
if [[ -z "$WIFI_INTERFACE" || -z "$FIRMWARE_FILE" ]]; then
    log_error "Missing required arguments: interface (-i) and firmware file (-f) are mandatory"
    print_usage
    exit 1
fi

# Validate firmware file exists
if [[ ! -f "$FIRMWARE_FILE" ]]; then
    log_error "Firmware file not found: $FIRMWARE_FILE"
    exit 1
fi

# Check for required commands
for cmd in curl nmcli; do
    if ! command_exists "$cmd"; then
        log_error "Required command not found: $cmd"
        exit 1
    fi
done

# Verify NetworkManager is running
log_info "Checking NetworkManager status..."
if ! nmcli gen status >/dev/null 2>&1; then
    log_error "NetworkManager is not running or not accessible"
    log_error ""
    log_error "To start NetworkManager, run:"
    log_error "  sudo systemctl start NetworkManager"
    log_error ""
    log_error "To enable it on boot:"
    log_error "  sudo systemctl enable NetworkManager"
    log_error ""
    log_error "Alternatively, if you're using a different network manager (connman, dhcpcd, etc.),"
    log_error "you may need to manually connect to WiFi or use `--skip-wifi` flag if already connected"
    exit 1
fi

log_info "===== ESP Recovery OTA Updater ====="
log_info "WiFi Interface: $WIFI_INTERFACE"
log_info "SSID: $WIFI_SSID"
log_info "IP Address: $IP_ADDRESS"
log_info "OTA Partition: $OTA_PARTITION"
log_info "Firmware File: $FIRMWARE_FILE"
if [[ $SKIP_WIFI -eq 1 ]]; then
    log_info "WiFi: SKIPPED (--skip-wifi flag)"
fi
log_info ""

# Save original WiFi connection
if [[ $SKIP_WIFI -eq 0 ]]; then
    log_info "Saving current WiFi connection..."
    ORIGINAL_SSID=$(nmcli -t -f active,ssid dev wifi 2>/dev/null | grep "^yes" | cut -d: -f2)
    if [[ -z "$ORIGINAL_SSID" ]]; then
        log_warning "No active WiFi connection detected"
    else
        log_info "Saved WiFi connection: $ORIGINAL_SSID"
    fi
    log_info ""
fi

# Step 1: Connect to WiFi
if [[ $SKIP_WIFI -eq 1 ]]; then
    log_info "Skipping WiFi connection (--skip-wifi flag set)"
else
    log_info "Connecting to WiFi network..."
    
    # Check interface status first
    if ! nmcli device status | grep -q "^$WIFI_INTERFACE"; then
        log_error "WiFi interface '$WIFI_INTERFACE' not found"
        log_info "Available interfaces:"
        nmcli device status | tail -n +2
        exit 1
    fi
    
    wifi_connect "$WIFI_SSID" "$WIFI_PASSWORD" "$WIFI_INTERFACE"

    if [[ $? -ne 0 ]]; then
        log_error "Failed to connect to WiFi after multiple attempts"
        log_info ""
        log_info "Scanning for available WiFi networks on $WIFI_INTERFACE..."
        available_networks=$(nmcli dev wifi list --rescan no ifname "$WIFI_INTERFACE" 2>/dev/null | tail -n +2)
        
        if [[ -z "$available_networks" ]]; then
            log_warning "No networks found. Trying to rescan..."
            nmcli dev wifi rescan ifname "$WIFI_INTERFACE" 2>/dev/null
            sleep 2
            available_networks=$(nmcli dev wifi list ifname "$WIFI_INTERFACE" 2>/dev/null | tail -n +2)
        fi
        
        if [[ -n "$available_networks" ]]; then
            log_info "Available networks:"
            echo "$available_networks" | head -20
        else
            log_warning "Could not scan for networks. Interface may be disabled or out of range."
        fi
        
        log_info ""
        log_info "Troubleshooting tips:"
        log_info "1. Verify the WiFi SSID '$WIFI_SSID' is correct and in range"
        log_info "2. Check if a password is required: use -p <password>"
        log_info "3. Ensure interface '$WIFI_INTERFACE' is enabled: nmcli radio wifi on"
        log_info "4. Try manually connecting: nmcli device wifi connect \"$WIFI_SSID\" ifname \"$WIFI_INTERFACE\""
        
        exit 1
    fi

    log_success "Connected to WiFi"
    sleep 2
fi

# Mark that we're now connected to the ESP device network
CONNECTED_TO_ESP=1

# Step 2: Send reset command
log_info "Sending reset command to OTA updater..."
RESET_URL="http://${IP_ADDRESS}/command/resetToOTAUpdater"

HTTP_RESPONSE=$(curl -s -X POST -w "%{http_code}" -o /dev/null "$RESET_URL")

if [[ "$HTTP_RESPONSE" == "404" ]]; then
    log_success "Received 404 - Already in OTA updater mode"
else
    log_info "Received HTTP $HTTP_RESPONSE - Device is being reset to OTA updater"
    
    if [[ "$HTTP_RESPONSE" != "200" && "$HTTP_RESPONSE" != "202" ]]; then
        log_warning "Unexpected HTTP status code: $HTTP_RESPONSE"
    fi
    
    # Step 3: Disconnect from WiFi
    if [[ $SKIP_WIFI -eq 0 ]]; then
        log_info "Disconnecting from WiFi..."
        nmcli device disconnect "$WIFI_INTERFACE"
        
        if [[ $? -ne 0 ]]; then
            log_warning "Failed to disconnect WiFi (may already be disconnected)"
        fi
        
        sleep 1
    fi
    
    # Step 4: Wait 500ms
    log_info "Waiting 500ms for device to bootstrap..."
    sleep 0.5
    
    # Step 5: Reconnect to WiFi
    log_info "Reconnecting to WiFi network..."
    wifi_connect "$WIFI_SSID" "$WIFI_PASSWORD" "$WIFI_INTERFACE"
    
    if [[ $? -ne 0 ]]; then
        log_error "Failed to reconnect to WiFi after multiple attempts"
        exit 1
    fi
    
    log_success "Reconnected to WiFi"
    sleep 2
    
    # Step 6: Send reset command again and verify 404
    log_info "Verifying OTA updater mode with second reset command..."
    HTTP_RESPONSE=$(curl -s -w "%{http_code}" -o /dev/null "$RESET_URL")
    
    if [[ "$HTTP_RESPONSE" != "404" ]]; then
        log_error "Expected 404 response, got $HTTP_RESPONSE - Device may not be in OTA updater mode"
        exit 1
    fi
    
    log_success "Confirmed in OTA updater mode (404 response)"
fi

log_info ""
log_info "Device is in OTA updater mode. Proceeding with firmware upload..."
sleep 1

# Step 7: Upload firmware to partition
log_info "Uploading firmware to partition '$OTA_PARTITION'..."
UPLOAD_URL="http://${IP_ADDRESS}/upload?label=${OTA_PARTITION}"

# Use curl to upload the file - explicitly specify POST method
HTTP_RESPONSE=$(curl -s -X POST -w "%{http_code}" -o /tmp/upload_response.json --data-binary @"$FIRMWARE_FILE" "$UPLOAD_URL")

if [[ "$HTTP_RESPONSE" != "200" ]]; then
    log_error "Upload failed with HTTP status code: $HTTP_RESPONSE"
    if [[ -f /tmp/upload_response.json ]]; then
        log_error "Response: $(cat /tmp/upload_response.json)"
        rm /tmp/upload_response.json
    fi
    cleanup_on_error
    exit 1
fi

RESPONSE_BODY=$(cat /tmp/upload_response.json)
rm /tmp/upload_response.json

if echo "$RESPONSE_BODY" | grep -q '"status":"success"'; then
    log_success "Firmware uploaded successfully"
    log_info "Response: $RESPONSE_BODY"
else
    log_error "Upload response indicates failure"
    log_error "Response: $RESPONSE_BODY"
    cleanup_on_error
    exit 1
fi

log_info ""
log_info "Setting boot partition to '$OTA_PARTITION'..."

# Step 8: Set boot partition
SET_BOOT_URL="http://${IP_ADDRESS}/set_boot"
BOOT_JSON="{\"label\":\"$OTA_PARTITION\"}"

HTTP_RESPONSE=$(curl -s -w "%{http_code}" -o /tmp/boot_response.json -H "Content-Type: application/json" -d "$BOOT_JSON" "$SET_BOOT_URL")

if [[ "$HTTP_RESPONSE" != "200" ]]; then
    log_error "Failed to set boot partition with HTTP status code: $HTTP_RESPONSE"
    if [[ -f /tmp/boot_response.json ]]; then
        log_error "Response: $(cat /tmp/boot_response.json)"
        rm /tmp/boot_response.json
    fi
    cleanup_on_error
    exit 1
fi

RESPONSE_BODY=$(cat /tmp/boot_response.json)
rm /tmp/boot_response.json

if echo "$RESPONSE_BODY" | grep -q '"status":"success"'; then
    log_success "Boot partition set successfully"
    log_info "Response: $RESPONSE_BODY"
else
    log_error "Boot partition set response indicates failure"
    log_error "Response: $RESPONSE_BODY"
    cleanup_on_error
    exit 1
fi

log_info ""
log_info "Sending restart command to device..."

# Step 9: Restart the device
RESET_URL="http://${IP_ADDRESS}/reset"
HTTP_RESPONSE=$(curl -s -w "%{http_code}" -o /dev/null -X POST "$RESET_URL")

if [[ "$HTTP_RESPONSE" == "200" ]]; then
    log_success "Restart command sent"
else
    log_warning "Restart command returned HTTP $HTTP_RESPONSE (may still restart)"
fi

log_info "Waiting for device to restart..."
sleep 3

# Step 10: Reconnect to original WiFi if one was active
if [[ $SKIP_WIFI -eq 0 ]]; then
    if [[ -n "$ORIGINAL_SSID" ]]; then
        log_info "Reconnecting to original WiFi: $ORIGINAL_SSID"
        wifi_connect "$ORIGINAL_SSID" "" "$WIFI_INTERFACE"
        
        if [[ $? -ne 0 ]]; then
            log_warning "Failed to reconnect to original WiFi network after multiple attempts"
        else
            log_success "Reconnected to original WiFi"
            sleep 2
        fi
    else
        log_info "No original WiFi connection to restore"
    fi
fi

log_info ""
log_success "===== OTA Update Complete ====="
log_info "Firmware has been uploaded to partition '$OTA_PARTITION'"
log_info "Boot partition has been set to '$OTA_PARTITION'"
log_info "Device has been restarted and will boot from the new partition"

exit 0
