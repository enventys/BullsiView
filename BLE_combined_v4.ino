/*
 * Combined AMB82 BLE Control Service and BLE UART Service Implementation
 * Version 4: Includes WiFi Channel Selection based on Scan Results
 *
 * Merges the custom control service logic with a standard BLE UART service.
 * Includes optional token expiration based on inactivity.
 * Selects the least congested WiFi channel (1-11) before starting the AP.
 * Device Name: Bullsi
 * Board Target: Ameba (using BLEDevice.h)
 */

// --- JPG Streaming Feature Flag ---
#define JPG_CONTINUOUS

#include "BLEDevice.h" // Core BLE library
#include "WiFi.h"      // WiFi library for Ameba
#include <stdlib.h>    // For random()
#include <time.h>      // For seeding random (Note: may not provide wall-clock time on embedded)
#include <vector>      // For storing channel scores
#include <limits>      // For numeric_limits

#ifdef JPG_CONTINUOUS
#include "VideoStream.h" // For camera streaming
#endif

// --- Step 1: Define UUIDs and Device Name ---

#define DEVICE_NAME "Bullsi"

// --- AMB82 Control Service UUIDs ---
#define AMB82_SERVICE_UUID      "A4951234-C5B1-4B44-B512-1370F02D74DE"
#define CLAIM_CHAR_UUID         "A4955678-C5B1-4B44-B512-1370F02D74D1" // (Read)
#define HOLD_CHAR_UUID          "A4955678-C5B1-4B44-B512-1370F02D74D2" // (Write)
#define SSID_CONTROL_CHAR_UUID  "A4955678-C5B1-4B44-B512-1370F02D74D3" // (Write)
#define CURRENT_SSID_CHAR_UUID  "A4955678-C5B1-4B44-B512-1370F02D74D4" // (Read, Notify)
#define WIPE_CHAR_UUID          "A4955678-C5B1-4B44-B512-1370F02D74D5" // (Write)
#define STATUS_CHAR_UUID        "A4955678-C5B1-4B44-B512-1370F02D74D6" // (Read, Notify)

// --- BLE UART Service UUIDs (Nordic Semiconductor Standard) ---
#define UART_SERVICE_UUID      "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_RX_CHAR_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // Write Property (Client -> Device)
#define UART_TX_CHAR_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // Read, Notify Property (Device -> Client)

// --- Configuration Constants ---
const unsigned int TOKEN_LENGTH = 8;
const unsigned int SSID_LENGTH = 6;
const uint8_t CONN_ID_INVALID = 0xFF; // Use 0xFF as placeholder for invalid connection ID
const unsigned int UART_BUFFER_SIZE = 8; // Reduce to 8 bytes to minimize attribute table usage
const int NUM_WIFI_CHANNELS = 11; // Consider channels 1-11 for 2.4GHz

// *** Token Expiration Configuration ***
// Set the duration in milliseconds after which an inactive token expires.
// Set to 0 to disable token expiration completely.
const unsigned long TOKEN_EXPIRATION_MS = 60000; // e.g., 60000 ms = 60 seconds

// --- Step 2: Declare BLE Objects and Application State ---

// AMB82 Control Service Objects
BLEService        ambService(AMB82_SERVICE_UUID);
BLECharacteristic claimChar(CLAIM_CHAR_UUID);
BLECharacteristic holdChar(HOLD_CHAR_UUID);
BLECharacteristic ssidControlChar(SSID_CONTROL_CHAR_UUID);
BLECharacteristic currentSsidChar(CURRENT_SSID_CHAR_UUID);
BLECharacteristic wipeChar(WIPE_CHAR_UUID);
BLECharacteristic statusChar(STATUS_CHAR_UUID);

// BLE UART Service Objects
BLEService        uartService(UART_SERVICE_UUID);
BLECharacteristic uartRxChar(UART_RX_CHAR_UUID); // Client writes here
BLECharacteristic uartTxChar(UART_TX_CHAR_UUID); // Client reads/notifies from here

// Combined Advertising Data
BLEAdvertData advdata;
BLEAdvertData scandata;

// AMB82 Control Service State Variables
String        activeToken = ""; // Token is now generated at boot
unsigned long lastHoldTimeMillis = 0; // Timestamp of the last successful claim or hold (for expiration)
String        currentSsid = "";
String        apChannel = ""; // Will be determined by scan
bool          statusNotifyEnabled = false;
bool          ssidNotifyEnabled = false;
uint8_t       statusConnId = CONN_ID_INVALID; // Track who subscribed to status
uint8_t       ssidConnId = CONN_ID_INVALID;   // Track who subscribed to SSID
bool          apStarted = false; // Flag to track if AP has been started

