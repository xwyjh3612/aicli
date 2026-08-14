package com.aitest.aicli;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

import java.nio.charset.StandardCharsets;
import java.util.List;

public final class TerminalWireCodecTest {
    @Test
    public void unpacksRawJsonEventsWithoutChangingText() throws Exception {
        String first = "{\"type\":\"terminal.data\",\"payload\":{\"data\":\"\\u001b[32m中文\"}}";
        String second = "{\"type\":\"terminal.resize\",\"sequence\":2}";
        byte[] firstBytes = first.getBytes(StandardCharsets.UTF_8);
        byte[] secondBytes = second.getBytes(StandardCharsets.UTF_8);
        int bodySize = 4 + firstBytes.length + 4 + secondBytes.length;
        byte[] packet = new byte[16 + bodySize];
        header(packet, 0, bodySize, 2);
        int position = 16;
        position = event(packet, position, firstBytes);
        event(packet, position, secondBytes);

        assertEquals(List.of(first, second), TerminalWireCodec.unpack(packet));
    }

    @Test
    public void unpacksOverlappingLz4Match() throws Exception {
        // Body is [length=10][aaaaaaaaaa]. The LZ4 sequence contains five
        // literals followed by a nine-byte overlapping offset-1 match.
        byte[] compressed = new byte[] {
                0x55, 0x0a, 0x00, 0x00, 0x00, 0x61, 0x01, 0x00, 0x00
        };
        byte[] packet = new byte[16 + compressed.length];
        header(packet, 1, 14, 1);
        System.arraycopy(compressed, 0, packet, 16, compressed.length);

        assertEquals(List.of("aaaaaaaaaa"), TerminalWireCodec.unpack(packet));
    }

    private static void header(byte[] packet, int flags, int bodySize, int eventCount) {
        packet[0] = 'A';
        packet[1] = 'I';
        packet[2] = 'C';
        packet[3] = 'L';
        packet[4] = 1;
        packet[5] = (byte) flags;
        writeInt(packet, 8, bodySize);
        writeInt(packet, 12, eventCount);
    }

    private static int event(byte[] packet, int position, byte[] value) {
        writeInt(packet, position, value.length);
        position += 4;
        System.arraycopy(value, 0, packet, position, value.length);
        return position + value.length;
    }

    private static void writeInt(byte[] output, int offset, int value) {
        output[offset] = (byte) value;
        output[offset + 1] = (byte) (value >>> 8);
        output[offset + 2] = (byte) (value >>> 16);
        output[offset + 3] = (byte) (value >>> 24);
    }
}
