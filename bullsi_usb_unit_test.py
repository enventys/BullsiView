import asyncio
import platform
from bleak import BleakScanner

async def main():
    print("Scanning for BLE devices...")
    # You can specify the adapter if needed, but often not necessary
    # adapter = "hci0" # This is Linux format, Windows uses different identifiers
    # devices = await BleakScanner.discover(adapter=adapter)

    # Default discovery uses the default adapter
    devices = await BleakScanner.discover()

    if not devices:
        print("No devices found.")
        return

    print(f"Found {len(devices)} devices:")
    for device in devices:
        print(f"  Address: {device.address}")
        print(f"  Name: {device.name}")
        print(f"  Details: {device.details}")
        print(f"  Metadata: {device.metadata}")
        print("-" * 20)

if __name__ == "__main__":
    # Handle asyncio event loop setup for different platforms if needed,
    # but default usually works on Windows for simple scripts.
    if platform.system() == "Windows":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

    asyncio.run(main())