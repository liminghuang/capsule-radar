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
#include <esp_heap_caps.h>

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

// NetworkClient::readBytes() treats a transient negative TLS read as end-of-input,
// which makes ArduinoJson intermittently report IncompleteInput. Deliberately wrap
// the client without overriding readBytes(): Stream's timed byte reader retries
// temporary no-data reads until the configured timeout.
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return _source.available(); }
    int read() override {
        const int value = _source.read();
        if (value >= 0) ++_bytesRead;
        return value;
    }
    int peek() override { return _source.peek(); }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    Stream& _source;
    size_t _bytesRead = 0;
};

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // Try each independent provider once. Retrying the primary immediately can violate its
    // one-request-per-second limit and adds another full timeout to an already slow failure.
    if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;
    if (fetchFrom(ADSB_FALLBACK_HOST, out)) return true;

    // Some ESP32 TLS fingerprints are rejected by the public API edge even though the
    // same endpoint works from a desktop client.  ADS-B data is public and contains no
    // device credentials, so keep a last-resort plaintext path for the known-compatible
    // primary provider. HTTPS remains preferred for both providers above.
    Serial.println("[adsb] HTTPS providers failed; trying adsb.lol over HTTP fallback");
    return fetchFrom(ADSB_PRIMARY_HOST, out, false);
}

bool AdsbClient::fetchFrom(const char* host, std::vector<Aircraft>& out, bool secure) {
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char url[160];
    snprintf(url, sizeof(url), "%s://%s/v2/point/%.4f/%.4f/%.0f",
             secure ? "https" : "http", host, _lat, _lon, nm);

    WiFiClientSecure secureClient;
    WiFiClient plainClient;
#if ADSB_HTTPS_INSECURE
    if (secure) secureClient.setInsecure();             // hobby: skip cert validation
#else
    // client.setCACert(ROOT_CA_PEM);                  // production: pin the root CA
#endif

    HTTPClient http;
    http.setReuse(false);
    http.useHTTP10(true);               // avoid proxy/API edges rejecting ESP32 HTTP/1.1
    http.setConnectTimeout(6000);    // fail reasonably fast: a slow host must not block the
    http.setTimeout(8000);           // task (and the user's route/photo lookups) for too long
    const bool begun = secure ? http.begin(secureClient, url)
                              : http.begin(plainClient, url);
    if (!begun) {
        Serial.printf("[adsb] begin failed (%s%s)\n", host, secure ? "" : " over HTTP");
        return false;
    }
    http.setUserAgent(ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");

    const int code = http.GET();
    if (code != 200) {
        Serial.printf("[adsb] HTTP %d (%s%s)\n", code, host, secure ? "" : " over HTTP");
        if (code > 0) {
            String errorBody = http.getString();
            if (errorBody.length() > 160) errorBody.remove(160);
            Serial.printf("[adsb] error body: %s\n", errorBody.c_str());
        }
        http.end();
        return false;
    }

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    const int expectedBytes = http.getSize();
    NetworkClient& responseStream = http.getStream();
    ReliableJsonStream jsonStream(responseStream);
    DeserializationError err = deserializeJson(doc, jsonStream,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[adsb] JSON parse failed (%s): %s; expected=%d read=%u available=%d connected=%d\n",
                      host, err.c_str(), expectedBytes, (unsigned)jsonStream.bytesRead(),
                      responseStream.available(), responseStream.connected());
        http.end();
        return false;
    }
    http.end();

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

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
