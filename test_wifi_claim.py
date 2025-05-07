import multiprocessing
import time
from wifi_operations import scan_wifi, connect_to_wifi, known_networks

def claim_device():
    """
    Simulate claiming a device and retrieving its SSID and password.
    For this test, pick the first known network.
    """
    for ssid, password in known_networks.items():
        return ssid, password
    raise Exception("No known networks available for claim.")

def wifi_subprocess(ssid, password):
    """
    Subprocess to handle WiFi connection.
    """
    print(f"[Subprocess] Scanning for SSID: {ssid}")
    available_ssids = scan_wifi()
    print(f"[Subprocess] Available SSIDs: {available_ssids}")
    if ssid in available_ssids:
        print(f"[Subprocess] SSID '{ssid}' found. Attempting to connect...")
        connect_to_wifi(ssid, password)
        print(f"[Subprocess] Connection attempt finished.")
    else:
        print(f"[Subprocess] SSID '{ssid}' not found. Cannot connect.")

def test_wifi_claim_and_connect():
    """
    Main test: claim device, scan, connect, spawn subprocess, report success.
    """
    print("[Main] Claiming device...")
    ssid, password = claim_device()
    print(f"[Main] Claimed SSID: {ssid}, Password: {password}")

    # Spawn subprocess for WiFi operations
    wifi_proc = multiprocessing.Process(target=wifi_subprocess, args=(ssid, password))
    wifi_proc.start()
    print("[Main] WiFi subprocess started. Main process will wait for completion.")

    # Optionally, do other work here or monitor the process
    wifi_proc.join(timeout=30)  # Wait up to 30 seconds for subprocess
    if wifi_proc.is_alive():
        print("[Main] WiFi subprocess still running, terminating...")
        wifi_proc.terminate()
    else:
        print("[Main] WiFi subprocess completed.")

if __name__ == "__main__":
    test_wifi_claim_and_connect()
