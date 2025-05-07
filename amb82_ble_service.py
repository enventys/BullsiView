import asyncio
import time
import random
import string
import uuid
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# --- Define UUIDs and Device Name ---
AMB82_SERVICE_UUID = "A4951234-C5B1-4B44-B512-1370F02D74DE"
CLAIM_CHAR_UUID = "A4955678-C5B1-4B44-B512-1370F02D74D1"  # (Read)
HOLD_CHAR_UUID = "A4955678-C5B1-4B44-B512-1370F02D74D2"  # (Write)
SSID_CONTROL_CHAR_UUID = "A4955678-C5B1-4B44-B512-1370F02D74D3"  # (Write)
CURRENT_SSID_CHAR_UUID = "A4955678-C5B1-4B44-B512-1370F02D74D4"  # (Read, Notify)
WIPE_CHAR_UUID = "A4955678-C5B1-4B44-B512-1370F02D74D5"  # (Write)
STATUS_CHAR_UUID = "A4955678-C5B1-4B44-B512-1370F02D74D6"  # (Read, Notify)

DEVICE_NAME = "Bullsi"
TOKEN_EXPIRY_DURATION = 3  # seconds
TOKEN_LENGTH = 8

class AMB82Service:
    """
    A class to manage interactions with the AMB82 BLE service.
    Handles connection, characteristic interactions, and state management.
    """
    def __init__(self, loop=None):
        self.loop = loop or asyncio.get_event_loop()
        self.client = None
        self.address = None
        self.log_messages = []
        self.reset_state()

        # Characteristic handles (will be populated after connection)
        self._claim_char = None
        self._hold_char = None
        self._ssid_control_char = None
        self._current_ssid_char = None
        self._wipe_char = None
        self._status_char = None

        # Notification handlers
        self._status_notification_handler = None
        self._ssid_notification_handler = None

    def _log(self, message):
        """Adds a message to the internal log."""
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
        self.log_messages.append(f"[{timestamp}] {message}")
        print(f"LOG: {message}") # Also print for immediate feedback

    def reset_state(self):
        """Resets the internal state of the service."""
        self._log("Resetting service state.")
        self.active_token = None
        self.last_hold_time = 0
        self.claim_enabled = True
        self.current_ssid = "N/A"
        self.status = "Idle" # Example status, adjust based on actual device feedback

    async def _find_device(self):
        """Scans for the target BLE device."""
        self._log(f"Scanning for device: {DEVICE_NAME}...")
        devices = await BleakScanner.discover()
        for device in devices:
            if device.name == DEVICE_NAME:
                self._log(f"Found device: {device.name} ({device.address})")
                return device.address
        self._log(f"Device {DEVICE_NAME} not found.")
        return None

    async def connect(self):
        """Connects to the BLE device."""
        if self.client and self.client.is_connected:
            self._log("Already connected.")
            return True

        self.address = await self._find_device()
        if not self.address:
            return False

        self.client = BleakClient(self.address, loop=self.loop)
        try:
            self._log(f"Attempting to connect to {self.address}...")
            await self.client.connect()
            self._log("Successfully connected.")

            # Discover services and characteristics
            await self._discover_characteristics()

            # Start notifications if characteristics were found
            if self._status_char:
                await self.start_status_notifications(self._default_status_handler)
            if self._current_ssid_char:
                 await self.start_ssid_notifications(self._default_ssid_handler)

            return True
        except BleakError as e:
            self._log(f"Connection failed: {e}")
            self.client = None
            return False
        except Exception as e:
            # Log the type and representation for better debugging
            self._log(f"An unexpected error occurred during connection. Type: {type(e)}, Repr: {repr(e)}, Message: {e}")
            if self.client and self.client.is_connected:
                await self.client.disconnect()
            self.client = None
            return False

    async def _discover_characteristics(self):
        """Discovers and maps the required characteristics."""
        if not self.client or not self.client.is_connected:
            self._log("Cannot discover characteristics: Not connected.")
            return

        self._log("Discovering services and characteristics...")
        try:
            # Use client.services directly
            svcs = self.client.services
            amb_service = svcs.get_service(AMB82_SERVICE_UUID)

            if not amb_service:
                self._log(f"Service {AMB82_SERVICE_UUID} not found.")
                # Attempt to list all services for debugging
                for service in svcs:
                    self._log(f" Found service: {service.uuid}")
                return

            self._log(f"Found service: {amb_service.uuid}")

            # Map characteristics using the BleakGATTService object
            self._claim_char = amb_service.get_characteristic(CLAIM_CHAR_UUID)
            self._hold_char = amb_service.get_characteristic(HOLD_CHAR_UUID)
            self._ssid_control_char = amb_service.get_characteristic(SSID_CONTROL_CHAR_UUID)
            self._current_ssid_char = amb_service.get_characteristic(CURRENT_SSID_CHAR_UUID)
            self._wipe_char = amb_service.get_characteristic(WIPE_CHAR_UUID)
            self._status_char = amb_service.get_characteristic(STATUS_CHAR_UUID)

            # Log found characteristics
            if self._claim_char: self._log(f" Found Claim Char: {self._claim_char.uuid}")
            if self._hold_char: self._log(f" Found Hold Char: {self._hold_char.uuid}")
            if self._ssid_control_char: self._log(f" Found SSID Control Char: {self._ssid_control_char.uuid}")
            if self._current_ssid_char: self._log(f" Found Current SSID Char: {self._current_ssid_char.uuid}")
            if self._wipe_char: self._log(f" Found Wipe Char: {self._wipe_char.uuid}")
            if self._status_char: self._log(f" Found Status Char: {self._status_char.uuid}")

            if not all([self._claim_char, self._hold_char, self._ssid_control_char,
                        self._current_ssid_char, self._wipe_char, self._status_char]):
                self._log("Warning: Not all expected characteristics were found.")

        except BleakError as e:
            self._log(f"Error discovering characteristics: {e}")
        except Exception as e:
            self._log(f"An unexpected error occurred during characteristic discovery: {e}")


    async def disconnect(self):
        """Disconnects from the BLE device."""
        if self.client and self.client.is_connected:
            self._log("Disconnecting...")
            await self.client.disconnect()
            self._log("Disconnected.")
        else:
            self._log("Not connected.")
        self.client = None
        self.reset_state() # Reset state on disconnect

    def _generate_token(self):
        """Generates a random token."""
        return ''.join(random.choices(string.ascii_letters + string.digits, k=TOKEN_LENGTH))

    def _is_token_valid(self, token):
        """Checks if the provided token is the active one and not expired."""
        if not token or token != self.active_token:
            return False
        if time.time() - self.last_hold_time > TOKEN_EXPIRY_DURATION and TOKEN_EXPIRATION != 0:
            self._log("Token expired.")
            # self.reset_state() # Reset state if token expires? Or just invalidate token?
            self.active_token = None # Invalidate token
            self.claim_enabled = True
            return False
        return True

    # --- Characteristic Interaction Methods ---

    async def claim(self):
        """Attempts to claim control of the device."""
        if not self.client or not self.client.is_connected or not self._claim_char:
            self._log("Claim failed: Not connected or claim characteristic not found.")
            return None
        if not self.claim_enabled:
             self._log("Claim failed: Claiming is currently disabled (likely held by a token).")
             return None
        if self.active_token and not self._is_token_valid(self.active_token): # Check if existing token expired
             self._log("Claiming allowed: Previous token expired.")
             # Proceed to claim

        self._log("Attempting to claim...")
        try:
            # Read the claim characteristic - Device should generate/provide token?
            # Assuming the device returns the token upon reading the claim char
            token_bytes = await self.client.read_gatt_char(self._claim_char.uuid)
            token = token_bytes.decode('utf-8').strip() # Assuming UTF-8 encoding

            if token and len(token) == TOKEN_LENGTH: # Basic validation
                self.active_token = token
                self.last_hold_time = time.time()
                self.claim_enabled = False # Disable further claims until token expires/reset
                self._log(f"Claim successful. Received token: {self.active_token}")
                return self.active_token
            else:
                self._log(f"Claim failed: Invalid token received from device ('{token}').")
                return None
        except BleakError as e:
            self._log(f"Claim failed: BLE error - {e}")
            return None
        except Exception as e:
            self._log(f"Claim failed: Unexpected error - {e}")
            return None


    async def hold(self, token):
        """Sends a hold command with the provided token."""
        if not self.client or not self.client.is_connected or not self._hold_char:
            self._log("Hold failed: Not connected or hold characteristic not found.")
            return False
        if not self._is_token_valid(token):
             self._log(f"Hold failed: Invalid or expired token '{token}'.")
             return False

        self._log(f"Sending hold command with token: {token}...")
        try:
            await self.client.write_gatt_char(self._hold_char.uuid, token.encode('utf-8'), response=True) # Assuming UTF-8
            self.last_hold_time = time.time() # Update hold time on successful write
            self._log("Hold command successful.")
            return True
        except BleakError as e:
            self._log(f"Hold failed: BLE error - {e}")
            return False
        except Exception as e:
            self._log(f"Hold failed: Unexpected error - {e}")
            return False

    async def ssid(self, token, new_ssid=None, password=None):
        """Sends SSID configuration command."""
        if not self.client or not self.client.is_connected or not self._ssid_control_char:
            self._log("SSID command failed: Not connected or SSID control characteristic not found.")
            return False
        if not self._is_token_valid(token):
            self._log(f"SSID command failed: Invalid or expired token '{token}'.")
            return False

        # Construct the command payload (example format, adjust as needed)
        # Format: "token,ssid,password" or just "token" to request current SSID?
        # Based on test, just sending token seems to trigger *something*.
        # Let's assume sending the token + SSID + password (if provided) sets it.
        # The exact format needs clarification based on the device's firmware.
        # For now, let's just send the token as per the test structure hint.
        # A more robust implementation would require a defined protocol.
        payload = token # Simplistic assumption based on test code structure
        if new_ssid:
             payload += f",{new_ssid}" # Example delimiter
             if password:
                 payload += f",{password}"

        self._log(f"Sending SSID command with payload: '{payload}'...")
        try:
            await self.client.write_gatt_char(self._ssid_control_char.uuid, payload.encode('utf-8'), response=True)
            self.last_hold_time = time.time() # Assume SSID command also acts as a hold
            self._log("SSID command successful.")
            # Optionally: Read CURRENT_SSID_CHAR_UUID to confirm change?
            # await self.read_current_ssid() # Requires read_current_ssid method
            return True
        except BleakError as e:
            self._log(f"SSID command failed: BLE error - {e}")
            return False
        except Exception as e:
            self._log(f"SSID command failed: Unexpected error - {e}")
            return False

    async def wipe(self, token):
        """Sends a wipe command."""
        if not self.client or not self.client.is_connected or not self._wipe_char:
            self._log("Wipe failed: Not connected or wipe characteristic not found.")
            return False
        if not self._is_token_valid(token):
            self._log(f"Wipe command failed: Invalid or expired token '{token}'.")
            return False

        self._log(f"Sending wipe command with token: {token}...")
        try:
            await self.client.write_gatt_char(self._wipe_char.uuid, token.encode('utf-8'), response=True)
            self.last_hold_time = time.time() # Assume wipe command also acts as a hold
            self._log("Wipe command successful.")
            # self.reset_state() # Wipe likely resets the device state
            return True
        except BleakError as e:
            self._log(f"Wipe failed: BLE error - {e}")
            return False
        except Exception as e:
            self._log(f"Wipe failed: Unexpected error - {e}")
            return False

    async def read_current_ssid(self):
        """Reads the current SSID characteristic."""
        if not self.client or not self.client.is_connected or not self._current_ssid_char:
            self._log("Read SSID failed: Not connected or characteristic not found.")
            return "Error"
        if "read" not in self._current_ssid_char.properties:
             self._log("Read SSID failed: Characteristic does not support read.")
             return "Error: Not readable"

        self._log("Reading current SSID...")
        try:
            ssid_bytes = await self.client.read_gatt_char(self._current_ssid_char.uuid)
            self.current_ssid = ssid_bytes.decode('utf-8').strip()
            self._log(f"Current SSID read: {self.current_ssid}")
            return self.current_ssid
        except BleakError as e:
            self._log(f"Read SSID failed: BLE error - {e}")
            return "Error"
        except Exception as e:
            self._log(f"Read SSID failed: Unexpected error - {e}")
            return "Error"

    async def read_status(self):
        """Reads the status characteristic."""
        if not self.client or not self.client.is_connected or not self._status_char:
            self._log("Read Status failed: Not connected or characteristic not found.")
            return "Error"
        if "read" not in self._status_char.properties:
             self._log("Read Status failed: Characteristic does not support read.")
             return "Error: Not readable"

        self._log("Reading status...")
        try:
            status_bytes = await self.client.read_gatt_char(self._status_char.uuid)
            self.status = status_bytes.decode('utf-8').strip() # Adjust decoding as needed
            self._log(f"Status read: {self.status}")
            return self.status
        except BleakError as e:
            self._log(f"Read Status failed: BLE error - {e}")
            return "Error"
        except Exception as e:
            self._log(f"Read Status failed: Unexpected error - {e}")
            return "Error"

    # --- Notification Handlers ---

    def _default_status_handler(self, sender, data):
        """Default handler for status notifications."""
        self.status = data.decode('utf-8').strip() # Adjust decoding as needed
        self._log(f"Status Notification Received: {self.status}")
        # Potentially update other state based on status
        if "Expired" in self.status or "Reset" in self.status: # Example status checks
             self.active_token = None
             self.claim_enabled = True

    def _default_ssid_handler(self, sender, data):
        """Default handler for SSID notifications."""
        self.current_ssid = data.decode('utf-8').strip()
        self._log(f"Current SSID Notification Received: {self.current_ssid}")

    async def start_status_notifications(self, callback=None):
        """Starts notifications for the status characteristic."""
        if not self.client or not self.client.is_connected or not self._status_char:
            self._log("Start Status Notify failed: Not connected or characteristic not found.")
            return False
        if "notify" not in self._status_char.properties:
             self._log("Start Status Notify failed: Characteristic does not support notify.")
             return False

        self._status_notification_handler = callback or self._default_status_handler
        self._log("Starting status notifications...")
        try:
            await self.client.start_notify(self._status_char.uuid, self._status_notification_handler)
            self._log("Status notifications started.")
            return True
        except BleakError as e:
            self._log(f"Failed to start status notifications: {e}")
            return False
        except Exception as e:
            self._log(f"Unexpected error starting status notifications: {e}")
            return False

    async def stop_status_notifications(self):
        """Stops notifications for the status characteristic."""
        if not self.client or not self.client.is_connected or not self._status_char or not self._status_notification_handler:
            self._log("Stop Status Notify failed: Not connected, characteristic not found, or notifications not started.")
            return False
        self._log("Stopping status notifications...")
        try:
            await self.client.stop_notify(self._status_char.uuid)
            self._log("Status notifications stopped.")
            self._status_notification_handler = None
            return True
        except BleakError as e:
            self._log(f"Failed to stop status notifications: {e}")
            return False
        except Exception as e:
            self._log(f"Unexpected error stopping status notifications: {e}")
            return False

    async def start_ssid_notifications(self, callback=None):
        """Starts notifications for the current SSID characteristic."""
        if not self.client or not self.client.is_connected or not self._current_ssid_char:
            self._log("Start SSID Notify failed: Not connected or characteristic not found.")
            return False
        if "notify" not in self._current_ssid_char.properties:
             self._log("Start SSID Notify failed: Characteristic does not support notify.")
             return False

        self._ssid_notification_handler = callback or self._default_ssid_handler
        self._log("Starting current SSID notifications...")
        try:
            await self.client.start_notify(self._current_ssid_char.uuid, self._ssid_notification_handler)
            self._log("Current SSID notifications started.")
            return True
        except BleakError as e:
            self._log(f"Failed to start current SSID notifications: {e}")
            return False
        except Exception as e:
            self._log(f"Unexpected error starting SSID notifications: {e}")
            return False

    async def stop_ssid_notifications(self):
        """Stops notifications for the current SSID characteristic."""
        if not self.client or not self.client.is_connected or not self._current_ssid_char or not self._ssid_notification_handler:
            self._log("Stop SSID Notify failed: Not connected, characteristic not found, or notifications not started.")
            return False
        self._log("Stopping current SSID notifications...")
        try:
            await self.client.stop_notify(self._current_ssid_char.uuid)
            self._log("Current SSID notifications stopped.")
            self._ssid_notification_handler = None
            return True
        except BleakError as e:
            self._log(f"Failed to stop current SSID notifications: {e}")
            return False
        except Exception as e:
            self._log(f"Unexpected error stopping SSID notifications: {e}")
            return False

    # --- State Management ---

    def get_state(self):
        """Returns the current state of the service."""
        token_expired = False
        if self.active_token and time.time() - self.last_hold_time > TOKEN_EXPIRY_DURATION:
            token_expired = True
            # Don't reset state here, just report expiry status

        return {
            "active_token": self.active_token if not token_expired else None,
            "claim_enabled": self.claim_enabled or token_expired, # Claim becomes enabled if token expires
            "current_ssid": self.current_ssid,
            "last_hold_time": self.last_hold_time,
            "token_expired": token_expired,
            "status": self.status, # Include status if available
            "is_connected": self.client.is_connected if self.client else False,
        }

    def reset(self):
        """Public method to reset the service state."""
        # Note: This resets the *wrapper's* state.
        # It does NOT send a command to reset the *device* itself,
        # unless implicitly done via wipe or similar command.
        self.reset_state()


