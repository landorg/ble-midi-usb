// BLE MIDI Adapter: BLE MIDI central in, class-compliant USB MIDI device out.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <cstring>

#include "BleMidiParser.h"
#include "USB.h"
#include "USBMIDI.h"
#include "adapter.h"
#ifdef HAS_SCREEN
#include "ui.h"
#endif

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Needs an ESP32-S3 (native USB OTG). A C3 cannot be a USB MIDI device."
#endif

// Optional advertised-name filter for first pairing; empty = any BLE MIDI device.
#ifndef PEER_NAME_PREFIX
#define PEER_NAME_PREFIX ""
#endif

static const NimBLEUUID kMidiService("03b80e5a-ede8-4b33-a751-6ce34ec4c700");
static const NimBLEUUID kMidiChar("7772e5db-3868-4112-a1a9-f2669d106bf3");

// Hold to forget the stored peer; -1 disables it.
#ifndef FORGET_BUTTON_PIN
#define FORGET_BUTTON_PIN 0
#endif

static constexpr int kBootButtonPin = FORGET_BUTTON_PIN;
static constexpr uint32_t kForgetHoldMs = 3000;
static constexpr uint32_t kSeenExpiryMs = 15000;
static constexpr size_t kMaxSeen = 12;

static USBMIDI usbMidi;
static Preferences prefs;
static QueueHandle_t usbQueue;
static SemaphoreHandle_t stateLock;

class StateGuard {
public:
    StateGuard() { xSemaphoreTake(stateLock, portMAX_DELAY); }
    ~StateGuard() { xSemaphoreGive(stateLock); }
};

struct SeenDevice {
    NimBLEAddress address;
    char name[32];
    int rssi;
    uint32_t lastSeen;
    bool used;
};

// All guarded by stateLock.
static SeenDevice seen[kMaxSeen];
static NimBLEAddress storedPeer;  // empty until a device has been connected
static char storedName[32];
static NimBLEAddress pendingPeer;
static bool havePending = false;
static NimBLEAddress requestedPeer;
static bool haveRequest = false;
static bool forgetRequested = false;
static bool forgetKeepsAutoPair = true;
static bool autoPair = true;  // connect to the first device seen while none is stored

static NimBLEClient* client = nullptr;
static volatile bool connected = false;
static volatile bool connecting = false;
static volatile bool discovery = false;

static void queuePacket(const UsbMidiPacket& pkt, void*) {
    // Called from the NimBLE host task; never block it.
    xQueueSend(usbQueue, &pkt, 0);
}

static BleMidiParser parser(queuePacket, nullptr);

static void queueAllNotesOff() {
    for (uint8_t ch = 0; ch < 16; ch++) {
        UsbMidiPacket pkt = {{0x0B, static_cast<uint8_t>(0xB0 | ch), 123, 0}};
        queuePacket(pkt, nullptr);
    }
}

