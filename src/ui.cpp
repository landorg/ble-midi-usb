#ifdef HAS_SCREEN

#include "ui.h"

#include <M5Unified.h>

#include <cstring>

#include "MidiFormat.h"
#include "adapter.h"

static constexpr int kWidth = 320;
static constexpr int kHeaderHeight = 24;
static constexpr int kLineHeight = 16;
static constexpr int kMonitorLines = 13;
static constexpr int kColumns = 40;
static constexpr int kRowHeight = 42;
static constexpr int kDeviceRows = 5;
static constexpr uint32_t kMonitorRedrawMs = 100;
static constexpr uint32_t kListRefreshMs = 500;

static constexpr uint16_t kBackground = TFT_BLACK;
static constexpr uint16_t kDim = 0x8410;
static constexpr uint16_t kHeaderBackground = 0x18E3;
static constexpr uint16_t kConnectedBackground = 0x0220;
static constexpr uint16_t kRowBackground = 0x1082;

enum Page { PageMonitor, PageDevices };

static QueueHandle_t midiQueue;
static Page page = PageMonitor;
static bool screenOn = false;
static bool redrawAll = true;
static uint8_t brightness;

static UsbMidiPacket history[kMonitorLines];
static int historyCount = 0;
static bool monitorDirty = true;
static char shownLines[kMonitorLines][kColumns + 1];
static uint16_t shownColors[kMonitorLines];

static DeviceEntry devices[kDeviceRows];
static size_t deviceCount = 0;
static bool devicesLoaded = false;

static char shownStatus[kColumns + 1];

static uint16_t kindColor(MidiKind kind) {
    switch (kind) {
        case MidiKind::NoteOn: return TFT_GREEN;
        case MidiKind::NoteOff: return 0x03E0;
        case MidiKind::Control: return TFT_CYAN;
        case MidiKind::Bend: return TFT_YELLOW;
        case MidiKind::Pressure: return TFT_ORANGE;
        case MidiKind::Program: return TFT_MAGENTA;
        case MidiKind::SysEx: return TFT_PINK;
        default: return TFT_WHITE;
    }
}

static void drawHeader() {
    char name[32] = "";
    DeviceEntry first;
    if (adapterListDevices(&first, 1) && first.state != LinkState::Idle) {
        strcpy(name, first.name[0] ? first.name : first.address);
    }

    char status[kColumns + 1];
    switch (adapterLinkState()) {
        case LinkState::Connected: snprintf(status, sizeof(status), "%s", name); break;
        case LinkState::Connecting: snprintf(status, sizeof(status), "Connecting..."); break;
        case LinkState::Searching: snprintf(status, sizeof(status), "Searching..."); break;
        default: snprintf(status, sizeof(status), "No device selected"); break;
    }
    if (!redrawAll && !strcmp(status, shownStatus)) return;
    strcpy(shownStatus, status);

    auto& d = M5.Display;
    bool isConnected = adapterLinkState() == LinkState::Connected;
    d.fillRect(0, 0, kWidth, kHeaderHeight, kHeaderBackground);
    d.setTextColor(isConnected ? TFT_GREEN : TFT_WHITE, kHeaderBackground);
    d.setCursor(4, 4);
    d.print(status);
    d.fillCircle(kWidth - 26, 12, 4, page == PageMonitor ? TFT_WHITE : kDim);
    d.fillCircle(kWidth - 12, 12, 4, page == PageDevices ? TFT_WHITE : kDim);
}

static void drawMonitor() {
    auto& d = M5.Display;
    for (int row = 0; row < kMonitorLines; row++) {
        char line[kColumns + 1] = "";
        uint16_t color = TFT_WHITE;
        // Oldest at the top, and the newest on the last line that has anything.
        if (row < historyCount) {
            char text[kColumns + 1];
            color = kindColor(formatUsbMidi(history[row], text, sizeof(text)));
            strcpy(line, text);
        } else if (row == 0 && historyCount == 0) {
            strcpy(line, "No MIDI yet");
            color = kDim;
        }
        if (!redrawAll && color == shownColors[row] && !strcmp(line, shownLines[row])) continue;
        strcpy(shownLines[row], line);
        shownColors[row] = color;

        char padded[kColumns + 1];
        snprintf(padded, sizeof(padded), "%-*s", kColumns, line);
        d.setTextColor(color, kBackground);
        d.setCursor(0, kHeaderHeight + 2 + row * kLineHeight);
        d.print(padded);
    }
    monitorDirty = false;
}

static void drawDeviceRow(size_t row) {
    auto& d = M5.Display;
    const int y = kHeaderHeight + 2 + row * kRowHeight;
    const DeviceEntry& e = devices[row];
    uint16_t background = e.state == LinkState::Connected ? kConnectedBackground : kRowBackground;
    d.fillRoundRect(2, y, kWidth - 4, kRowHeight - 3, 5, background);

    const char* state = "";
    uint16_t stateColor = kDim;
    switch (e.state) {
        case LinkState::Connected: state = "Connected"; stateColor = TFT_GREEN; break;
        case LinkState::Connecting: state = "Connecting"; stateColor = TFT_YELLOW; break;
        case LinkState::Searching: state = "Searching"; stateColor = TFT_YELLOW; break;
        default: break;
    }
    d.setTextColor(TFT_WHITE, background);
    d.setCursor(8, y + 3);
    d.print(e.name[0] ? e.name : "(no name)");
    d.setTextColor(kDim, background);
    d.setCursor(8, y + 21);
    d.print(e.address);
    if (e.rssi) d.printf("  %d dBm", e.rssi);
    if (state[0]) {
        d.setTextColor(stateColor, background);
        d.setCursor(kWidth - 8 - 8 * static_cast<int>(strlen(state)), y + 21);
        d.print(state);
    }
}