// BLE UART Service State Variables
bool          uartNotifyEnabled = false; // Flag if UART TX notifications are enabled
uint8_t       uartConnId = CONN_ID_INVALID; // Track who subscribed to UART TX

#ifdef JPG_CONTINUOUS
// --- JPG Streaming Globals & Constants ---
#define CHANNEL 0 // Camera channel
VideoSetting config(1024, 576, CAM_FPS, VIDEO_JPEG, 1); // Example custom resolution
WiFiServer server(80); // HTTP server for streaming
uint32_t img_addr = 0;
uint32_t img_len = 0;
#define PART_BOUNDARY "123456789000000000000987654321"
char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
char* IMG_HEADER = "Content-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n";
#endif // JPG_CONTINUOUS

// --- Step 3: Helper Functions ---

// Simple random string generator
String generateRandomString(unsigned int length) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    String result = "";
    result.reserve(length);
    // Ensure random seed is set in setup()
    for (unsigned int i = 0; i < length; ++i) {
        result += charset[random(0, sizeof(charset) - 1)];
    }
    return result;
}

String getCurrentStatus() {
    if (activeToken.length() == 0) {
        return "unclaimed";
    } else {
        if (TOKEN_EXPIRATION_MS > 0 && (millis() - lastHoldTimeMillis > TOKEN_EXPIRATION_MS)) {
             return "unclaimed"; // Treat as unclaimed if expired
        }
        return "claimed"; // Otherwise, it's claimed
    }
}

// Forward declaration for use in callbacks
void updateAndNotifyStatus(uint8_t connID = CONN_ID_INVALID);

// Update the Status characteristic and notify if enabled (Control Service)
void updateAndNotifyStatus(uint8_t connID) {
    String currentStatus = getCurrentStatus();
    statusChar.writeString(currentStatus);

    if (statusNotifyEnabled) {
        if (connID != CONN_ID_INVALID && BLE.connected(connID)) {
             Serial.print("[CtrlSvc] Notifying status '"); Serial.print(currentStatus);
             Serial.print("' to Conn "); Serial.println(connID);
             statusChar.notify(connID);
        } else {
             Serial.print("[CtrlSvc] Updating status char to '"); Serial.print(currentStatus);
             Serial.println("' (Notify enabled but target connection invalid/disconnected)");
        }
    } else {
         Serial.print("[CtrlSvc] Status updated to '"); Serial.print(currentStatus);
         Serial.println("' (Notify disabled)");
    }
}

// --- Step 3.5: WiFi Scan and Channel Selection Helpers (from example) ---

void printEncryptionTypeEx(uint32_t thisType)
{
    /*  Arduino wifi api use encryption type to mapping to security type.
     *  This function demonstrate how to get more richful information of security type.
     */
    switch (thisType) {
        case SECURITY_OPEN:
            Serial.print("Open");
            break;
        case SECURITY_WEP_PSK:
            Serial.print("WEP");
            break;
        case SECURITY_WPA_TKIP_PSK:
            Serial.print("WPA TKIP");
            break;
        case SECURITY_WPA_AES_PSK:
            Serial.print("WPA AES");
            break;
        case SECURITY_WPA2_AES_PSK:
            Serial.print("WPA2 AES");
            break;
        case SECURITY_WPA2_TKIP_PSK:
            Serial.print("WPA2 TKIP");
            break;
        case SECURITY_WPA2_MIXED_PSK:
            Serial.print("WPA2 Mixed");
            break;
        case SECURITY_WPA_WPA2_MIXED:
            Serial.print("WPA/WPA2 AES");
            break;
        case SECURITY_WPA3_AES_PSK:
            Serial.print("WPA3 AES");
            break;
        case SECURITY_WPA2_WPA3_MIXED:
            Serial.print("WPA2/WPA3");
    }
}

void printEncryptionType(int thisType)
{
    // read the encryption type and print out the name:
    switch (thisType) {
        case ENC_TYPE_WEP:
            Serial.print("WEP"); // Changed to print for inline use
            break;
        case ENC_TYPE_WPA:
            Serial.print("WPA"); // Changed to print
            break;
        case ENC_TYPE_WPA2:
            Serial.print("WPA2"); // Changed to print
            break;
        case ENC_TYPE_WPA3:
            Serial.print("WPA3"); // Changed to print
            break;
        case ENC_TYPE_NONE:
            Serial.print("None"); // Changed to print
            break;
        case ENC_TYPE_AUTO:
            Serial.print("Auto"); // Changed to print
            break;
        default:
             Serial.print("Unknown"); // Handle unknown types
             break;
    }
}