static void usbTxTask(void*) {
    UsbMidiPacket pkt;
    for (;;) {
        if (xQueueReceive(usbQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;
        midiEventPacket_t ev = {pkt.b[0], pkt.b[1], pkt.b[2], pkt.b[3]};
        usbMidi.writePacket(&ev);
#ifdef HAS_SCREEN
        uiOnMidi(pkt);
#endif
    }
}

static void copyName(char (&dst)[32], const char* src) {
    strncpy(dst, src, sizeof(dst) - 1);
    dst[sizeof(dst) - 1] = 0;
}

static void loadPeer() {
    prefs.begin("bridge", true);
    String addr = prefs.getString("peer", "");
    uint8_t type = prefs.getUChar("peerType", 0);
    String name = prefs.getString("peerName", "");
    prefs.end();
    if (addr.length()) storedPeer = NimBLEAddress(addr.c_str(), type);
    copyName(storedName, name.c_str());
}

// Only called from the BLE task, which is the only writer of the stored peer.
static void savePeer(const NimBLEAddress& addr) {
    char name[32];
    {
        StateGuard guard;
        copyName(name, addr == storedPeer ? storedName : "");
        for (const SeenDevice& s : seen) {
            if (s.used && s.address == addr && s.name[0]) copyName(name, s.name);
        }
        if (addr == storedPeer && !strcmp(name, storedName)) return;
        storedPeer = addr;
        copyName(storedName, name);
    }
    prefs.begin("bridge", false);
    prefs.putString("peer", addr.toString().c_str());
    prefs.putUChar("peerType", addr.getType());
    prefs.putString("peerName", name);
    prefs.end();
}

static void dropLink() {
    if (!client || !client->isConnected()) return;
    client->disconnect();
    for (int i = 0; i < 200 && client->isConnected(); i++) delay(10);
}

static void forgetPeer(bool keepAutoPair) {
    {
        StateGuard guard;
        storedPeer = NimBLEAddress();
        storedName[0] = 0;
        havePending = false;
        autoPair = keepAutoPair;
    }
    dropLink();
    prefs.begin("bridge", false);
    prefs.clear();
    prefs.end();
    NimBLEDevice::deleteAllBonds();
    Serial.println("Forgot stored peer and bonds");
}

static void rememberSeen(const NimBLEAdvertisedDevice* dev) {
    SeenDevice* slot = nullptr;
    SeenDevice* oldest = &seen[0];
    uint32_t now = millis();
    for (SeenDevice& s : seen) {
        if (s.used && s.address == dev->getAddress()) {
            slot = &s;
            break;
        }
        if (!s.used) {
            if (!slot) slot = &s;
        } else if (s.lastSeen < oldest->lastSeen) {
            oldest = &s;
        }
    }
    if (!slot) slot = oldest;
    if (!slot->used || !(slot->address == dev->getAddress())) {
        slot->address = dev->getAddress();
        slot->name[0] = 0;
        slot->used = true;
    }
    if (!dev->getName().empty()) copyName(slot->name, dev->getName().c_str());
    slot->rssi = dev->getRSSI();
    slot->lastSeen = now;
}

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        bool midi = dev->isAdvertisingService(kMidiService);
        {
            StateGuard guard;
            if (midi) rememberSeen(dev);
            if (connected || connecting || havePending || haveRequest) return;
            bool match;
            if (!storedPeer.isNull()) {
                match = dev->getAddress() == storedPeer;
            } else {
                match = autoPair && midi && dev->getName().rfind(PEER_NAME_PREFIX, 0) == 0;
            }
            if (!match) return;
            pendingPeer = dev->getAddress();
            havePending = true;
        }
        Serial.printf("Found %s (%s)\n", dev->getName().c_str(), dev->getAddress().toString().c_str());
    }
} scanCallbacks;

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int reason) override {
        Serial.printf("Disconnected (reason %d)\n", reason);
        connected = false;
        parser.reset();
        queueAllNotesOff();
    }
} clientCallbacks;

static void onMidiNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
#ifdef LOG_RAW_BLE
    char line[3 * 64 + 16];
    int n = snprintf(line, sizeof(line), "%3u:", static_cast<unsigned>(len));
    for (size_t i = 0; i < len && i < 64; i++) n += snprintf(line + n, sizeof(line) - n, " %02x", data[i]);
    Serial.println(line);
#endif
    parser.parse(data, len);
}

static bool connectPeer(const NimBLEAddress& addr) {
    if (!client) {
        client = NimBLEDevice::createClient();
        client->setClientCallbacks(&clientCallbacks, false);
        // 7.5-15 ms connection interval for low latency; the device may override.
        client->setConnectionParams(6, 12, 0, 200);
        client->setConnectTimeout(5000);
    }
    if (!client->connect(addr)) {
        Serial.println("Connect failed");
        return false;
    }

    NimBLERemoteService* svc = client->getService(kMidiService);
    NimBLERemoteCharacteristic* chr = svc ? svc->getCharacteristic(kMidiChar) : nullptr;
    // NimBLE secures the link and retries if the CCCD write needs encryption.
    if (!chr || !chr->canNotify() || !chr->subscribe(true, onMidiNotify)) {
        Serial.println("No usable BLE MIDI characteristic");
        dropLink();
        return false;
    }
    // Per the BLE MIDI spec the central reads once after connecting.
    if (chr->canRead()) chr->readValue();

    savePeer(addr);
    connected = true;
    Serial.println("Connected, forwarding MIDI");
    return true;
}

// Searching for a device: listen all the time. Connected: only brief windows, so
// that scanning takes as little radio time from the MIDI link as possible.
static void updateScan() {
    static bool lowDuty = false;
    NimBLEScan* scan = NimBLEDevice::getScan();
    bool want = !connected || discovery;
    bool wantLowDuty = connected;
    if (!want || wantLowDuty != lowDuty) {
        if (scan->isScanning()) scan->stop();
    }
    if (want && !scan->isScanning()) {
        lowDuty = wantLowDuty;
        scan->setInterval(lowDuty ? 200 : 100);
        scan->setWindow(lowDuty ? 30 : 100);
        scan->start(0, false, true);  // until stopped
    }
}

