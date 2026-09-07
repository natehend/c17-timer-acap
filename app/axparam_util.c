#include "axparam_util.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

void get_string_param(AXParameter* p, const char* name, char* dest, size_t destlen) {
    GError* error = NULL;
    gchar* value  = NULL;
    if (!ax_parameter_get(p, name, &value, &error)) {
        syslog(LOG_ERR, "Failed to read parameter %s: %s", name, error ? error->message : "?");
        if (error)
            g_error_free(error);
        dest[0] = '\0';
        return;
    }
    g_strlcpy(dest, value, destlen);
    g_free(value);
}

double get_double_param(AXParameter* p, const char* name, double def) {
    char buf[64];
    get_string_param(p, name, buf, sizeof(buf));
    if (buf[0] == '\0')
        return def;
    return atof(buf); // NOLINT
}

int get_int_param(AXParameter* p, const char* name, int def) {
    char buf[64];
    get_string_param(p, name, buf, sizeof(buf));
    if (buf[0] == '\0')
        return def;
    return atoi(buf); // NOLINT
}

bool get_bool_param(AXParameter* p, const char* name, bool def) {
    char buf[16];
    get_string_param(p, name, buf, sizeof(buf));
    if (buf[0] == '\0')
        return def;
    return (g_ascii_strcasecmp(buf, "yes") == 0) || (g_ascii_strcasecmp(buf, "true") == 0) ||
           (strcmp(buf, "1") == 0);
}

void set_string_param(AXParameter* p, const char* name, const char* value) {
    GError* error = NULL;
    if (!ax_parameter_set(p, name, value, TRUE, &error)) {
        syslog(LOG_ERR, "Failed to write parameter %s: %s", name, error ? error->message : "?");
        if (error)
            g_error_free(error);
    }
}
