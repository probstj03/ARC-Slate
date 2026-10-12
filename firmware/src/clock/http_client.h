#pragma once
// Small HTTPS/HTTP client for Nextcloud (CalDAV/WebDAV) and Open-Meteo.
// The response body is streamed into a sink (no big buffers); chunked encoding is handled.
//
// Certificates: the curated root list in ca_roots.h, or /ca.pem on LittleFS if present
// (own CA), or none at all when the config says tlsInsecure.
#include <Arduino.h>

namespace clockhttp {

struct Request {
    const char* method = "GET";
    String url;
    String user, pass;                   // Basic authentication (empty = none)
    String body;
    const char* contentType = nullptr;
    const char* depth = nullptr;         // WebDAV "Depth" header
    String ifNoneMatch;                  // ETag of the copy we already have
    bool insecure = false;
    uint32_t timeoutMs = 15000;
};

struct Result {
    int status = 0;                      // HTTP status, <= 0: transport error
    String etag;
    String error;                        // short text for the log / status page
    size_t bytes = 0;
};

using SinkFn = void (*)(const uint8_t* data, size_t len, void* ctx);

// Runs one request. `sink` receives the body of 2xx responses (nullptr = discard).
Result perform(const Request& req, SinkFn sink, void* ctx);

// Percent-encodes a path (keeps '/'): "Notizen/Aufgaben Liste.md" -> "Notizen/Aufgaben%20Liste.md"
String encodePath(const String& path);

}  // namespace clockhttp
