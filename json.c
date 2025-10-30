#define JSMN_IMPLEMENTATION
#include "json.h"
#include "string.h"

// Helper to copy token string into a buffer
void json_token_to_str(jsmn_parser *parser, int token_idx, char *buf, int buf_len) {
    jsmntok_t *t = &parser->tokens[token_idx];
    int len = t->end - t->start;
    if (len >= buf_len) {
        len = buf_len - 1;
    }
    memcpy(buf, parser->js + t->start, len);
    buf[len] = 0;
}
