#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // v7
#include "santander.h"
#include "config.h"
#ifndef DOCK_DISPLAY_CONFIG
#error "config.h missing or out of date: copy config.example.h to config.h in this folder and fill it in"
#endif

static const char *TFL_BASE = "https://api.tfl.gov.uk";
static const char *GQL_URL = "https://santandercycles.tfl.gov.uk/bikesharefe-gql";
static const char *USER_AGENT = "london-dock-display/0.1";

// Only the fields the display needs, to keep the all-London answer small.
static const char *SUPPLY_QUERY =
    "query GetSystemSupply($input: SupplyInput) { supply(input: $input) { stations { "
    "stationId bikesAvailable bikeDocksAvailable ebikesAvailable totalBikesAvailable isOffline "
    "ebikes { batteryStatus { percent distanceRemaining { value unit } } } } } }";

// ------------------------------------------------------------------ helpers
static String urlEncode(const char *s) {
  String out;
  for (; *s; s++) {
    char c = *s;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char hex[4];
      snprintf(hex, sizeof hex, "%%%02X", (uint8_t)c);
      out += hex;
    }
  }
  return out;
}

static String tflUrl(const String &path) {
  String url = String(TFL_BASE) + path;
  if (sizeof(TFL_APP_KEY) > 1) url += String(path.indexOf('?') >= 0 ? "&" : "?") + "app_key=" + TFL_APP_KEY;
  return url;
}

// "BikePoints_42" -> 42
static int32_t bikePointNumber(const char *id) {
  const char *u = strrchr(id, '_');
  return (int32_t)atol(u ? u + 1 : id);
}

static bool startsWithNoCase(const char *s, const char *prefix) {
  for (; *prefix; s++, prefix++)
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
  return true;
}

// GET a small JSON body into a String. Returns the HTTP code, or <0.
static int httpGet(const String &url, String &body) {
  WiFiClientSecure client;
  // TODO(verify): pin the root CA instead of setInsecure() once it works.
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code;
}

// ------------------------------------------------------------------ TfL
bool tflSearch(const char *query, int32_t &id, char *name, int nameLen, char *err, int errLen) {
  String body;
  int code = httpGet(tflUrl(String("/BikePoint/Search?query=") + urlEncode(query)), body);
  if (code != 200) { snprintf(err, errLen, "search http %d", code); return false; }

  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["commonName"] = true;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (e) { snprintf(err, errLen, "search json %s", e.c_str()); return false; }

  JsonArray matches = doc.as<JsonArray>();
  JsonObject pick;
  for (JsonObject m : matches) {
    const char *n = m["commonName"] | "";
    Serial.printf("dock: '%s' matches %s \"%s\"\n", query, m["id"] | "?", n);
    if (pick.isNull() && startsWithNoCase(n, query)) pick = m;
  }
  if (pick.isNull() && matches.size() > 0) pick = matches[0].as<JsonObject>();
  if (pick.isNull()) { snprintf(err, errLen, "no dock '%.16s'", query); return false; }

  id = bikePointNumber(pick["id"] | "");
  strlcpy(name, pick["commonName"] | "", nameLen);
  if (id <= 0) { snprintf(err, errLen, "bad dock id"); return false; }
  return true;
}

