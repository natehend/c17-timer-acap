#include "io_discovery.h"

#include <ctype.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "local_vapix.h"

static char* to_lower_dup(const char* s) {
    char* out = g_strdup(s);
    for (char* p = out; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return out;
}

bool io_discover_port_by_name(const char* name_substring, int* out_port) {
    // Input.NiceName.I<N>=<nice name> is the classic per-port label.
    char err[256] = {0};
    char* list    = local_vapix_get("/axis-cgi/param.cgi?action=list&group=Input.NiceName", err, sizeof(err));
    if (!list) {
        syslog(LOG_WARNING, "io_discovery: local param.cgi list failed: %s", err);
        return false;
    }

    char* needle = to_lower_dup(name_substring);
    bool found   = false;
    int port     = -1;

    char* saveptr = NULL;
    char* line    = strtok_r(list, "\n", &saveptr);
    while (line) {
        char* eq = strchr(line, '=');
        if (eq) {
            *eq             = '\0';
            const char* key = line;
            char* value     = eq + 1;
            // strip trailing \r and surrounding quotes, if any
            size_t vlen = strlen(value);
            while (vlen > 0 && (value[vlen - 1] == '\r' || value[vlen - 1] == '\n'))
                value[--vlen] = '\0';
            if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
                value[vlen - 1] = '\0';
                value++;
            }

            char* value_lower = to_lower_dup(value);
            if (strstr(value_lower, needle)) {
                // key looks like "root.Input.NiceName.I3"; pull the trailing digits.
                const char* i_marker = strrchr(key, 'I');
                if (i_marker && isdigit((unsigned char)i_marker[1])) {
                    port  = atoi(i_marker + 1); // NOLINT
                    found = true;
                }
            }
            g_free(value_lower);
        }
        if (found)
            break;
        line = strtok_r(NULL, "\n", &saveptr);
    }

    g_free(needle);
    free(list);

    if (found) {
        *out_port = port;
        syslog(LOG_INFO, "io_discovery: matched '%s' to I/O port %d", name_substring, port);
    } else {
        syslog(LOG_INFO,
              "io_discovery: no I/O port nice name contained '%s' - "
              "set the matching *Port setting manually in the app's configuration page",
              name_substring);
    }
    return found;
}
