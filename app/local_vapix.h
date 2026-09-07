/**
 * local_vapix - shared plumbing for calling VAPIX on THIS device from
 * inside the ACAP.
 *
 * Three different parts of this app need to talk to the local device's
 * own VAPIX endpoints: io_discovery.c (looking up which I/O port is
 * which C8310 button), display.c (driving the C17's display via the
 * Speaker display notification REST API) and mediaclip.c (listing and
 * playing the device's stored audio clips). They all authenticate the
 * same way, so the credential fetch and the two HTTP verbs live here
 * once instead of being copy-pasted three times.
 *
 * Credentials come from the platform's VAPIX service account over
 * D-Bus (com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials, which
 * manifest.json declares under resources.dbus.requiredMethods). Axis
 * documents that the account handed back has **admin** access level -
 * which matters, because the Speaker display notification API requires
 * admin. See:
 * https://developer.axis.com/acap/develop/VAPIX-access-for-ACAP-applications/
 *
 * All calls go to 127.0.0.12, the loopback address Axis reserves for
 * exactly this "ACAP talking to its own host's VAPIX" case - not
 * 127.0.0.1 and not the device's LAN IP.
 *
 * The credential string ("user:password", already in curl's userpwd
 * form) is fetched once and cached for the life of the process, since
 * the display path calls this roughly once a second while a timer is
 * running and a D-Bus round trip per tick would be pure waste.
 */
#ifndef LOCAL_VAPIX_H
#define LOCAL_VAPIX_H

#include <stdbool.h>
#include <stddef.h>

// Loopback address Axis reserves for an ACAP's calls to its own host.
#define LOCAL_VAPIX_HOST "127.0.0.12"

/**
 * Returns the cached "user:password" credential string for this app's
 * VAPIX service account, fetching it over D-Bus on first use. Returns
 * NULL if the fetch failed (a message is written to syslog); the
 * failure is not cached, so a later call retries - useful because this
 * app starts up before the platform is necessarily ready to answer.
 *
 * The returned string is owned by this module. Do not free it.
 */
const char* local_vapix_credentials(void);

/**
 * GET <path> from the local device. `path` is everything after the
 * host, starting with '/' (e.g. "/axis-cgi/playclip.cgi?clip=0").
 *
 * Returns a malloc'd response body on HTTP 200 (caller frees), or NULL
 * on any failure, writing a human-readable message into errbuf.
 */
char* local_vapix_get(const char* path, char* errbuf, size_t errlen);

/**
 * POST a JSON body to <path> on the local device, with
 * Content-Type: application/json.
 *
 * Returns a malloc'd response body (caller frees) for any 2xx, or NULL
 * on failure with a message in errbuf. Note this accepts the whole 2xx
 * range rather than 200 alone: the Speaker display notification REST
 * API answers 200, but sibling /config/rest endpoints have been seen to
 * answer 201/204, and there's no reason to treat those as errors.
 */
char* local_vapix_post_json(const char* path, const char* body, char* errbuf, size_t errlen);

#endif // LOCAL_VAPIX_H
