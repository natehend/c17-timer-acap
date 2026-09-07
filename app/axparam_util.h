/**
 * axparam_util - tiny helpers for reading/writing this app's AXParameter
 * values as strings/doubles/ints, shared between the main daemon
 * (main.c) and the web UI FastCGI handler (webui.c) so both read
 * configuration exactly the same way.
 */
#ifndef AXPARAM_UTIL_H
#define AXPARAM_UTIL_H

#include <axsdk/axparameter.h>
#include <stdbool.h>
#include <stddef.h>

void get_string_param(AXParameter* p, const char* name, char* dest, size_t destlen);
double get_double_param(AXParameter* p, const char* name, double def);
int get_int_param(AXParameter* p, const char* name, int def);
// Reads an ACAP "bool:no,yes" parameter. True for "yes"/"true"/"1"
// (case-insensitive), false otherwise (including unset/empty).
bool get_bool_param(AXParameter* p, const char* name, bool def);
void set_string_param(AXParameter* p, const char* name, const char* value);

#endif // AXPARAM_UTIL_H