static void refreshDevices(bool force) {
    static uint32_t lastRefresh = 0;
    uint32_t now = millis();
    if (!force && now - lastRefresh < kListRefreshMs) return;
    lastRefresh = now;

    DeviceEntry fresh[kDeviceRows];
    memset(fresh, 0, sizeof(fresh));
    size_t count = adapterListDevices(fresh, kDeviceRows);

    auto& d = M5.Display;
    if (force || count != deviceCount) {
        d.fillRect(0, kHeaderHeight, kWidth, 240 - kHeaderHeight, kBackground);
        memset(devices, 0, sizeof(devices));
        for (size_t i = 0; i < count; i++) {
            devices[i] = fresh[i];
            drawDeviceRow(i);
        }
        deviceCount = count;
        if (!count) {
            d.setTextColor(kDim, kBackground);
            d.setCursor(4, kHeaderHeight + 8);
            d.print("Looking for BLE MIDI devices...");
        }
    } else {
        for (size_t i = 0; i < count; i++) {
            if (!memcmp(&fresh[i], &devices[i], sizeof(DeviceEntry))) continue;
            devices[i] = fresh[i];
            drawDeviceRow(i);
        }
    }
    devicesLoaded = true;
}

static void setScreen(bool on) {
    screenOn = on;
    auto& d = M5.Display;
    if (on) {
        d.wakeup();
        d.setBrightness(brightness);
        redrawAll = true;
    } else {
        d.setBrightness(0);
        d.sleep();
    }
}

static void showPage(Page next) {
    if (page == next) return;
    page = next;
    redrawAll = true;
}

static void handleTouch() {
    if (!M5.Touch.getCount()) return;
    auto t = M5.Touch.getDetail();
    if (t.wasFlicked() && abs(t.distanceX()) > abs(t.distanceY())) {
        showPage(t.distanceX() < 0 ? PageDevices : PageMonitor);
        return;
    }
    if (page != PageDevices || !t.wasClicked()) return;

    int row = (t.y - kHeaderHeight - 2) / kRowHeight;
    if (t.y < kHeaderHeight + 2 || row >= static_cast<int>(deviceCount)) return;
    const DeviceEntry& e = devices[row];
    if (e.state == LinkState::Idle) {
        adapterSelectDevice(e);
    } else {
        adapterDeselectDevice();
    }
    refreshDevices(true);
}

static void drainMidi() {
    UsbMidiPacket pkt;
    while (xQueueReceive(midiQueue, &pkt, 0) == pdTRUE) {
        if (historyCount == kMonitorLines) {
            memmove(history, history + 1, sizeof(history[0]) * (kMonitorLines - 1));
            historyCount--;
        }
        history[historyCount++] = pkt;
        monitorDirty = true;
    }
}

void uiOnMidi(const UsbMidiPacket& pkt) {
    xQueueSend(midiQueue, &pkt, 0);
}

void uiBegin() {
    midiQueue = xQueueCreate(128, sizeof(UsbMidiPacket));

    auto cfg = M5.config();
    cfg.serial_baudrate = 0;  // the adapter has its own Serial set up
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    cfg.internal_imu = false;
    M5.begin(cfg);

    auto& d = M5.Display;
    d.setRotation(1);
    d.setFont(&fonts::AsciiFont8x16);
    d.setTextWrap(false);
    d.fillScreen(kBackground);
    brightness = d.getBrightness();
    if (!brightness) brightness = 100;
    setScreen(false);
}

void uiLoop() {
    static uint32_t lastMonitorRedraw = 0;
#ifdef LOG_STATUS
    static uint32_t lastLog = 0;
    if (millis() - lastLog >= 10000) {
        lastLog = millis();
        Serial.printf("board=%d screen=%d page=%d link=%d heap=%u\n", static_cast<int>(M5.getBoard()), screenOn, page,
                      static_cast<int>(adapterLinkState()), static_cast<unsigned>(ESP.getFreeHeap()));
    }
#endif
    M5.update();
    drainMidi();

    if (M5.BtnPWR.wasClicked()) {
        setScreen(!screenOn);
        Serial.printf("Screen %s\n", screenOn ? "on" : "off");
    }
    adapterSetDiscovery(screenOn && page == PageDevices);
    if (!screenOn) return;

    handleTouch();

    if (redrawAll) M5.Display.fillScreen(kBackground);
    drawHeader();
    uint32_t now = millis();
    if (page == PageMonitor) {
        if (redrawAll || (monitorDirty && now - lastMonitorRedraw >= kMonitorRedrawMs)) {
            lastMonitorRedraw = now;
            drawMonitor();
        }
    } else {
        refreshDevices(redrawAll);
    }
    redrawAll = false;
}

#endif  // HAS_SCREEN
