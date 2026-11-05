#include <unity.h>

#include <vector>

#include "BleMidiParser.h"

static std::vector<UsbMidiPacket> out;

static void collect(const UsbMidiPacket& pkt, void*) { out.push_back(pkt); }

static BleMidiParser parser(collect, nullptr);

static void parse(std::vector<uint8_t> bytes) { parser.parse(bytes.data(), bytes.size()); }

static void expectPacket(size_t i, uint8_t h, uint8_t a, uint8_t b, uint8_t c) {
    TEST_ASSERT_TRUE_MESSAGE(i < out.size(), "missing packet");
    uint8_t expected[4] = {h, a, b, c};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out[i].b, 4);
}

void setUp() {
    out.clear();
    parser.reset();
}

void tearDown() {}

void test_note_on() {
    parse({0x80, 0x80, 0x90, 0x3C, 0x64});
    TEST_ASSERT_EQUAL(1, out.size());
    expectPacket(0, 0x09, 0x90, 0x3C, 0x64);
}

void test_two_messages_with_timestamps() {
    parse({0x80, 0x81, 0x90, 0x3C, 0x64, 0x82, 0xB0, 0x07, 0x7F});
    TEST_ASSERT_EQUAL(2, out.size());
    expectPacket(0, 0x09, 0x90, 0x3C, 0x64);
    expectPacket(1, 0x0B, 0xB0, 0x07, 0x7F);
}

void test_running_status_without_timestamp() {
    parse({0x80, 0x80, 0x90, 0x3C, 0x64, 0x3E, 0x64});
    TEST_ASSERT_EQUAL(2, out.size());
    expectPacket(1, 0x09, 0x90, 0x3E, 0x64);
}

void test_running_status_with_timestamp() {
    parse({0x80, 0x80, 0xE0, 0x00, 0x40, 0x85, 0x10, 0x40});
    TEST_ASSERT_EQUAL(2, out.size());
    expectPacket(1, 0x0E, 0xE0, 0x10, 0x40);
}

void test_program_change_and_aftertouch() {
    parse({0x80, 0x80, 0xC1, 0x05, 0x80, 0xD2, 0x33});
    TEST_ASSERT_EQUAL(2, out.size());
    expectPacket(0, 0x0C, 0xC1, 0x05, 0x00);
    expectPacket(1, 0x0D, 0xD2, 0x33, 0x00);
}

void test_realtime() {
    parse({0x80, 0x80, 0xF8});
    TEST_ASSERT_EQUAL(1, out.size());
    expectPacket(0, 0x0F, 0xF8, 0x00, 0x00);
}

void test_sysex_across_packets() {
    parse({0x80, 0x80, 0xF0, 0x7E, 0x7F, 0x06});
    parse({0x80, 0x01, 0x80, 0xF7});
    TEST_ASSERT_EQUAL(2, out.size());
    expectPacket(0, 0x04, 0xF0, 0x7E, 0x7F);
    expectPacket(1, 0x07, 0x06, 0x01, 0xF7);
}

void test_sysex_with_interleaved_realtime() {
    parse({0x80, 0x80, 0xF0, 0x01, 0x80, 0xF8, 0x02, 0x80, 0xF7});
    TEST_ASSERT_EQUAL(3, out.size());
    expectPacket(0, 0x0F, 0xF8, 0x00, 0x00);
    expectPacket(1, 0x04, 0xF0, 0x01, 0x02);
    expectPacket(2, 0x05, 0xF7, 0x00, 0x00);
}

void test_invalid_header_ignored() {
    parse({0x40, 0x80, 0x90, 0x3C, 0x64});
    parse({0x80});
    TEST_ASSERT_EQUAL(0, out.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_note_on);
    RUN_TEST(test_two_messages_with_timestamps);
    RUN_TEST(test_running_status_without_timestamp);
    RUN_TEST(test_running_status_with_timestamp);
    RUN_TEST(test_program_change_and_aftertouch);
    RUN_TEST(test_realtime);
    RUN_TEST(test_sysex_across_packets);
    RUN_TEST(test_sysex_with_interleaved_realtime);
    RUN_TEST(test_invalid_header_ignored);
    return UNITY_END();
}
