#include "local_vapix.h"

#include <curl/curl.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "app_config.h" // APP_NAME

static void note_proxy_environment_once(void);

// Cached "user:password". Fetched lazily on first use and kept for the
// life of the process - see the header's doc comment for why (the
// display path would otherwise do a D-Bus round trip every second).
static char* g_credentials = NULL;

const char* local_vapix_credentials(void) {
    if (g_credentials)
        return g_credentials;

    GError* error               = NULL;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        syslog(LOG_WARNING, "local_vapix: D-Bus connect failed: %s", error ? error->message : "?");
        if (error)
            g_error_free(error);
        return NULL;
    }

    GVariant* result = g_dbus_connection_call_sync(connection,
                                                   "com.axis.HTTPConf1",
                                                   "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
                                                   "com.axis.HTTPConf1.VAPIXServiceAccounts1",
                                                   "GetCredentials",
                                                   g_variant_new("(s)", APP_NAME),
                                                   NULL,
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   -1,
                                                   NULL,
                                                   &error);
    g_object_unref(connection);
    if (!result) {
        syslog(LOG_WARNING, "local_vapix: GetCredentials failed: %s", error ? error->message : "?");
        if (error)
            g_error_free(error);
        return NULL;
    }

    const char* raw = NULL;
    g_variant_get(result, "(&s)", &raw);
    g_credentials = g_strdup(raw); // "user:password", already curl userpwd form
    g_variant_unref(result);

    syslog(LOG_INFO, "local_vapix: acquired VAPIX service account credentials");
    return g_credentials;
}

struct write_buf {
    char* data;
    size_t len;
};

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    struct write_buf* buf = userdata;
    size_t n              = size * nmemb;
    char* grown           = realloc(buf->data, buf->len + n + 1);
    if (!grown)
        return 0; // signals error to curl
    buf->data = grown;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

// Shared request path for both verbs below. json_body == NULL means a
// plain GET; non-NULL means a JSON POST.
//
// accept_any_2xx exists because the two callers have genuinely
// different expectations: the CGI endpoints (playclip.cgi, param.cgi)
// only ever answer 200, while /config/rest endpoints are free to answer
// 201/204 for a successful write. Treating "not exactly 200" as an
// error would make a perfectly successful display update look like a
// failure.
static char* local_vapix_request(const char* path,
                                 const char* json_body,
                                 bool accept_any_2xx,
                                 char* errbuf,
                                 size_t errlen) {
    const char* creds = local_vapix_credentials();
    if (!creds) {
        snprintf(errbuf, errlen, "no local VAPIX credentials (D-Bus lookup failed)");
        return NULL;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        snprintf(errbuf, errlen, "curl_easy_init failed");
        return NULL;
    }

    char url[1024];
    snprintf(url, sizeof(url), "http://%s%s", LOCAL_VAPIX_HOST, path);

    struct write_buf buf       = {0};
    struct curl_slist* headers = NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    // NEVER through a proxy. libcurl honors http_proxy / HTTP_PROXY /
    // ALL_PROXY from the environment by default, and a device with a
    // system HTTP proxy configured (System > Network) hands that
    // variable to every app it starts. Left alone, curl then sends this
    // request for the device's OWN loopback address out to the corporate
    // proxy, which cannot reach 127.0.0.12 and answers 503 - and every
    // display update and clip on that device fails with a Squid error
    // page while the app itself looks perfectly healthy. Seen on a
    // customer device in exactly that state. An empty proxy string is
    // libcurl's documented way to say "no proxy, whatever the
    // environment says".
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    note_proxy_environment_once();
    // The local service account is documented as supporting basic auth
    // on 127.0.0.12; ANY lets curl negotiate digest instead if a
    // firmware version prefers it, rather than hardcoding one and
    // breaking on the other.
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    curl_easy_setopt(curl, CURLOPT_USERPWD, creds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    // Short timeouts on purpose: this is a loopback call to the same
    // box, so anything slow is a fault, not latency - and the display
    // path runs on the main loop once a second, where a multi-second
    // stall would visibly freeze the countdown.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);

    if (json_body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        snprintf(errbuf, errlen, "curl error: %s", curl_easy_strerror(res));
        free(buf.data);
        if (headers)
            curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    bool ok = accept_any_2xx ? (http_code >= 200 && http_code < 300) : (http_code == 200);
    if (!ok) {
        snprintf(errbuf, errlen, "HTTP %ld: %s", http_code, buf.data ? buf.data : "(empty body)");
        free(buf.data);
        return NULL;
    }

    if (!buf.data) {
        // A 204-style empty body is a perfectly valid success - hand
        // back an empty string rather than NULL so callers don't have
        // to treat "succeeded with no content" as a failure.
        buf.data = calloc(1, 1);
    }
    return buf.data;
}

// Says once, in the app's own log, that a system proxy is present in the
// environment and is being bypassed for local calls. The value can carry
// a username and password (http://user:pass@proxy:3128), so only the
// part after any '@' is logged.
static void note_proxy_environment_once(void) {
    static bool noted = false;
    if (noted)
        return;
    noted = true;

    static const char* const vars[] = {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY",
                                       "all_proxy", "ALL_PROXY"};
    for (size_t i = 0; i < G_N_ELEMENTS(vars); i++) {
        const char* v = getenv(vars[i]);
        if (!v || !v[0])
            continue;
        const char* at   = strrchr(v, '@');
        const char* show = at ? at + 1 : v;
        syslog(LOG_INFO,
              "local_vapix: %s=%s%s is set in this app's environment (the device has a system "
              "HTTP proxy configured); it is bypassed for the device's own address " LOCAL_VAPIX_HOST,
              vars[i],
              at ? "<credentials>@" : "",
              show);
        return;
    }
}

char* local_vapix_get(const char* path, char* errbuf, size_t errlen) {
    return local_vapix_request(path, NULL, false, errbuf, errlen);
}

char* local_vapix_post_json(const char* path, const char* body, char* errbuf, size_t errlen) {
    return local_vapix_request(path, body, true, errbuf, errlen);
}
