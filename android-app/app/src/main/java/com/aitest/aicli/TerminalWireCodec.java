package com.aitest.aicli;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Decodes the optional packed-lz4-v1 WebSocket envelope. The envelope contains
 * the original JSON terminal events byte-for-byte; after unpacking, the app's
 * existing event and VT rendering paths remain unchanged.
 */
final class TerminalWireCodec {
    static final String TRANSPORT = "packed-lz4-v1";

    private static final int HEADER_BYTES = 16;
    private static final int FLAG_LZ4 = 0x01;
    private static final int MAXIMUM_BODY_BYTES = 4 * 1024 * 1024;

    private TerminalWireCodec() { }

    static List<String> unpack(byte[] packet) throws IOException {
        if (packet == null || packet.length < HEADER_BYTES ||
                packet[0] != 'A' || packet[1] != 'I' ||
                packet[2] != 'C' || packet[3] != 'L' ||
                unsigned(packet[4]) != 1 ||
                (unsigned(packet[5]) & ~FLAG_LZ4) != 0 ||
                packet[6] != 0 || packet[7] != 0) {
            throw new IOException("Unsupported terminal packet");
        }

        int bodySize = readInt(packet, 8);
        int eventCount = readInt(packet, 12);
        if (bodySize <= 0 || bodySize > MAXIMUM_BODY_BYTES ||
                eventCount <= 0 || eventCount > bodySize / 4) {
            throw new IOException("Invalid terminal packet size");
        }

        byte[] body;
        if ((unsigned(packet[5]) & FLAG_LZ4) != 0) {
            body = decompressLz4(packet, HEADER_BYTES,
                    packet.length - HEADER_BYTES, bodySize);
        } else {
            if (packet.length - HEADER_BYTES != bodySize) {
                throw new IOException("Invalid uncompressed terminal packet");
            }
            body = Arrays.copyOfRange(packet, HEADER_BYTES, packet.length);
        }

        List<String> events = new ArrayList<>(eventCount);
        int position = 0;
        for (int index = 0; index < eventCount; index++) {
            if (body.length - position < 4) {
                throw new IOException("Truncated terminal event length");
            }
            int length = readInt(body, position);
            position += 4;
            if (length < 0 || length > body.length - position) {
                throw new IOException("Truncated terminal event");
            }
            events.add(new String(body, position, length, StandardCharsets.UTF_8));
            position += length;
        }
        if (position != body.length) {
            throw new IOException("Trailing terminal packet data");
        }
        return events;
    }

    private static byte[] decompressLz4(byte[] input, int inputOffset,
                                        int inputLength, int outputLength)
            throws IOException {
        byte[] output = new byte[outputLength];
        int inputPosition = inputOffset;
        int inputEnd = inputOffset + inputLength;
        int outputPosition = 0;

        while (inputPosition < inputEnd) {
            int token = unsigned(input[inputPosition++]);
            int literalLength = token >>> 4;
            if (literalLength == 15) {
                LengthResult result = readExtendedLength(
                        input, inputPosition, inputEnd, literalLength);
                literalLength = result.length;
                inputPosition = result.position;
            }
            if (literalLength > inputEnd - inputPosition ||
                    literalLength > output.length - outputPosition) {
                throw new IOException("Invalid terminal literal");
            }
            System.arraycopy(input, inputPosition, output, outputPosition, literalLength);
            inputPosition += literalLength;
            outputPosition += literalLength;
            if (inputPosition == inputEnd) break;

            if (inputEnd - inputPosition < 2) {
                throw new IOException("Truncated terminal match");
            }
            int offset = unsigned(input[inputPosition]) |
                    (unsigned(input[inputPosition + 1]) << 8);
            inputPosition += 2;
            if (offset == 0 || offset > outputPosition) {
                throw new IOException("Invalid terminal match offset");
            }

            int matchLength = token & 0x0f;
            if (matchLength == 15) {
                LengthResult result = readExtendedLength(
                        input, inputPosition, inputEnd, matchLength);
                matchLength = result.length;
                inputPosition = result.position;
            }
            if (matchLength > MAXIMUM_BODY_BYTES - 4) {
                throw new IOException("Terminal match is too large");
            }
            matchLength += 4;
            if (matchLength > output.length - outputPosition) {
                throw new IOException("Terminal match exceeds output");
            }
            for (int index = 0; index < matchLength; index++) {
                output[outputPosition] = output[outputPosition - offset];
                outputPosition++;
            }
        }
        if (outputPosition != output.length) {
            throw new IOException("Incomplete terminal packet");
        }
        return output;
    }

    private static LengthResult readExtendedLength(byte[] input, int position,
                                                   int end, int initial)
            throws IOException {
        int length = initial;
        for (;;) {
            if (position >= end) throw new IOException("Truncated terminal length");
            int value = unsigned(input[position++]);
            if (length > MAXIMUM_BODY_BYTES - value) {
                throw new IOException("Terminal length is too large");
            }
            length += value;
            if (value != 255) return new LengthResult(position, length);
        }
    }

    private static int readInt(byte[] input, int offset) {
        return unsigned(input[offset]) |
                (unsigned(input[offset + 1]) << 8) |
                (unsigned(input[offset + 2]) << 16) |
                (unsigned(input[offset + 3]) << 24);
    }

    private static int unsigned(byte value) {
        return value & 0xff;
    }

    private static final class LengthResult {
        final int position;
        final int length;

        LengthResult(int position, int length) {
            this.position = position;
            this.length = length;
        }
    }
}
