#pragma once

#include <cstddef>

// What the screen needs from the BLE side. Safe to call from any task.

enum class LinkState { Idle, Searching, Connecting, Connected };

struct DeviceEntry {
    char address[18];
    unsigned char addressType;
    char name[32];
    int rssi;  // 0 when the device is not currently being heard
    LinkState state;  // Idle unless this is the selected device
};

LinkState adapterLinkState();

// The selected device first, if any, then other BLE MIDI devices in range.
size_t adapterListDevices(DeviceEntry* out, size_t max);

void adapterSelectDevice(const DeviceEntry& device);

// Drops and forgets the selected device, and stays unconnected until one is chosen.
void adapterDeselectDevice();

// Scanning continues while connected only when this is on.
void adapterSetDiscovery(bool on);
