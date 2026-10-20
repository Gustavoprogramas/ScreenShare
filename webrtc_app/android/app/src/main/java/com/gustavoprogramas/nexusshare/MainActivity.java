package com.gustavoprogramas.nexusshare;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.media.AudioManager;
import android.net.Uri;
import android.net.http.SslError;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.PermissionRequest;
import android.webkit.SslErrorHandler;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.util.Locale;
import java.util.regex.Pattern;

/**
 * Nexus Share para Android: somente assistir.
 *
 * Abre a mesma página servida pelo PC que hospeda (porta 3000), dentro de uma WebView.
 * O certificado autoassinado do PC é aceito apenas para o endereço escolhido pelo usuário,
 * na porta 3000 — igual ao modo LAN/VPN do app Windows. Nenhuma permissão de câmera,
 * microfone ou captura de tela é concedida, então o celular não consegue transmitir.
 */
public class MainActivity extends Activity {
    private static final String LAUNCHER_URL = "file:///android_asset/launcher.html";
    private static final int PORT = 3000;
    private static final String PREFS = "nexus";
    private static final String KEY_HOST = "host";

    private static final Pattern IPV4 = Pattern.compile(
            "^((25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)\\.){3}(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)$");
    private static final Pattern IPV6 = Pattern.compile("^[0-9a-fA-F:.]{2,45}$");
    private static final Pattern HOSTNAME = Pattern.compile(
            "^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(\\.[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$");

    private WebView webView;
    private FrameLayout fullscreenContainer;
    private View customView;
    private WebChromeClient.CustomViewCallback customViewCallback;
    private SharedPreferences prefs;