// --- Step 3.6: WiFi Scan and Channel Selection ---

// Function to scan networks, print details, and select the best channel (1-11)
String scanAndSelectChannel() {
    Serial.println("\n** Scan Networks **"); // Match example output
    int numSsid = WiFi.scanNetworks();

    if (numSsid == -1) {
        Serial.println("[WiFi Scan] Couldn't get a wifi connection during scan. Defaulting to channel 6.");
        return "6"; // Default channel if scan fails
    }
    if (numSsid == 0) {
        Serial.println("[WiFi Scan] No networks found. Defaulting to channel 6.");
        return "6"; // Default channel if no networks are found
    }

    // print the list of networks seen:
    Serial.print("number of available networks:");
    Serial.println(numSsid);
    Serial.println("-----------------------------------------");

    // print the network number and name for each network found:
    for (int thisNet = 0; thisNet < numSsid; thisNet++) {
        Serial.print(thisNet);
        Serial.print(") ");
        Serial.print(WiFi.SSID(thisNet));
        Serial.print("\tChannel: "); // Added Channel info
        Serial.print(WiFi.channel(thisNet));
        Serial.print("\tSignal: ");
        Serial.print(WiFi.RSSI(thisNet));
        Serial.print(" dBm");
        Serial.print("\tEncryptionRaw: ");
        printEncryptionTypeEx(WiFi.encryptionTypeEx(thisNet));
        Serial.print("\tEncryption: ");
        printEncryptionType(WiFi.encryptionType(thisNet));
        Serial.println(); // Newline after each network entry
    }
    Serial.println("-----------------------------------------");


    // --- Channel Scoring Logic (Remains the same) ---
    // Initialize channel scores
    std::vector<double> channelScores(NUM_WIFI_CHANNELS + 1, 0.0); // Index 1-11 used

    // Scoring parameters
    const double rssi_base_penalty = 100.0; // Base penalty added to RSSI (e.g., -50 dBm becomes 50 penalty)
    const double penalty_weight_ch0 = 1.0;  // Weight for same channel interference
    const double penalty_weight_ch1 = 0.5;  // Weight for 1 channel separation
    const double penalty_weight_ch2 = 0.25; // Weight for 2 channel separation

    Serial.println("[WiFi Scan] Calculating channel scores (lower penalty is better)...");

    // Calculate interference penalty for each channel
    for (int ch = 1; ch <= NUM_WIFI_CHANNELS; ++ch) {
        double totalPenalty = 0.0;
        for (int i = 0; i < numSsid; ++i) { // Use numSsid here
            int networkChannel = WiFi.channel(i);
            long networkRssi = WiFi.RSSI(i);
            int channelDiff = abs(networkChannel - ch);

            // Calculate penalty based on RSSI (stronger signal = higher penalty)
            // Adding rssi_base_penalty makes RSSI positive and scales it.
            // E.g., -30dBm -> 70 penalty base, -80dBm -> 20 penalty base
            double rssiPenalty = (double)networkRssi + rssi_base_penalty;
            if (rssiPenalty < 0) rssiPenalty = 0; // Ensure penalty is not negative

            // Apply penalty based on channel separation
            if (channelDiff == 0) {
                totalPenalty += rssiPenalty * penalty_weight_ch0;
            } else if (channelDiff == 1) {
                totalPenalty += rssiPenalty * penalty_weight_ch1;
            } else if (channelDiff == 2) {
                totalPenalty += rssiPenalty * penalty_weight_ch2;
            }
            // Ignore networks more than 2 channels away
        }
        channelScores[ch] = totalPenalty; // Store the total penalty for this channel
        Serial.print("  Channel "); Serial.print(ch); Serial.print(": Penalty Score = "); Serial.println(channelScores[ch]);
    }
    Serial.println("-----------------------------------------");

    // Find the channel with the minimum penalty score
    int bestChannel = 1; // Default to channel 1
    double minPenalty = std::numeric_limits<double>::max();

    for (int ch = 1; ch <= NUM_WIFI_CHANNELS; ++ch) {
        if (channelScores[ch] < minPenalty) {
            minPenalty = channelScores[ch];
            bestChannel = ch;
        }
    }

    Serial.print("[WiFi Scan] Preferred Channel Selected: "); Serial.println(bestChannel);
    Serial.println("-----------------------------------------");

    return String(bestChannel);
}