static void bleTask(void*) {
    for (;;) {
        bool forget;
        bool keepAutoPair;
        bool request;
        NimBLEAddress requested;
        {
            StateGuard guard;
            forget = forgetRequested;
            forgetRequested = false;
            keepAutoPair = forgetKeepsAutoPair;
            request = haveRequest;
            haveRequest = false;
            requested = requestedPeer;
        }
        if (forget) forgetPeer(keepAutoPair);
        if (request) {
            dropLink();
            StateGuard guard;
            pendingPeer = requested;
            havePending = true;
        }

        bool pending;
        NimBLEAddress addr;
        {
            StateGuard guard;
            pending = havePending;
            havePending = false;
            addr = pendingPeer;
        }
        if (pending && !connected) {
            NimBLEScan* scan = NimBLEDevice::getScan();
            if (scan->isScanning()) scan->stop();
            connecting = true;
            {
                StateGuard guard;
                requestedPeer = addr;  // which device the screen shows as connecting
            }
            bool ok = connectPeer(addr);
            connecting = false;
            if (!ok) delay(500);
            continue;
        }

        updateScan();
        delay(20);
    }
}

LinkState adapterLinkState() {
    if (connected) return LinkState::Connected;
    if (connecting) return LinkState::Connecting;
    StateGuard guard;
    return havePending || haveRequest                 ? LinkState::Connecting
           : !storedPeer.isNull() || autoPair         ? LinkState::Searching
                                                      : LinkState::Idle;
}

size_t adapterListDevices(DeviceEntry* out, size_t max) {
    LinkState link = adapterLinkState();
    uint32_t now = millis();
    StateGuard guard;
    size_t n = 0;

    auto add = [&](const NimBLEAddress& addr, const char* name, int rssi, LinkState state) {
        DeviceEntry& e = out[n++];
        strncpy(e.address, addr.toString().c_str(), sizeof(e.address) - 1);
        e.address[sizeof(e.address) - 1] = 0;
        e.addressType = addr.getType();
        strncpy(e.name, name, sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = 0;
        e.rssi = rssi;
        e.state = state;
    };

    bool busy = link == LinkState::Connecting;
    NimBLEAddress target = busy && (haveRequest || connecting) ? requestedPeer : storedPeer;
    if (busy && havePending) target = pendingPeer;
    if (!target.isNull() && n < max) {
        const char* name = target == storedPeer ? storedName : "";
        int rssi = 0;
        for (const SeenDevice& s : seen) {
            if (!s.used || !(s.address == target)) continue;
            if (s.name[0]) name = s.name;
            if (now - s.lastSeen < kSeenExpiryMs) rssi = s.rssi;
        }
        add(target, name, rssi, link);
    }
    for (const SeenDevice& s : seen) {
        if (n >= max) break;
        if (!s.used || now - s.lastSeen >= kSeenExpiryMs || s.address == target) continue;
        add(s.address, s.name, s.rssi, LinkState::Idle);
    }
    return n;
}

void adapterSelectDevice(const DeviceEntry& device) {
    StateGuard guard;
    requestedPeer = NimBLEAddress(device.address, device.addressType);
    haveRequest = true;
}

void adapterDeselectDevice() {
    StateGuard guard;
    haveRequest = false;
    forgetRequested = true;
    forgetKeepsAutoPair = false;
}

void adapterSetDiscovery(bool on) { discovery = on; }

static void checkForgetButton() {
    if (kBootButtonPin < 0) return;
    static uint32_t pressedSince = 0;
    if (digitalRead(kBootButtonPin) == LOW) {
        if (!pressedSince) pressedSince = millis();
        if (millis() - pressedSince >= kForgetHoldMs) {
            {
                StateGuard guard;
                forgetRequested = true;
                forgetKeepsAutoPair = true;
            }
            pressedSince = 0;
            while (digitalRead(kBootButtonPin) == LOW) delay(10);
        }
    } else {
        pressedSince = 0;
    }
}

void setup() {
    Serial.begin(115200);
    if (kBootButtonPin >= 0) pinMode(kBootButtonPin, INPUT_PULLUP);

    stateLock = xSemaphoreCreateMutex();
    usbQueue = xQueueCreate(256, sizeof(UsbMidiPacket));
    usbMidi.begin();
    USB.begin();
#ifdef HAS_SCREEN
    uiBegin();
#endif
    xTaskCreate(usbTxTask, "usbTx", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr);

    loadPeer();

    NimBLEDevice::init(USB_PRODUCT);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bond, no MITM, secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, true);  // repeats keep the device list fresh
    scan->setActiveScan(true);                     // name / service UUID may be in the scan response
    scan->setDuplicateFilter(0);
    scan->setMaxResults(0);  // the callback is all that's used; keep nothing

    Serial.printf("Adapter up, peer: %s\n",
                  storedPeer.isNull() ? "(none, will pair with first BLE MIDI device)"
                                      : storedPeer.toString().c_str());

    xTaskCreate(bleTask, "ble", 6144, nullptr, 2, nullptr);
}

void loop() {
    checkForgetButton();
#ifdef HAS_SCREEN
    uiLoop();
    delay(5);
#else
    delay(20);
#endif
}