    private String trustedHost;   // ex.: 100.101.102.103
    private String trustedOrigin; // ex.: https://100.101.102.103:3000
    private volatile String pendingError = "";

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.parseColor("#0F172A"));
        webView = new WebView(this);
        webView.setBackgroundColor(Color.parseColor("#0F172A"));
        fullscreenContainer = new FrameLayout(this);
        fullscreenContainer.setBackgroundColor(Color.BLACK);
        fullscreenContainer.setVisibility(View.GONE);
        root.addView(webView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(fullscreenContainer, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);

        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            WebView.setWebContentsDebuggingEnabled(true);
        }

        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setMediaPlaybackRequiresUserGesture(false);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setSupportZoom(false);
        settings.setSupportMultipleWindows(false);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        settings.setUserAgentString(settings.getUserAgentString() + " NexusShareAndroid/1.3");

        webView.addJavascriptInterface(new LauncherBridge(), "NexusAndroid");
        webView.setWebViewClient(new NexusWebViewClient());
        webView.setWebChromeClient(new NexusChromeClient());

        // Sempre começa na tela inicial (o IP salvo já vem preenchido).
        loadLauncher("");
    }

    // ---------------------------------------------------------------------------------
    // Navegação
    // ---------------------------------------------------------------------------------

    private void loadLauncher(String error) {
        pendingError = error == null ? "" : error;
        trustedHost = null;
        trustedOrigin = null;
        webView.loadUrl(LAUNCHER_URL);
    }

    private void openHost(String host) {
        trustedHost = host;
        String hostPart = host.contains(":") ? "[" + host + "]" : host;
        trustedOrigin = "https://" + hostPart + ":" + PORT;
        webView.loadUrl(trustedOrigin + "/?client=android");
    }

    private boolean isLauncher(String url) {
        if (url == null) return false;
        int hash = url.indexOf('#');
        return (hash >= 0 ? url.substring(0, hash) : url).equals(LAUNCHER_URL);
    }

    private static String stripBrackets(String host) {
        if (host == null) return "";
        if (host.startsWith("[") && host.endsWith("]")) host = host.substring(1, host.length() - 1);
        return host.toLowerCase(Locale.ROOT);
    }

    /** Somente o PC escolhido, na porta 3000, sobre HTTPS. */
    private boolean isTrusted(String url) {
        if (trustedHost == null || url == null) return false;
        try {
            Uri uri = Uri.parse(url);
            return "https".equals(uri.getScheme()) && uri.getPort() == PORT
                    && stripBrackets(uri.getHost()).equals(stripBrackets(trustedHost));
        } catch (RuntimeException e) {
            return false;
        }
    }

    static String normalizeHost(String input) {
        if (input == null) return null;
        String value = input.trim();
        value = value.replaceFirst("(?i)^https?://", "");
        int cut = value.length();
        for (char c : new char[]{'/', '?', '#'}) {
            int i = value.indexOf(c);
            if (i >= 0 && i < cut) cut = i;
        }
        value = value.substring(0, cut);
        if (value.startsWith("[")) {
            int end = value.indexOf(']');
            if (end < 0) return null;
            value = value.substring(1, end);
        } else if (value.indexOf(':') >= 0 && value.indexOf(':') == value.lastIndexOf(':')) {
            value = value.substring(0, value.indexOf(':')); // remove ":3000"
        }
        if (value.isEmpty() || value.length() > 253) return null;
        if (IPV4.matcher(value).matches()) return value;
        if (value.contains(":") && IPV6.matcher(value).matches()) return value.toLowerCase(Locale.ROOT);
        if (HOSTNAME.matcher(value).matches()) return value.toLowerCase(Locale.ROOT);
        return null;
    }

    // ---------------------------------------------------------------------------------
    // Ponte com a tela inicial (assets/launcher.html)
    // ---------------------------------------------------------------------------------

    private class LauncherBridge {
        @JavascriptInterface
        public String getSavedHost() {
            return prefs.getString(KEY_HOST, "");
        }

        @JavascriptInterface
        public String takeError() {
            String error = pendingError;
            pendingError = "";
            return error;
        }

        @JavascriptInterface
        public void connect(final String input) {
            runOnUiThread(() -> {
                // Só a tela inicial pode escolher o PC; a página remota não troca o destino.
                if (!isLauncher(webView.getUrl())) return;
                String host = normalizeHost(input);
                if (host == null) {
                    loadLauncher("Endereço inválido. Digite só o IP do PC, ex.: 100.101.102.103");
                    return;
                }
                prefs.edit().putString(KEY_HOST, host).apply();
                openHost(host);
            });
        }
    }

    // ---------------------------------------------------------------------------------
    // WebView
    // ---------------------------------------------------------------------------------

    private class NexusWebViewClient extends WebViewClient {
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            String url = request.getUrl().toString();
            return !(isLauncher(url) || isTrusted(url));
        }

        @SuppressLint("WebViewClientOnReceivedSslError")
        @Override
        public void onReceivedSslError(WebView view, SslErrorHandler handler, SslError error) {
            // O PC que hospeda usa certificado autoassinado. Aceito só para o IP escolhido:3000.
            if (isTrusted(error.getUrl())) handler.proceed();
            else handler.cancel();
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
            if (!request.isForMainFrame() || isLauncher(request.getUrl().toString())) return;
            String host = trustedHost == null ? "" : " (" + trustedHost + ")";
            loadLauncher("Não foi possível conectar ao PC" + host + ". Confira se ele está em "
                    + "\"Hospedar (LAN / VPN)\", se o Tailscale/ZeroTier está ligado no celular e no PC "
                    + "e se a porta TCP 3000 está liberada no firewall do Windows.");
        }
    }

    private class NexusChromeClient extends WebChromeClient {
        @Override
        public void onPermissionRequest(PermissionRequest request) {
            // Somente assistir: nada de câmera, microfone ou captura.
            request.deny();
        }

        @Override
        public void onShowCustomView(View view, CustomViewCallback callback) {
            if (customView != null) {
                callback.onCustomViewHidden();
                return;
            }
            customView = view;
            customViewCallback = callback;
            fullscreenContainer.addView(view, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            fullscreenContainer.setVisibility(View.VISIBLE);
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
            setSystemBarsHidden(true);
        }

        @Override
        public void onHideCustomView() {
            exitFullscreen();
        }
    }

    private void exitFullscreen() {
        if (customView == null) return;
        fullscreenContainer.removeView(customView);
        fullscreenContainer.setVisibility(View.GONE);
        customView = null;
        setSystemBarsHidden(false);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        if (customViewCallback != null) customViewCallback.onCustomViewHidden();
        customViewCallback = null;
    }

    @SuppressWarnings("deprecation")
    private void setSystemBarsHidden(boolean hidden) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller == null) return;
            if (hidden) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            } else {
                controller.show(WindowInsets.Type.systemBars());
            }
        } else {
            View decor = getWindow().getDecorView();
            decor.setSystemUiVisibility(hidden
                    ? View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    : View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    // ---------------------------------------------------------------------------------
    // Ciclo de vida
    // ---------------------------------------------------------------------------------

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        if (customView != null) {
            exitFullscreen();
        } else if (!isLauncher(webView.getUrl())) {
            loadLauncher(""); // volta para escolher outro PC
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        webView.onResume();
    }

    @Override
    protected void onPause() {
        webView.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        webView.destroy();
        super.onDestroy();
    }
}
