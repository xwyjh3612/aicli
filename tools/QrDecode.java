import com.google.zxing.BinaryBitmap;
import com.google.zxing.LuminanceSource;
import com.google.zxing.MultiFormatReader;
import com.google.zxing.common.HybridBinarizer;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.File;

/** Small build/test helper for decoding the desktop pairing QR screenshot. */
public final class QrDecode {
    private static final class ImageSource extends LuminanceSource {
        private final byte[] values;

        ImageSource(BufferedImage image) {
            super(image.getWidth(), image.getHeight());
            values = new byte[image.getWidth() * image.getHeight()];
            for (int y = 0; y < image.getHeight(); y++) {
                for (int x = 0; x < image.getWidth(); x++) {
                    int rgb = image.getRGB(x, y);
                    int red = (rgb >>> 16) & 0xff;
                    int green = (rgb >>> 8) & 0xff;
                    int blue = rgb & 0xff;
                    values[y * image.getWidth() + x] =
                            (byte) ((red * 30 + green * 59 + blue * 11) / 100);
                }
            }
        }

        @Override public byte[] getRow(int y, byte[] row) {
            int width = getWidth();
            if (row == null || row.length < width) row = new byte[width];
            System.arraycopy(values, y * width, row, 0, width);
            return row;
        }

        @Override public byte[] getMatrix() { return values; }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("usage: QrDecode image.png");
        BufferedImage image = ImageIO.read(new File(args[0]));
        if (image == null) throw new IllegalArgumentException("unsupported image");
        System.out.print(new MultiFormatReader()
                .decode(new BinaryBitmap(new HybridBinarizer(new ImageSource(image))))
                .getText());
    }
}
