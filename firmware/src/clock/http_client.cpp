#include "http_client.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include "ca_roots.h"

namespace clockhttp {

namespace {

class FuncSink : public Stream {
public:
    FuncSink(SinkFn fn, void* ctx) : _fn(fn), _ctx(ctx) {}
    size_t bytes() const { return _bytes; }
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* buf, size_t n) override {
        if (_fn) _fn(buf, n, _ctx);
        _bytes += n;
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }

private:
    SinkFn _fn;
    void* _ctx;
    size_t _bytes = 0;
};

String s_customCa;          // /ca.pem, loaded once
bool s_customCaLoaded = false;

const char* customCa() {
    if (!s_customCaLoaded) {
        s_customCaLoaded = true;
        if (LittleFS.exists("/ca.pem")) {
            File f = LittleFS.open("/ca.pem", "r");
            if (f) { s_customCa = f.readString(); f.close(); }
        }
    }
    return s_customCa.length() ? s_customCa.c_str() : nullptr;
}

}  // namespace

String encodePath(const String& path) {
    String out;
    out.reserve(path.length() + 8);
    for (size_t i = 0; i < path.length(); i++) {
        const uint8_t c = (uint8_t)path[i];
        const bool keep = isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (keep) out += (char)c;
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); out += b; }
    }
    return out;
}

Result perform(const Request& req, SinkFn sink, void* ctx) {
    Result res;
    const bool https = req.url.startsWith("https://");

    WiFiClientSecure secure;
    WiFiClient plain;
    WiFiClient* client = &plain;
    if (https) {
        if (req.insecure) secure.setInsecure();
        else if (const char* ca = customCa()) secure.setCACert(ca);
        else secure.setCACert(kCaRoots);
        secure.setTimeout((req.timeoutMs + 999) / 1000);
        client = &secure;
    }

    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(req.timeoutMs);
    http.setReuse(false);
    http.setUserAgent("ARC-Clock/" ARC_FW_VERSION);
    http.useHTTP10(false);
    if (!http.begin(*client, req.url)) {
        res.error = "URL ungueltig";
        return res;
    }
    if (req.user.length()) http.setAuthorization(req.user.c_str(), req.pass.c_str());
    if (req.contentType) http.addHeader("Content-Type", req.contentType);
    if (req.depth) http.addHeader("Depth", req.depth);
    if (req.ifNoneMatch.length()) http.addHeader("If-None-Match", req.ifNoneMatch);
    http.addHeader("Accept-Encoding", "identity");
    const char* keys[] = {"ETag"};
    http.collectHeaders(keys, 1);

    const int code = req.body.length() || strcmp(req.method, "GET") != 0
                         ? http.sendRequest(req.method, (uint8_t*)req.body.c_str(), req.body.length())
                         : http.GET();
    res.status = code;
    if (code <= 0) {
        res.error = HTTPClient::errorToString(code);
        http.end();
        return res;
    }
    if (code >= 200 && code < 300) {
        FuncSink s(sink, ctx);
        http.writeToStream(&s);
        res.bytes = s.bytes();
    }
    if (http.hasHeader("ETag")) res.etag = http.header("ETag");
    if (code >= 400) res.error = "HTTP " + String(code);
    http.end();
    return res;
}

}  // namespace clockhttp