// --- Step 3.7: Define AP start function ---
void startAP() {
    if (!apStarted && activeToken.length() > 0 && apChannel.length() > 0) { // Ensure token AND channel are ready
        Serial.println("\n[WiFi] Attempting to start Access Point...");

        char ssidBuf[SSID_LENGTH + 1];
        char tokenBuf[TOKEN_LENGTH + 1]; // Use token as password
        char channelBuf[apChannel.length() + 1];
        int ssid_status = 0; // 0 = not hidden, 1 = hidden

        currentSsid.toCharArray(ssidBuf, sizeof(ssidBuf));
        activeToken.toCharArray(tokenBuf, sizeof(tokenBuf));
        apChannel.toCharArray(channelBuf, sizeof(channelBuf));

        Serial.print("[WiFi] Starting AP with SSID: "); Serial.print(ssidBuf);
        Serial.print(", Channel: "); Serial.print(channelBuf);
        Serial.print(", Password: "); Serial.println(tokenBuf);

        int ap_status = WiFi.apbegin(ssidBuf, tokenBuf, channelBuf, ssid_status);
        Serial.print("[WiFi] apbegin() returned: ");
        Serial.println(ap_status);

        if (ap_status == WL_CONNECTED) {
            apStarted = true; // Set flag so we don't try again
            Serial.print("[WiFi] Access Point started successfully!");
            Serial.print(" SSID: "); Serial.print(currentSsid);
            Serial.print(", Password: "); Serial.print(activeToken);
            Serial.print(", Channel: "); Serial.println(apChannel);

            // Print WiFi info
            IPAddress ip = WiFi.localIP();
            Serial.print("[WiFi] IP Address: "); Serial.println(ip);
            IPAddress subnet = WiFi.subnetMask();
            Serial.print("[WiFi] NetMask: "); Serial.println(subnet);
            IPAddress gateway = WiFi.gatewayIP();
            Serial.print("[WiFi] Gateway: "); Serial.println(gateway);
            Serial.print("[WiFi] SSID: "); Serial.println(WiFi.SSID());
            byte bssid[6];
            WiFi.BSSID(bssid);
            Serial.print("[WiFi] BSSID: ");
            for (int i = 0; i < 6; i++) {
                Serial.print(bssid[i], HEX);
                if (i < 5) Serial.print(":");
            }
            Serial.println();
            byte encryption = WiFi.encryptionType();
            Serial.print("[WiFi] Encryption Type: "); Serial.println(encryption, HEX);

        } else {
            Serial.println("[WiFi] Failed to start Access Point. Check parameters and logs.");
        }
         Serial.println("-----------------------------------------"); // Separator
    } else if (apStarted) {
         Serial.println("[WiFi] AP already started.");
    } else {
     Serial.println("[WiFi] Cannot start AP: Token or Channel not ready yet.");
    }
}

#ifdef JPG_CONTINUOUS
// --- JPG Streaming Helper Functions ---
void sendHeader(WiFiClient& client)
{
    client.print("HTTP/1.1 200 OK\r\nContent-type: multipart/x-mixed-replace; boundary=");
    client.println(PART_BOUNDARY);
    client.print("Transfer-Encoding: chunked\r\n");
    client.print("\r\n");
}

void sendChunk(WiFiClient& client, uint8_t* buf, uint32_t len)
{
    uint8_t chunk_buf[64] = {0};
    uint8_t chunk_len = snprintf((char*)chunk_buf, 64, "%lX\r\n", len);
    client.write(chunk_buf, chunk_len);
    client.write(buf, len);
    client.print("\r\n");
}
#endif // JPG_CONTINUOUS

// --- Step 4: Implement BLE Callback Functions ---

// --- AMB82 Control Service Callbacks ---

// Claim Characteristic (Read)
void claimReadCallback(BLECharacteristic* chr, uint8_t connID) {
    Serial.print("[Callback] Claim read request from Conn "); Serial.println(connID);
    String response = activeToken;
    Serial.print("[CtrlSvc] Claim request: Returning pre-generated token: "); Serial.println(activeToken);
    chr->writeString(response);
}

