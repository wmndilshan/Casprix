import java.io.*;
import java.net.*;
import java.util.zip.*;

/**
 * Downloads the GN (Generate Ninja) binary for Windows.
 * GN is Skia's build system — needed to configure the ARM64 build.
 */
public class DownloadGN {
    public static void main(String[] args) throws Exception {
        String outDir = args.length > 0 ? args[0] : ".";
        File gnExe = new File(outDir, "gn.exe");

        if (gnExe.exists()) {
            System.out.println("GN already exists: " + gnExe.getAbsolutePath());
            return;
        }

        // GN Windows binary from Chrome infrastructure
        String url = "https://chrome-infra-packages.appspot.com/dl/gn/gn/windows-amd64/+/latest";

        System.out.println("Downloading GN for Windows...");
        System.out.println("URL: " + url);

        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setConnectTimeout(30_000);
        conn.setReadTimeout(120_000);
        conn.setInstanceFollowRedirects(true);
        conn.setRequestProperty("User-Agent", "Java/Skia-Build");

        int code = conn.getResponseCode();
        System.out.println("HTTP " + code);

        if (code == 200) {
            // Response is a ZIP file containing gn.exe
            File tmpZip = new File(outDir, "gn-tmp.zip");
            try (InputStream in = conn.getInputStream();
                 FileOutputStream fos = new FileOutputStream(tmpZip)) {
                byte[] buf = new byte[8192];
                int n;
                long total = 0;
                while ((n = in.read(buf)) != -1) {
                    fos.write(buf, 0, n);
                    total += n;
                }
                System.out.printf("Downloaded %,d bytes%n", total);
            }

            // Extract gn.exe from ZIP
            try (ZipInputStream zis = new ZipInputStream(new FileInputStream(tmpZip))) {
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    if (entry.getName().equals("gn.exe") || entry.getName().endsWith("/gn.exe")) {
                        try (FileOutputStream fos = new FileOutputStream(gnExe)) {
                            byte[] buf = new byte[8192];
                            int n;
                            while ((n = zis.read(buf)) != -1) {
                                fos.write(buf, 0, n);
                            }
                        }
                        System.out.println("Extracted: " + gnExe.getAbsolutePath());
                        break;
                    }
                }
            }
            tmpZip.delete();

            if (gnExe.exists()) {
                System.out.println("SUCCESS: " + gnExe.getAbsolutePath());
            } else {
                System.out.println("ERROR: gn.exe not found in archive");
                System.exit(1);
            }
        } else {
            System.out.println("FAILED: HTTP " + code);
            System.exit(1);
        }
    }
}
