// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept, and hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar
// then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;

    const char* host = _useFallback ? ADSB_FALLBACK_HOST : ADSB_PRIMARY_HOST;
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char url[160];
    snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, _lat, _lon, nm);

    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure();                              // hobby: skip cert validation
#else
    // client.setCACert(ROOT_CA_PEM);                  // production: pin the root CA
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4000);
    http.setTimeout(8000);
    if (!http.begin(client, url)) { _useFallback = !_useFallback; return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    if (code != 200) { http.end(); _useFallback = !_useFallback; return false; }

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter;
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) { _useFallback = !_useFallback; return false; }

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) { _useFallback = !_useFallback; return false; }

    std::vector<Aircraft> tmp;
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) break;   // hard cap: protect RAM

        if (a["lat"].isNull() || a["lon"].isNull()) continue;  // need a position

        Aircraft ac;
        ac.hex    = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat    = a["lat"].as<double>();
        ac.lon    = a["lon"].as<double>();

        if (a["alt_baro"].is<const char*>()) { ac.onGround = true; ac.altBaro = 0; }  // "ground"
        else                                  ac.altBaro = a["alt_baro"] | 0.0f;

        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        tmp.push_back(std::move(ac));
    }

    out.swap(tmp);
    _lastOkMs = now;
    return true;
}