// Hold Characteristic (Write)
void holdWriteCallback(BLECharacteristic* chr, uint8_t connID) {
    String receivedToken = chr->readString();
    Serial.print("[Callback] Hold write request from Conn "); Serial.print(connID);
    Serial.print(" with token: '"); Serial.print(receivedToken); Serial.println("'");

    if (TOKEN_EXPIRATION_MS > 0 && activeToken.length() > 0 && (millis() - lastHoldTimeMillis > TOKEN_EXPIRATION_MS)) {
        Serial.println("[CtrlSvc] Hold denied: Token has expired.");
        return;
    }

    if (activeToken.length() == 0) {
        Serial.println("[CtrlSvc] Hold denied: Device token not initialized (Error).");
    } else if (receivedToken.equals(activeToken)) {
        lastHoldTimeMillis = millis(); // Refresh the hold timer
        Serial.print("[CtrlSvc] Token hold refreshed for: "); Serial.println(activeToken);
    } else {
        Serial.print("[CtrlSvc] Hold denied: Token mismatch. Provided: '"); Serial.print(receivedToken);
        Serial.print("', Active: '"); Serial.print(activeToken); Serial.println("'");
    }
}

// SSID Control Characteristic (Write)
void ssidControlWriteCallback(BLECharacteristic* chr, uint8_t connID) {
    String receivedToken = chr->readString();
    Serial.print("[Callback] SSID Control write request from Conn "); Serial.print(connID);
    Serial.print(" with token: '"); Serial.print(receivedToken); Serial.println("'");

    if (TOKEN_EXPIRATION_MS > 0 && activeToken.length() > 0 && (millis() - lastHoldTimeMillis > TOKEN_EXPIRATION_MS)) {
        Serial.println("[CtrlSvc] SSID request denied: Token has expired.");
        return;
    }

    if (activeToken.length() == 0) {
        Serial.println("[CtrlSvc] SSID request denied: Device token not initialized (Error).");
    } else if (receivedToken.equals(activeToken)) {
        lastHoldTimeMillis = millis(); // Refresh timer
        Serial.print("[CtrlSvc] Token accepted. Using established SSID: "); Serial.println(currentSsid);
        Serial.print("[CtrlSvc] Using established token: "); Serial.println(activeToken);
        Serial.print("[CtrlSvc] Using established channel: "); Serial.println(apChannel);

        currentSsidChar.writeString(currentSsid); // Update characteristic value

        if (ssidNotifyEnabled && ssidConnId != CONN_ID_INVALID && BLE.connected(ssidConnId)) {
            Serial.print("[CtrlSvc] Notifying current SSID to Conn "); Serial.println(ssidConnId);
            currentSsidChar.notify(ssidConnId);
        }
    } else {
        Serial.print("[CtrlSvc] SSID request denied: Token mismatch. Provided: '"); Serial.print(receivedToken);
        Serial.print("', Active: '"); Serial.print(activeToken); Serial.println("'");
    }
}

// Current SSID Characteristic (Read)
void currentSsidReadCallback(BLECharacteristic* chr, uint8_t connID) {
    Serial.print("[Callback] Current SSID read request from Conn "); Serial.println(connID);
    Serial.print("[CtrlSvc] Sending current SSID: '"); Serial.print(currentSsid); Serial.println("'");
    chr->writeString(currentSsid);
}

// Wipe Characteristic (Write)
void wipeWriteCallback(BLECharacteristic* chr, uint8_t connID) {
    String receivedToken = chr->readString();
    Serial.print("[Callback] Wipe write request from Conn "); Serial.print(connID);
    Serial.print(" with token: '"); Serial.print(receivedToken); Serial.println("'");

    if (TOKEN_EXPIRATION_MS > 0 && activeToken.length() > 0 && (millis() - lastHoldTimeMillis > TOKEN_EXPIRATION_MS)) {
        Serial.println("[CtrlSvc] Wipe request denied: Token has expired.");
        return;
    }

    if (activeToken.length() == 0) {
        Serial.println("[CtrlSvc] Wipe request denied: Device token not initialized (Error).");
    } else if (receivedToken.equals(activeToken)) {
        lastHoldTimeMillis = millis(); // Refresh timer
        Serial.println("[CtrlSvc] !!! WIPE EVENT INITIATED by token !!!");
        // TODO: Add actual wipe actions here
    } else {
        Serial.print("[CtrlSvc] Wipe request denied: Token mismatch. Provided: '"); Serial.print(receivedToken);
        Serial.print("', Active: '"); Serial.print(activeToken); Serial.println("'");
    }
}

// Status Characteristic (Read)
void statusReadCallback(BLECharacteristic* chr, uint8_t connID) {
    Serial.print("[Callback] Status read request from Conn "); Serial.println(connID);
    updateAndNotifyStatus(CONN_ID_INVALID); // Update internal value
    String currentStatus = getCurrentStatus();
    Serial.print("[CtrlSvc] Sending status: '"); Serial.print(currentStatus); Serial.println("'");
    chr->writeString(currentStatus);
}

