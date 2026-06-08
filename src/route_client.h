#pragma once
// Look up a flight's origin/destination by callsign via adsbdb.com (free, no key).
// Device-only (uses WiFi/HTTPS). City names are returned in English.
#include <stddef.h>

bool route_fetch(const char *callsign, char *from, size_t fn, char *to, size_t tn);
