/**
 * webui - the app's whole user interface and HTTP API, served by a tiny
 * HTTP server running inside this same process.
 *
 * Same architecture as the C8310 Customizer, for the same
 * real-hardware-confirmed reasons: this does NOT go through Apache's
 * FastCGI or httpConfig CGI paths, both of which failed silently and
 * untraceably on a real device. It's a plain GSocketService, and running
 * in-process means it shares live timer state with the physical buttons
 * with no IPC and nothing to keep in sync.
 *
 * The socket binds to **loopback only**, so the sole route to it is
 * manifest.json's reverseProxy entry at /local/c17timer/ui - which is
 * declared admin-only, and therefore gives every page and every API call
 * the device's own HTTPS and login for free. Nothing here does its own
 * authentication, because nothing unauthenticated can arrive.
 *
 * Three inherited gotchas that shape every URL in here, all confirmed on
 * real hardware by the C8310 Customizer:
 *
 *  1. Apache's reverseProxy matches its apiPath EXACTLY - no subtree. A
 *     request to /local/c17timer/ui/settings never reaches this app at
 *     all. So every page and every API verb is a query parameter on the
 *     one exact path, never a second path segment.
 *  2. Apache forwards the full original path unmodified, producing a
 *     request line like "GET //local/c17timer/ui". The mount prefix is
 *     stripped here rather than assumed away.
 *  3. Pages reached through the proxy pick up Apache's
 *     Content-Security-Policy (script-src 'self'), which silently blocks
 *     inline <script> blocks and inline event-handler attributes. All
 *     JavaScript is therefore served as same-origin external scripts -
 *     and referenced by query string (?action=poll-js), not by a
 *     relative filename, because the proxied URL has no trailing slash
 *     and a bare filename resolves against the parent directory.
 */
#ifndef WEBUI_H
#define WEBUI_H

#include <axsdk/axparameter.h>

#include "local_config.h"

/**
 * Binds the web UI's port and starts serving.
 *
 * `cfg` is the app's single live configuration, borrowed not copied -
 * the settings page writes through it directly, then calls
 * `on_config_saved` so the rest of the app can react (re-clamp the
 * running timer, repaint the display) without this module needing to
 * know what any of that involves.
 */
void webui_start(AXParameter* axparam, timer_config_t* cfg, void (*on_config_saved)(void));

#endif // WEBUI_H