// Status CCCD Callback (Handling Notifications)
void statusNotifyCallback(BLECharacteristic* chr, uint8_t connID, uint16_t cccd) {
    Serial.print("[Callback] Status CCCD changed by Conn "); Serial.print(connID);
    Serial.print(". Value: 0x"); Serial.println(cccd, HEX);

    if (cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY) {
        Serial.println("  Status Notifications ENABLED");
        statusNotifyEnabled = true;
        statusConnId = connID;
        updateAndNotifyStatus(connID); // Send current status on subscribe
    } else {
        Serial.println("  Status Notifications DISABLED");
        if(connID == statusConnId) {
            statusNotifyEnabled = false;
            statusConnId = CONN_ID_INVALID;
        }
    }
}

// Current SSID CCCD Callback (Handling Notifications)
void ssidNotifyCallback(BLECharacteristic* chr, uint8_t connID, uint16_t cccd) {
    Serial.print("[Callback] Current SSID CCCD changed by Conn "); Serial.print(connID);
    Serial.print(". Value: 0x"); Serial.println(cccd, HEX);

    if (cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY) {
        Serial.println("  Current SSID Notifications ENABLED");
        ssidNotifyEnabled = true;
        ssidConnId = connID;
        if (BLE.connected(connID)) {
             Serial.print("[CtrlSvc] Notifying current SSID '"); Serial.print(currentSsid);
             Serial.print("' to Conn "); Serial.println(connID);
             chr->notify(connID);
        }
    } else {
        Serial.println("  Current SSID Notifications DISABLED");
        if(connID == ssidConnId) {
             ssidNotifyEnabled = false;
             ssidConnId = CONN_ID_INVALID;
        }
    }
}


// --- BLE UART Service Callbacks ---

// UART RX Characteristic (Write from Client)
void uartRxWriteCallback(BLECharacteristic* chr, uint8_t connID) {
    printf("[Callback] UART RX write from Conn %d:\n", connID);
    if (chr->getDataLen() > 0) {
        Serial.print("  Received via BLE UART: ");
        Serial.print(chr->readString());
        Serial.println();
        // Optional: Echo back
    }
}

// UART TX Characteristic (Read by Client)
void uartTxReadCallback(BLECharacteristic* chr, uint8_t connID) {
    printf("[Callback] UART TX read by Conn %d\n", connID);
}

// UART TX CCCD Callback (Handling Notifications)
void uartTxNotifyCallback(BLECharacteristic* chr, uint8_t connID, uint16_t cccd) {
    Serial.print("[Callback] UART TX CCCD changed by Conn "); Serial.print(connID);
    Serial.print(". Value: 0x"); Serial.println(cccd, HEX);

    if (cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY) {
        Serial.println("  UART TX Notifications ENABLED");
        uartNotifyEnabled = true;
        uartConnId = connID;
    } else {
        Serial.println("  UART TX Notifications DISABLED");
        if(connID == uartConnId) {
            uartNotifyEnabled = false;
            uartConnId = CONN_ID_INVALID;
        }
    }
}


