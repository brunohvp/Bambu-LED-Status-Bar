#include "Log.h"
#include <cstdarg>

static const int LOG_LINES = 30;
static const int LINE_LEN = 96;
static char g_lines[LOG_LINES][LINE_LEN];
static int g_head = 0; // next slot to write
static int g_count = 0;

void Log::add(const char *fmt, ...) {
    char buf[LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.println(buf);

    strncpy(g_lines[g_head], buf, LINE_LEN - 1);
    g_lines[g_head][LINE_LEN - 1] = '\0';
    g_head = (g_head + 1) % LOG_LINES;
    if (g_count < LOG_LINES) g_count++;
}

String Log::recent() {
    String out;
    int start = (g_head - g_count + LOG_LINES) % LOG_LINES;
    for (int i = 0; i < g_count; i++) {
        out += g_lines[(start + i) % LOG_LINES];
        out += '\n';
    }
    return out;
}