bool tflBikePoint(int32_t id, Dock &out, char *err, int errLen) {
  String body;
  int code = httpGet(tflUrl(String("/BikePoint/BikePoints_") + id), body);
  if (code != 200) { snprintf(err, errLen, "tfl %ld http %d", (long)id, code); return false; }

  JsonDocument filter;
  filter["commonName"] = true;
  filter["additionalProperties"][0]["key"] = true;
  filter["additionalProperties"][0]["value"] = true;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (e) { snprintf(err, errLen, "tfl json %s", e.c_str()); return false; }

  int nbBikes = -1, nbStandard = -1, nbE = 0, nbEmpty = -1, nbDocks = -1;
  bool locked = false, installed = true;
  for (JsonObject p : doc["additionalProperties"].as<JsonArray>()) {
    const char *k = p["key"] | "";
    const char *v = p["value"] | "";
    if (!strcmp(k, "NbBikes")) nbBikes = atoi(v);
    else if (!strcmp(k, "NbStandardBikes")) nbStandard = atoi(v);
    else if (!strcmp(k, "NbEBikes")) nbE = atoi(v);
    else if (!strcmp(k, "NbEmptyDocks")) nbEmpty = atoi(v);
    else if (!strcmp(k, "NbDocks")) nbDocks = atoi(v);
    else if (!strcmp(k, "Locked")) locked = !strcasecmp(v, "true");
    else if (!strcmp(k, "Installed")) installed = !strcasecmp(v, "true");
  }
  if (nbBikes < 0 || nbEmpty < 0) { snprintf(err, errLen, "tfl %ld no counts", (long)id); return false; }

  out.id = id;
  strlcpy(out.name, doc["commonName"] | "", sizeof out.name);
  out.hasCounts = true;
  out.closed = locked || !installed;
  // NbBikes counts e-bikes too; NbStandardBikes is the classic ones.
  out.bikes = nbStandard >= 0 ? nbStandard : max(0, nbBikes - nbE);
  out.ebikes = nbE;
  out.spaces = nbEmpty;
  out.docks = nbDocks;
  return true;
}

// ------------------------------------------------------------------ GraphQL
// Reads an HTTP body as a plain or chunked stream, a buffer at a time, so
// ArduinoJson can parse one station at a time without holding the whole
// answer in RAM. useHTTP10() should stop the server chunking; this handles
// it anyway in case it doesn't.
class BodyStream : public Stream {
 public:
  BodyStream(Client &c, bool chunked, uint32_t timeoutMs) : c_(c), chunked_(chunked), wait_(timeoutMs) {}
  int available() override { return (peeked_ >= 0 || pos_ < len_) ? 1 : c_.available(); }
  int read() override {
    if (peeked_ >= 0) { int c = peeked_; peeked_ = -1; return c; }
    return next();
  }
  int peek() override {
    if (peeked_ < 0) peeked_ = next();
    return peeked_;
  }
  size_t write(uint8_t) override { return 0; }

 private:
  int raw() {
    if (pos_ < len_) return buf_[pos_++];
    uint32_t start = millis();
    for (;;) {
      int avail = c_.available();
      if (avail > 0) {
        int n = c_.read(buf_, avail < (int)sizeof buf_ ? avail : (int)sizeof buf_);
        if (n > 0) { len_ = n; pos_ = 0; return buf_[pos_++]; }
      } else if (!c_.connected()) {
        return -1;
      }
      if (millis() - start > wait_) return -1;
      delay(1);
    }
  }
  int next() {
    if (!chunked_) return raw();
    if (left_ == 0) {
      if (done_) return -1;
      if (started_ && (raw() != '\r' || raw() != '\n')) { done_ = true; return -1; }
      started_ = true;
      long size = 0;
      bool digits = false, ext = false;
      int c;
      while ((c = raw()) >= 0 && c != '\r') {
        if (c == ';') ext = true;
        if (ext || !isxdigit(c)) continue;
        size = size * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
        digits = true;
      }
      if (c < 0 || raw() != '\n' || !digits || size == 0) { done_ = true; return -1; }
      left_ = size;
    }
    int c = raw();
    if (c < 0) { done_ = true; return -1; }
    left_--;
    return c;
  }

  Client &c_;
  bool chunked_;
  uint32_t wait_;
  uint8_t buf_[512];
  int pos_ = 0, len_ = 0;
  int peeked_ = -1;
  long left_ = 0;
  bool started_ = false, done_ = false;
};

static bool findText(Stream &s, const char *pat) {
  size_t n = strlen(pat), m = 0;
  for (;;) {
    int c = s.read();
    if (c < 0) return false;
    if (c == pat[m]) { if (++m == n) return true; }
    else m = (c == pat[0]) ? 1 : 0;
  }
}

static int peekNonSpace(Stream &s) {
  for (;;) {
    int c = s.peek();
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { s.read(); continue; }
    return c;
  }
}

// GraphQL stationId is "42" (a string), but take a number too.
static int32_t stationNumber(const JsonDocument &st) {
  if (st["stationId"].is<const char *>()) return bikePointNumber(st["stationId"].as<const char *>());
  return st["stationId"].as<int32_t>();
}