# --- Example Usage (Async Context Needed) ---
async def main():
    service = AMB82Service()
    try:
        if await service.connect():
            print("\n--- Connected ---")
            initial_state = service.get_state()
            print("Initial State:")
            print(initial_state)

            # Example: Read initial status and SSID
            await service.read_status()
            await service.read_current_ssid()
            print("State after reads:")
            print(service.get_state())

            # --- Claim/Hold Example ---
            print("\n--- Attempting Claim ---")
            token = await service.claim()
            if token:
                print(f"Claim successful, token: {token}")
                print("State after claim:")
                print(service.get_state())

                print("\n--- Attempting Hold (Valid) ---")
                hold_success = await service.hold(token)
                print(f"Hold result: {hold_success}")
                print("State after hold:")
                print(service.get_state())

                print("\n--- Waiting for token expiry (sleep > 3s) ---")
                await asyncio.sleep(TOKEN_EXPIRY_DURATION + 1)
                expiry_wait_state = service.get_state()
                print("State after expiry wait:")
                print(expiry_wait_state)

                print("\n--- Attempting Hold (Expired) ---")
                hold_success_expired = await service.hold(token)
                print(f"Hold result (expired): {hold_success_expired}")

                print("\n--- Attempting Claim Again (should work now) ---")
                new_token = await service.claim()
                print(f"Second claim result: {new_token}")
                print("State after second claim:")
                print(service.get_state())

                if new_token:
                     print("\n--- Attempting Wipe ---")
                     wipe_success = await service.wipe(new_token)
                     print(f"Wipe result: {wipe_success}")
                     print("State after wipe:")
                     print(service.get_state())


            else:
                print("Claim failed.")

        else:
            print("Connection failed.")

    except Exception as e:
        print(f"An error occurred in main: {e}")
    finally:
        if service.client and service.client.is_connected:
            print("\n--- Disconnecting ---")
            await service.disconnect()

if __name__ == "__main__":
    # Running async code requires an event loop
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("Process interrupted by user.")
