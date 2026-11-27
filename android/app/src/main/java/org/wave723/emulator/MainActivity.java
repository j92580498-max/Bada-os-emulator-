package org.wave723.emulator;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final int PICK_ZIP = 1;
    private static final long MAX_ZIP_BYTES = 512L * 1024 * 1024;

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private File firmware;
    private Button open;
    private Button trace;
    private ScrollView scroll;
    private TextView output;

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);

        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setBackgroundColor(Color.rgb(15, 23, 34));
        screen.setOnApplyWindowInsetsListener((view, insets) -> {
            screen.setPadding(dp(18) + insets.getSystemWindowInsetLeft(),
                    dp(14) + insets.getSystemWindowInsetTop(),
                    dp(18) + insets.getSystemWindowInsetRight(),
                    dp(14) + insets.getSystemWindowInsetBottom());
            return insets;
        });
        TextView title = new TextView(this);
        title.setText("Samsung Wave 723");
        title.setTextSize(26);
        title.setTextColor(Color.rgb(229, 239, 249));
        screen.addView(title);

        LinearLayout actions = new LinearLayout(this);
        open = new Button(this);
        open.setText("Open ZIP");
        open.setOnClickListener(button -> {
            Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            picker.addCategory(Intent.CATEGORY_OPENABLE);
            picker.setType("*/*");
            startActivityForResult(picker, PICK_ZIP);
        });
        actions.addView(open);
        trace = new Button(this);
        trace.setText("Trace reset");
        trace.setEnabled(false);
        trace.setOnClickListener(button -> process(null));
        actions.addView(trace);
        screen.addView(actions);

        scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(22, 34, 49));
        output = new TextView(this);
        output.setPadding(dp(12), dp(10), dp(12), dp(10));
        output.setTextColor(Color.rgb(211, 225, 239));
        output.setTypeface(Typeface.MONOSPACE);
        output.setTextSize(14);
        output.setTextIsSelectable(true);
        output.setText("Open a Wave 723 firmware ZIP to inspect its components.");
        scroll.addView(output);
        screen.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(screen);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_ZIP && result == RESULT_OK && data != null && data.getData() != null) {
            process(data.getData());
        }
    }

    private void process(Uri picked) {
        File current = firmware;
        boolean runTrace = picked == null;
        if (runTrace && current == null) return;
        String engine = new File(getApplicationInfo().nativeLibraryDir, "libwave723_qemu.so")
                .getAbsolutePath();
        open.setEnabled(false);
        trace.setEnabled(false);
        output.setText(runTrace ? "Tracing AMSS reset..." : "Opening firmware...");
        worker.execute(() -> {
            File candidate = null;
            String report;
            try {
                candidate = runTrace ? current : copyToCache(picked);
                report = Native.readFirmware(candidate.getAbsolutePath(), engine, runTrace);
            } catch (Exception error) {
                if (!runTrace && candidate != null) candidate.delete();
                candidate = null;
                report = error.getMessage();
                if (report == null) report = error.getClass().getSimpleName();
            }
            File loaded = candidate;
            String message = report;
            runOnUiThread(() -> {
                if (isDestroyed()) {
                    if (!runTrace && loaded != null) loaded.delete();
                    return;
                }
                if (!runTrace && loaded != null) {
                    if (firmware != null) firmware.delete();
                    firmware = loaded;
                }
                output.setText(message);
                scroll.scrollTo(0, 0);
                open.setEnabled(true);
                trace.setEnabled(firmware != null);
            });
        });
    }

    private File copyToCache(Uri uri) throws IOException {
        File copy = File.createTempFile("wave-firmware-", ".zip", getCacheDir());
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream destination = new FileOutputStream(copy)) {
            if (input == null) throw new IOException("Could not open firmware.");
            byte[] buffer = new byte[65536];
            long total = 0;
            for (int bytes; (bytes = input.read(buffer)) != -1;) {
                total += bytes;
                if (total > MAX_ZIP_BYTES) throw new IOException("Firmware ZIP exceeds 512 MiB.");
                destination.write(buffer, 0, bytes);
            }
            return copy;
        } catch (IOException | RuntimeException error) {
            copy.delete();
            throw error;
        }
    }

    private int dp(int pixels) {
        return Math.round(pixels * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        File current = firmware;
        if (current != null) worker.execute(current::delete);
        worker.shutdown();
        super.onDestroy();
    }
}
