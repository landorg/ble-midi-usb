#include <unity.h>

#include "MidiFormat.h"

static void expectLine(UsbMidiPacket pkt, MidiKind kind, const char* text) {
    char buf[48];
    TEST_ASSERT_EQUAL(static_cast<int>(kind), static_cast<int>(formatUsbMidi(pkt, buf, sizeof(buf))));
    TEST_ASSERT_EQUAL_STRING(text, buf);
}

void setUp() {}
void tearDown() {}

void test_notes() {
    expectLine({{0x09, 0x90, 60, 100}}, MidiKind::NoteOn, "Ch 1  Note On   C4   vel 100");
    expectLine({{0x09, 0x9F, 61, 1}}, MidiKind::NoteOn, "Ch 16 Note On   C#4  vel 1");
    expectLine({{0x08, 0x83, 0, 0}}, MidiKind::NoteOff, "Ch 4  Note Off  C-1 ");
    expectLine({{0x09, 0x93, 127, 0}}, MidiKind::NoteOff, "Ch 4  Note Off  G9  ");
}

void test_controllers_and_expression() {
    expectLine({{0x0B, 0xB1, 74, 64}}, MidiKind::Control, "Ch 2  CC 74     64");
    expectLine({{0x0E, 0xE1, 0, 64}}, MidiKind::Bend, "Ch 2  Bend     +0");
    expectLine({{0x0E, 0xE1, 0, 0}}, MidiKind::Bend, "Ch 2  Bend     -8192");
    expectLine({{0x0E, 0xE1, 127, 127}}, MidiKind::Bend, "Ch 2  Bend     +8191");
    expectLine({{0x0D, 0xD2, 73, 0}}, MidiKind::Pressure, "Ch 3  Pressure 73");
    expectLine({{0x0A, 0xA0, 60, 9}}, MidiKind::Pressure, "Ch 1  Poly AT   C4   9");
    expectLine({{0x0C, 0xC0, 5, 0}}, MidiKind::Program, "Ch 1  Program  5");
}

void test_sysex() {
    expectLine({{0x04, 0xF0, 0x7E, 0x7F}}, MidiKind::SysEx, "SysEx F0 7E 7F");
    expectLine({{0x05, 0xF7, 0, 0}}, MidiKind::SysEx, "SysEx F7");
    expectLine({{0x06, 0x01, 0xF7, 0}}, MidiKind::SysEx, "SysEx 01 F7");
    expectLine({{0x07, 0x06, 0x01, 0xF7}}, MidiKind::SysEx, "SysEx 06 01 F7");
}

void test_system() {
    expectLine({{0x0F, 0xF8, 0, 0}}, MidiKind::System, "Clock");
    expectLine({{0x05, 0xF6, 0, 0}}, MidiKind::System, "Tune Request");
    expectLine({{0x03, 0xF2, 1, 1}}, MidiKind::System, "Song Position 129");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_notes);
    RUN_TEST(test_controllers_and_expression);
    RUN_TEST(test_sysex);
    RUN_TEST(test_system);
    return UNITY_END();
}