// --- Step 5: Setup Function ---
void setup() {
    Serial.begin(115200);
    delay(2000); // Allow Serial monitor connection

    Serial.println("\nStarting Combined BLE Service v4 (Control + UART + WiFi Scan)...");
    Serial.print("Device Name: "); Serial.println(DEVICE_NAME);
    if (TOKEN_EXPIRATION_MS > 0) {
        Serial.print("Token Expiration Enabled: "); Serial.print(TOKEN_EXPIRATION_MS / 1000); Serial.println(" seconds");
    } else {
        Serial.println("Token Expiration Disabled.");
    }

    // Seed random number generator
    unsigned long entropy = analogRead(A0) ^ analogRead(A1) ^ analogRead(A2) ^ millis() ^ micros();
    randomSeed(entropy);
    Serial.print("Random seed initialized with entropy: "); Serial.println(entropy);

    // 1. Scan WiFi and select the best channel *before* generating credentials
    apChannel = scanAndSelectChannel(); // Determine channel first

    // 2. Generate permanent random SSID and Token (Password) for this session
    currentSsid = generateRandomString(SSID_LENGTH);
    activeToken = generateRandomString(TOKEN_LENGTH); // Generate token at boot
    lastHoldTimeMillis = millis(); // Initialize timestamp

    // Set the BLE characteristic for SSID
    currentSsidChar.writeString(currentSsid);

    // 3. Print initial WiFi info (including selected channel)
    Serial.println("[WiFi] --- Initial AP Credentials ---");
    Serial.print("[WiFi] SSID: "); Serial.println(currentSsid);
    Serial.print("[WiFi] Selected Channel: "); Serial.println(apChannel); // Log selected channel
    Serial.print("[WiFi] Password (Token): "); Serial.println(activeToken);
    Serial.println("[WiFi] These values will remain fixed until the device is reset.");

    // 4. Start AP immediately using the selected channel
    startAP();

    // 5. Initialize BLE services (AFTER starting AP)

    // --- Configure Advertising Data ---
    advdata.addFlags(GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED);
    advdata.addCompleteName(DEVICE_NAME);

    // --- Configure Scan Response Data ---
    scandata.addCompleteServices(BLEUUID(AMB82_SERVICE_UUID));
    scandata.addCompleteServices(BLEUUID(UART_SERVICE_UUID));
    Serial.println("Scan response configured for both services.");

    // --- Configure AMB82 Control Service Characteristics ---
    Serial.println("Configuring AMB82 Control Service characteristics...");
    claimChar.setReadProperty(true); claimChar.setReadPermissions(GATT_PERM_READ); claimChar.setReadCallback(claimReadCallback);
    holdChar.setWriteProperty(true); holdChar.setWritePermissions(GATT_PERM_WRITE); holdChar.setWriteCallback(holdWriteCallback); holdChar.setBufferLen(TOKEN_LENGTH + 5);
    ssidControlChar.setWriteProperty(true); ssidControlChar.setWritePermissions(GATT_PERM_WRITE); ssidControlChar.setWriteCallback(ssidControlWriteCallback); ssidControlChar.setBufferLen(TOKEN_LENGTH + 5);
    currentSsidChar.setReadProperty(true); currentSsidChar.setNotifyProperty(true); currentSsidChar.setReadPermissions(GATT_PERM_READ); currentSsidChar.setReadCallback(currentSsidReadCallback); currentSsidChar.setCCCDCallback(ssidNotifyCallback); currentSsidChar.setBufferLen(SSID_LENGTH + 5);
    wipeChar.setWriteProperty(true); wipeChar.setWritePermissions(GATT_PERM_WRITE); wipeChar.setWriteCallback(wipeWriteCallback); wipeChar.setBufferLen(TOKEN_LENGTH + 5);
    statusChar.setReadProperty(true); statusChar.setNotifyProperty(true); statusChar.setReadPermissions(GATT_PERM_READ); statusChar.setReadCallback(statusReadCallback); statusChar.setCCCDCallback(statusNotifyCallback); statusChar.setBufferLen(15);

    // --- Add Characteristics to the AMB82 Control Service ---
    ambService.addCharacteristic(claimChar); ambService.addCharacteristic(holdChar); ambService.addCharacteristic(ssidControlChar); ambService.addCharacteristic(currentSsidChar); ambService.addCharacteristic(wipeChar); ambService.addCharacteristic(statusChar);
    Serial.println("AMB82 Control characteristics added to service.");

    // --- Configure BLE UART Service Characteristics ---
    Serial.println("Configuring BLE UART Service characteristics...");
    uartRxChar.setWriteProperty(true); uartRxChar.setWritePermissions(GATT_PERM_WRITE); uartRxChar.setWriteCallback(uartRxWriteCallback); uartRxChar.setBufferLen(UART_BUFFER_SIZE);
    uartTxChar.setReadProperty(true); uartTxChar.setReadPermissions(GATT_PERM_READ); uartTxChar.setNotifyProperty(true); uartTxChar.setCCCDCallback(uartTxNotifyCallback); uartTxChar.setBufferLen(UART_BUFFER_SIZE);

    // --- Add Characteristics to the UART Service ---
    uartService.addCharacteristic(uartRxChar); uartService.addCharacteristic(uartTxChar);
    Serial.println("BLE UART characteristics added to service.");

    // --- Initialize BLE Stack ---
    BLE.init();
    Serial.println("BLE stack initialized.");

    // --- Configure BLE Server ---
    BLE.configAdvert()->setAdvData(advdata);
    BLE.configAdvert()->setScanRspData(scandata);
    BLE.configServer(1); // Max 1 connection

    // --- Add BOTH Services to the BLE Server ---
    BLE.addService(ambService); Serial.print("AMB82 Control Service added (UUID: "); Serial.print(AMB82_SERVICE_UUID); Serial.println(")");
    BLE.addService(uartService); Serial.print("BLE UART Service added (UUID: "); Serial.print(UART_SERVICE_UUID); Serial.println(")");

    // Set initial characteristic values
    updateAndNotifyStatus(CONN_ID_INVALID); // Set initial status ("claimed")
    currentSsidChar.writeString(currentSsid); // Already set

    // --- Start Advertising ---
    BLE.beginPeripheral();

    Serial.println("BLE Peripheral initialized and advertising.");
    Serial.println("-----------------------------------------");

#ifdef JPG_CONTINUOUS
    // --- Initialize Camera and HTTP Server for Streaming ---
    Serial.println("[JPG Stream] Initializing Camera...");
    Camera.configVideoChannel(CHANNEL, config);
    Camera.videoInit();
    Camera.channelBegin(CHANNEL);
    Serial.println("[JPG Stream] Camera Initialized.");

    Serial.println("[JPG Stream] Starting HTTP Server on port 80...");
    server.begin();
    Serial.println("[JPG Stream] HTTP Server Started.");
    Serial.println("-----------------------------------------");
#endif // JPG_CONTINUOUS
}