static void fillFromStation(Dock &d, JsonObjectConst st) {
  int bikes = st["bikesAvailable"] | 0;
  int e = st["ebikesAvailable"] | 0;
  int total = st["totalBikesAvailable"] | -1;
  d.hasCounts = true;
  d.closed = st["isOffline"] | false;
  // bikesAvailable looks like classic bikes only; if it equals the total,
  // it included the e-bikes.
  d.bikes = (total >= 0 && total == bikes && e > 0) ? bikes - e : bikes;
  d.ebikes = e;
  d.spaces = st["bikeDocksAvailable"] | 0;
  d.docks = -1;

  d.hasBatt = true;
  d.nBatt = 0;
  d.rangeUnit[0] = 0;
  for (JsonObjectConst eb : st["ebikes"].as<JsonArrayConst>()) {
    JsonObjectConst b = eb["batteryStatus"];
    float range = b["distanceRemaining"]["value"] | 0.0f;
    addBatt(d, b["percent"] | 0, (int)(range + 0.5f));
    if (!d.rangeUnit[0]) {
      const char *u = b["distanceRemaining"]["unit"] | "";
      if (tolower((unsigned char)u[0]) == 'm') strcpy(d.rangeUnit, "mi");
      else if (tolower((unsigned char)u[0]) == 'k') strcpy(d.rangeUnit, "km");
    }
  }
}

bool fetchSupply(const int32_t *ids, Dock *out, int n, char *err, int errLen) {
  WiFiClientSecure client;
  // TODO(verify): pin the root CA instead of setInsecure() once it works.
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true); // no chunked encoding, so the body can be streamed
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  const char *headers[] = {"Transfer-Encoding"};
  http.collectHeaders(headers, 1);
  if (!http.begin(client, GQL_URL)) { snprintf(err, errLen, "gql begin"); return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  String body = String("{\"query\":\"") + SUPPLY_QUERY +
                "\",\"variables\":{\"input\":{\"regionCode\":\"london\"}}}";
  int code = http.POST(body);
  if (code != 200) {
    snprintf(err, errLen, "gql http %d", code);
    http.end();
    return false;
  }
  bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
  BodyStream in(http.getStream(), chunked, HTTP_TIMEOUT_MS);

  // {"data":{"supply":{"stations":[{...},{...}]}}}: skip to the array, then
  // parse one station at a time and keep ours.
  bool ok = false;
  bool got[MAX_SLOTS] = {};
  int found = 0, seen = 0, want = 0;
  if (n > MAX_SLOTS) n = MAX_SLOTS;
  for (int i = 0; i < n; i++) if (ids[i] > 0) want++;
  if (!findText(in, "\"stations\"")) {
    snprintf(err, errLen, "gql no stations");
  } else {
    int c = peekNonSpace(in);
    if (c == ':') { in.read(); c = peekNonSpace(in); }
    if (c != '[') { snprintf(err, errLen, "gql stations not list"); }
    else {
      in.read();
      for (;;) {
        c = peekNonSpace(in);
        if (c == ']') { ok = true; break; }
        if (c != '{') { snprintf(err, errLen, "gql json at %d", seen); break; }
        JsonDocument st;
        DeserializationError e = deserializeJson(st, in);
        if (e) { snprintf(err, errLen, "gql json %s", e.c_str()); break; }
        seen++;
        int32_t sid = stationNumber(st);
        for (int i = 0; i < n; i++) {
          if (got[i] || ids[i] <= 0 || ids[i] != sid) continue;
          fillFromStation(out[i], st.as<JsonObjectConst>());
          got[i] = true;
          found++;
        }
        if (found == want) { ok = true; break; } // the rest isn't needed
        c = peekNonSpace(in);
        if (c == ',') { in.read(); continue; }
        if (c == ']') { ok = true; break; }
        snprintf(err, errLen, "gql json after %d", seen);
        break;
      }
    }
  }
  http.end();
  Serial.printf("gql: read %d stations, found %d of %d\n", seen, found, want);
  return ok;
}
