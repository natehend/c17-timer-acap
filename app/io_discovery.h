/**
 * io_discovery - best-effort auto-detection of which digital I/O "port"
 * number on THIS (local) device corresponds to each AXIS C8310 button.
 *
 * When the C8310 is wired into the C17's I/O connector (a 4-pin 2.5 mm
 * terminal block - the C8310 Volume Controller is a listed accessory
 * for the AXIS C1710), the speaker's firmware exposes each physical
 * button as its own numbered I/O port with a human-friendly "nice name"
 * (e.g. "Source 1 button", "Volume up button") - the same nice names you
 * see in the Port dropdown when building an Events rule in the device's
 * web UI.
 *
 * This module lists Input.NiceName.* parameters via the legacy param.cgi
 * on the local device (see local_vapix.h for how it authenticates) to
 * find the port number whose nice name contains a given
 * (case-insensitive) substring, e.g. "source 1" or "volume up".
 *
 * If this doesn't find a match (e.g. a firmware version that names ports
 * differently, or - much more commonly - ports whose Name field was
 * never actually filled in, since the grayed text shown there out of the
 * box is placeholder hint text rather than a saved value), the app falls
 * back to the manual Button1Port / Button2Port / Button3Port /
 * VolUpPort / VolDownPort / MutePort settings in the app's native
 * configuration page. Their defaults (1000-1005) are the numbering
 * confirmed on a real C1210 + C8310 pairing.
 */
#ifndef IO_DISCOVERY_H
#define IO_DISCOVERY_H

#include <stdbool.h>

/**
 * Look up the digital input port number whose nice name contains
 * `name_substring` (case-insensitive). Returns true and fills *out_port on
 * success, false if no match was found or the local lookup failed (in
 * which case a message is written to syslog).
 */
bool io_discover_port_by_name(const char* name_substring, int* out_port);

#endif // IO_DISCOVERY_H
