#ifndef UTILS_HTTP_MIME_H
#define UTILS_HTTP_MIME_H

#include <string.h>
#include <strings.h>

static inline const char *mime_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcasecmp(dot, ".js") || !strcasecmp(dot, ".mjs")) return "application/javascript; charset=utf-8";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".gif"))  return "image/gif";
    if (!strcasecmp(dot, ".webp")) return "image/webp";
    if (!strcasecmp(dot, ".ico"))  return "image/x-icon";
    if (!strcasecmp(dot, ".woff")) return "font/woff";
    if (!strcasecmp(dot, ".woff2")) return "font/woff2";
    if (!strcasecmp(dot, ".wasm")) return "application/wasm";
    if (!strcasecmp(dot, ".map"))  return "application/json";

    return "application/octet-stream";
}

#endif