// --- Step 6: Loop Function ---
void loop() {
    // --- Check for Token Expiration ---
    if (TOKEN_EXPIRATION_MS > 0 && activeToken.length() > 0) {
        unsigned long currentTime = millis();
        if (currentTime - lastHoldTimeMillis > TOKEN_EXPIRATION_MS) {
            Serial.print("\n[CtrlSvc] Token '"); Serial.print(activeToken); Serial.print("' EXPIRED after ");
            Serial.print(TOKEN_EXPIRATION_MS / 1000); Serial.println(" seconds of inactivity.");
            activeToken = ""; // Reset token
            lastHoldTimeMillis = 0;
            updateAndNotifyStatus(statusConnId); // Update status to "unclaimed"
            Serial.println("[WiFi] Note: Access Point password remains unchanged from the initial token.");
            Serial.println("-----------------------------------------");
        }
    }

    // --- Check Serial input for BLE UART TX ---
    if (Serial.available()) {
        String serialInput = Serial.readString();
        serialInput.trim();

        if (serialInput.length() > 0) {
            Serial.print("[Serial] Read: '"); Serial.print(serialInput); Serial.println("'");

            if (serialInput.length() > UART_BUFFER_SIZE) {
                 Serial.print("  Warning: Input too long, truncating to "); Serial.print(UART_BUFFER_SIZE); Serial.println(" bytes for BLE TX.");
                 serialInput = serialInput.substring(0, UART_BUFFER_SIZE);
            }

            uartTxChar.writeString(serialInput);

            if (uartNotifyEnabled && uartConnId != CONN_ID_INVALID && BLE.connected(uartConnId)) {
                Serial.print("  Notifying UART TX data to Conn "); Serial.println(uartConnId);
                uartTxChar.notify(uartConnId);
            }
        }
    }

#ifdef JPG_CONTINUOUS
    // --- Handle HTTP Client for JPG Streaming ---
    WiFiClient client = server.available();

    if (client) {
        Serial.println("[JPG Stream] New client connected");
        String currentLine = "";
        while (client.connected()) {
            if (client.available()) {
                char c = client.read();
                if (c == '\n') {
                    if (currentLine.length() == 0) {
                        Serial.println("[JPG Stream] Sending HTTP header and starting stream...");
                        sendHeader(client);
                        while (client.connected()) {
                            Camera.getImage(CHANNEL, &img_addr, &img_len);
                            if (img_len > 0) {
                                uint8_t chunk_buf[64] = {0};
                                uint8_t chunk_len = snprintf((char*)chunk_buf, 64, IMG_HEADER, img_len);
                                sendChunk(client, chunk_buf, chunk_len);
                                sendChunk(client, (uint8_t*)img_addr, img_len);
                                sendChunk(client, (uint8_t*)STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
                            } else {
                                Serial.println("[JPG Stream] Warning: Failed to get image frame.");
                            }
                            delay(5); // Adjust delay for frame rate/resolution balance
                        }
                        Serial.println("[JPG Stream] Client disconnected during stream.");
                        break;
                    } else {
                        currentLine = "";
                    }
                } else if (c != '\r') {
                    currentLine += c;
                }
            }
        }
        client.stop();
        Serial.println("[JPG Stream] Client disconnected.");
    }
#endif // JPG_CONTINUOUS
}
