package dev.invar;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Plain-JVM unit tests for {@link RawPremul}. */
public final class RawPremulTest {
    private static void check(boolean cond, String msg) {
        if (!cond) throw new AssertionError(msg);
    }

    private static byte[] makeBuf(int w, int h, int payloadLen) {
        byte[] b = new byte[16 + payloadLen];
        byte[] magic = {'M', 'B', 'R', 'A', 'W', 'P', 'R', 'E'};
        System.arraycopy(magic, 0, b, 0, 8);
        ByteBuffer.wrap(b, 8, 8).order(ByteOrder.LITTLE_ENDIAN).putInt(w).putInt(h);
        return b;
    }

    public static void main(String[] args) {
        // Non-magic inputs
        check(!RawPremul.isRawPremul(null), "null");
        check(!RawPremul.isRawPremul(new byte[0]), "empty");
        check(!RawPremul.isRawPremul(new byte[]{(byte) 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}), "png");
        check(!RawPremul.isRawPremul(new byte[]{'M', 'B', 'R', 'A', 'W', 'P', 'R'}), "short magic");

        // Valid payload
        byte[] ok = makeBuf(4, 2, 4 * 2 * 4);
        check(RawPremul.isRawPremul(ok), "valid magic");
        int[] dims = RawPremul.parseRawPremulHeader(ok);
        check(dims != null && dims[0] == 4 && dims[1] == 2, "valid dims");

        // Truncated header (< 16 bytes)
        byte[] truncHdr = {'M', 'B', 'R', 'A', 'W', 'P', 'R', 'E', 1, 0, 0};
        check(RawPremul.isRawPremul(truncHdr), "trunc hdr magic");
        check(RawPremul.parseRawPremulHeader(truncHdr) == null, "trunc hdr rejected");

        // Truncated pixel payload
        check(RawPremul.parseRawPremulHeader(makeBuf(2, 2, 12)) == null, "trunc pixels rejected");

        // Zero dimensions
        check(RawPremul.parseRawPremulHeader(makeBuf(0, 2, 0)) == null, "0 width rejected");
        check(RawPremul.parseRawPremulHeader(makeBuf(2, 0, 0)) == null, "0 height rejected");

        // Exceeds 16384 limit
        check(RawPremul.parseRawPremulHeader(makeBuf(16385, 1, 16385 * 4)) == null, ">16384 rejected");
        check(RawPremul.parseRawPremulHeader(makeBuf(1, 16385, 16385 * 4)) == null, ">16384 h rejected");

        // uint32 overflow (65536 * 65536 * 4 wraps to 0 in 32-bit math) & signed negative uint32
        check(RawPremul.parseRawPremulHeader(makeBuf(65536, 65536, 0)) == null, "uint32 overflow rejected");
        check(RawPremul.parseRawPremulHeader(makeBuf(-1, 1, 0)) == null, "negative uint32 rejected");

        // Trailing extra byte
        check(RawPremul.parseRawPremulHeader(makeBuf(1, 1, 5)) == null, "trailing byte rejected");

        // targetSize clamping & aspect preservation
        int[] s0 = RawPremul.targetSize(100, 300, 512);
        check(s0[0] == 100 && s0[1] == 300, "no upscale");
        int[] s1 = RawPremul.targetSize(1000, 3000, 512);
        check(s1[0] == 171 && s1[1] == 512, "portrait 512");
        int[] s2 = RawPremul.targetSize(4096, 2048, 2048);
        check(s2[0] == 2048 && s2[1] == 1024, "landscape 2048");
        int[] s3 = RawPremul.targetSize(1, 16000, 64);
        check(s3[0] == 1 && s3[1] == 64, "extreme aspect clamp");
        int[] s4 = RawPremul.targetSize(200, 100, 0);
        check(s4[0] == 200 && s4[1] == 100, "maxPx<=0 passthrough");

        // Area-averaging downscale: constant color + 4x and 3.2x 1px line preservation (matches raw_premul_test.c)
        byte[] solid = makeBuf(8, 4, 8 * 4 * 4);
        for (int i = 0; i < 8 * 4; i++) {
            int o = 16 + i * 4;
            solid[o] = 100; solid[o + 1] = 50; solid[o + 2] = 25; solid[o + 3] = (byte) 128;
        }
        byte[] solidOut = RawPremul.downscale(solid, 16, 8, 4, 4, 2);
        for (int i = 0; i < 4 * 2; i++) {
            int o = i * 4;
            check((solidOut[o] & 0xFF) == 100 && (solidOut[o + 1] & 0xFF) == 50
                    && (solidOut[o + 2] & 0xFF) == 25 && (solidOut[o + 3] & 0xFF) == 128,
                    "solid downscale");
        }
        byte[] lines = makeBuf(16, 4, 16 * 4 * 4);
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 16; x += 4) {
                int o = 16 + (y * 16 + x) * 4;
                lines[o] = (byte) 255; lines[o + 1] = (byte) 255;
                lines[o + 2] = (byte) 255; lines[o + 3] = (byte) 255;
            }
        }
        byte[] linesOut = RawPremul.downscale(lines, 16, 16, 4, 4, 1);
        for (int x = 0; x < 4; x++) {
            int o = x * 4;
            check((linesOut[o] & 0xFF) == 64 && (linesOut[o + 1] & 0xFF) == 64
                    && (linesOut[o + 2] & 0xFF) == 64 && (linesOut[o + 3] & 0xFF) == 64,
                    "4x 1px line preserved");
        }
        byte[] linesOut5 = RawPremul.downscale(lines, 16, 16, 4, 5, 1);
        int sumR = 0;
        for (int x = 0; x < 5; x++) {
            int o = x * 4;
            int r = linesOut5[o] & 0xFF;
            int g = linesOut5[o + 1] & 0xFF;
            int b = linesOut5[o + 2] & 0xFF;
            int a = linesOut5[o + 3] & 0xFF;
            check(r <= a && g <= a && b <= a, "3.2x premul rgb <= a");
            sumR += r;
        }
        check(sumR >= 314 && sumR <= 324, "3.2x 1px line energy preserved");

        // Exact integer half-up midpoint rounding: 98x1 -> 1x1 (49 of r=1 + 49 of r=2 => exact 1.5 => 2)
        byte[] src98 = makeBuf(98, 1, 98 * 4);
        for (int x = 0; x < 98; x++) {
            int o = 16 + x * 4;
            src98[o] = (byte) (x < 49 ? 1 : 2);
            src98[o + 3] = (byte) 255;
        }
        byte[] dst98 = RawPremul.downscale(src98, 16, 98, 1, 1, 1);
        check((dst98[0] & 0xFF) == 2 && (dst98[1] & 0xFF) == 0
                && (dst98[2] & 0xFF) == 0 && (dst98[3] & 0xFF) == 255,
                "98x1->1x1 exact half-up");

        // Exact integer midpoint overlap: 18x1 -> 11x1 (only x=8 has r=1 => dst x=5 overlap 9/18=0.5 => r=1, others 0)
        byte[] src18 = makeBuf(18, 1, 18 * 4);
        for (int x = 0; x < 18; x++) {
            int o = 16 + x * 4;
            src18[o] = (byte) (x == 8 ? 1 : 0);
            src18[o + 3] = (byte) 255;
        }
        byte[] dst11 = RawPremul.downscale(src18, 16, 18, 1, 11, 1);
        for (int x = 0; x < 11; x++) {
            int o = x * 4;
            check((dst11[o] & 0xFF) == (x == 5 ? 1 : 0) && (dst11[o + 1] & 0xFF) == 0
                    && (dst11[o + 2] & 0xFF) == 0 && (dst11[o + 3] & 0xFF) == 255,
                    "18x1->11x1 exact midpoint at x=" + x);
        }

        System.out.println("RawPremulTest: OK");
    }
}
