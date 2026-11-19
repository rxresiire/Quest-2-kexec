package com.rxresiire.kexecinstaller;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.rxresiire.kexecinstaller.adb.AdbConnection;
import com.rxresiire.kexecinstaller.adb.AdbCrypto;
import com.rxresiire.kexecinstaller.install.KexecInstaller;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.GeneralSecurityException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {

    private static final int PICK_MODULE = 42;
    private static final String DEFAULT_ADDRESS = "192.168.100.18:33869";

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final SimpleDateFormat clock = new SimpleDateFormat("HH:mm:ss", Locale.ROOT);

    private EditText addressField;
    private TextView statusView;
    private TextView logView;
    private AdbConnection connection;
    private String host = "192.168.100.18";
    private String hostName = "rxresiire-kexec";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUi());
        log("RXRESIIRE KEXEC INSTALLER ready.");
        log("Scope: stages and validates the out-of-tree module. The stock kernel is built with");
        log("CONFIG_KEXEC=n, so no kernel handoff is performed and no boot path is modified.");
    }

    @Override
    protected void onDestroy() {
        if (connection != null) {
            connection.close();
        }
        worker.shutdownNow();
        super.onDestroy();
    }

    private LinearLayout buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("RXRESIIRE KEXEC INSTALLER");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView warning = new TextView(this);
        warning.setText("Quest 2 / Hollywood - staging only. Kernel 4.19.325-cip132-st16 ships "
                + "CONFIG_KEXEC=n, so kexec cannot be enabled by a module and this app never "
                + "attempts a kernel transition.");
        warning.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        warning.setTextColor(Color.rgb(150, 90, 0));
        root.addView(warning, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        addressField = new EditText(this);
        addressField.setHint("host:port");
        addressField.setText(DEFAULT_ADDRESS);
        addressField.setInputType(InputType.TYPE_CLASS_TEXT);
        addressField.setSingleLine(true);
        root.addView(addressField);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.addView(button("Connect", this::connect), weight());
        row.addView(button("Probe", this::probe), weight());
        row.addView(button("Root", this::checkRoot), weight());
        root.addView(row);

        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.addView(button("Install", this::pickModule), weight());
        row2.addView(button("Load", this::loadModule), weight());
        row2.addView(button("Uninstall", this::uninstall), weight());
        root.addView(row2);

        statusView = new TextView(this);
        statusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        statusView.setPadding(0, dp(8), 0, dp(8));
        statusView.setText("Status: disconnected");
        root.addView(statusView);

        logView = new TextView(this);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        logView.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(logView);
        LinearLayout.LayoutParams scrollParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(scroll, scrollParams);
        return root;
    }

    private Button button(String label, Runnable action) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setOnClickListener(view -> worker.execute(action));
        return button;
    }

    private static LinearLayout.LayoutParams weight() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
    }

    private void connect() {
        String value = addressField.getText().toString().trim();
        int colon = value.lastIndexOf(':');
        if (colon <= 0) {
            setStatus("Status: use host:port");
            return;
        }
        host = value.substring(0, colon);
        int port;
        try {
            port = Integer.parseInt(value.substring(colon + 1));
        } catch (NumberFormatException invalid) {
            setStatus("Status: invalid port");
            return;
        }
        setStatus("Status: connecting to " + host + ":" + port + " ...");
        try {
            if (connection != null) {
                connection.close();
                connection = null;
            }
            AdbCrypto crypto = AdbCrypto.loadOrCreate(getFilesDir());
            connection = AdbConnection.open(host, port, 8000, crypto, hostName);
            log("ADB handshake complete with " + host + ":" + port);
            setStatus("Status: connected (RSA auth accepted)");
            if (!connection.hasRoot()) {
                log("Note: Magisk su is not granted yet; press Root and approve the prompt.");
            }
        } catch (GeneralSecurityException | IOException failure) {
            log("Connect failed: " + describe(failure));
            setStatus("Status: connect failed");
        }
    }

    private void probe() {
        KexecInstaller installer = installerOrNull();
        if (installer == null) {
            return;
        }
        try {
            log("--- probe ---");
            log(installer.probe());
        } catch (IOException failure) {
            log("Probe failed: " + describe(failure));
        }
    }

    private void checkRoot() {
        if (installerOrNull() == null) {
            return;
        }
        try {
            boolean root = connection.hasRoot();
            log("Magisk root: " + (root ? "granted (uid 0)" : "denied"));
            setStatus("Status: " + (root ? "root granted" : "root denied"));
        } catch (IOException failure) {
            log("Root check failed: " + describe(failure));
        }
    }

    private void pickModule() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        main.post(() -> startActivityForResult(intent, PICK_MODULE));
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_MODULE || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        worker.execute(() -> installFromUri(uri));
    }

    private void installFromUri(Uri uri) {
        KexecInstaller installer = installerOrNull();
        if (installer == null) {
            return;
        }
        try {
            byte[] module = readAll(uri);
            log("Selected module: " + module.length + " bytes");
            String report = installer.stage(module, null);
            log(report);
            setStatus("Status: staged (not loaded)");
            log("Press Load to insmod in staging mode, or Uninstall to roll back.");
        } catch (IOException | RuntimeException failure) {
            log("Install failed: " + describe(failure));
            setStatus("Status: install failed");
        }
    }

    private void loadModule() {
        KexecInstaller installer = installerOrNull();
        if (installer == null) {
            return;
        }
        try {
            log(installer.loadStaged());
            setStatus("Status: module loaded (staging)");
        } catch (IOException failure) {
            log("Load failed: " + describe(failure));
            setStatus("Status: load failed");
        }
    }

    private void uninstall() {
        KexecInstaller installer = installerOrNull();
        if (installer == null) {
            return;
        }
        try {
            log(installer.uninstall());
            setStatus("Status: removed");
        } catch (IOException failure) {
            log("Uninstall failed: " + describe(failure));
            setStatus("Status: uninstall failed");
        }
    }

    private KexecInstaller installerOrNull() {
        if (connection == null) {
            log("Not connected; press Connect first.");
            setStatus("Status: not connected");
            return null;
        }
        return new KexecInstaller(connection);
    }

    private byte[] readAll(Uri uri) throws IOException {
        try (InputStream stream = getContentResolver().openInputStream(uri)) {
            if (stream == null) {
                throw new IOException("Cannot open " + uri);
            }
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            byte[] chunk = new byte[16384];
            int read;
            while ((read = stream.read(chunk)) > 0) {
                buffer.write(chunk, 0, read);
            }
            return buffer.toByteArray();
        }
    }

    private void log(String line) {
        main.post(() -> {
            if (logView == null) {
                return;
            }
            logView.append("[" + clock.format(new Date()) + "] " + line + "\n");
        });
    }

    private void setStatus(String text) {
        main.post(() -> {
            if (statusView != null) {
                statusView.setText(text);
            }
        });
    }

    private static String describe(Throwable failure) {
        String message = failure.getMessage();
        return message == null || message.isEmpty() ? failure.getClass().getSimpleName() : message;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
