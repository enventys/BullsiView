from pywifi import PyWiFi, const, Profile
import time

# List of known networks and their passwords
known_networks = {
#    "Solid State Depot": "carterwashere",
    "descartes": "thenexteclipse",
    "taQlN7": "0k7fxCMw",
    "cvtdBd": "kMoDxVnl"
}

def scan_wifi():
    """Scan for available WiFi networks and return a list of SSIDs."""
    wifi = PyWiFi()
    iface = wifi.interfaces()[0]
    iface.scan()
    time.sleep(2)  # Wait for the scan to complete
    scan_results = iface.scan_results()
    return [network.ssid for network in scan_results]

def connect_to_wifi(ssid, password):
    """Connect to a WiFi network using the provided SSID and password."""
    wifi = PyWiFi()
    iface = wifi.interfaces()[0]
    iface.disconnect()
    time.sleep(1)
    
    profile = Profile()
    profile.ssid = ssid
    profile.auth = const.AUTH_ALG_OPEN
    profile.akm.append(const.AKM_TYPE_WPA2PSK)
    profile.cipher = const.CIPHER_TYPE_CCMP
    profile.key = password
    
    iface.remove_all_network_profiles()
    tmp_profile = iface.add_network_profile(profile)
    iface.connect(tmp_profile)
    time.sleep(10)  # Wait for the connection to establish
    
    if iface.status() == const.IFACE_CONNECTED:
        print(f"Connected to {ssid}")
        return True
    else:
        print(f"Failed to connect to {ssid}")
        return False

if __name__ == "__main__":
    # Demonstrate scanning for WiFi networks
    print("Scanning for WiFi networks...")
    ssids = scan_wifi()
    print("Available networks:", ssids)

    # Attempt to connect to the first known network found
    for ssid in ssids:
        if ssid in known_networks:
            print(f"Connecting to {ssid}...")
            connect_to_wifi(ssid, known_networks[ssid])
            break
