/* SSE (text/event-stream) extraction — pure C, host-testable.
 *
 * MCP Streamable HTTP servers answer POSTs with either plain JSON or an SSE
 * stream of `event: message` / `data: {json-rpc}` frames. This extracts each
 * event's concatenated data payload.
 */
#include "mcp_common.h"
#include <stdlib.h>
#include <string.h>

void mcp_sse_extract(const char *body, size_t len, mcp_sse_data_cb cb, void *user)
{
    size_t i = 0;
    char *data = NULL;
    size_t dlen = 0, dcap = 0;

    while (i < len) {
        size_t eol = i;
        while (eol < len && body[eol] != '\n') {
            eol++;
        }
        size_t linelen = eol - i;
        if (linelen && body[i + linelen - 1] == '\r') {
            linelen--;
        }
        const char *line = body + i;

        if (linelen == 0) {
            /* blank line: end of event */
            if (dlen > 0 && cb) {
                cb(data, dlen, user);
            }
            free(data);
            data = NULL;
            dlen = dcap = 0;
        } else if (linelen >= 5 && memcmp(line, "data:", 5) == 0) {
            const char *d = line + 5;
            size_t dl = linelen - 5;
            if (dl && *d == ' ') {
                d++;
                dl--;
            }
            if (dlen + dl + 1 > dcap) {
                dcap = (dlen + dl + 1) * 2;
                data = realloc(data, dcap);
            }
            memcpy(data + dlen, d, dl);
            dlen += dl;
            data[dlen] = '\0';
        }
        /* "event:", "id:" and comment lines are ignored */

        i = (eol < len) ? eol + 1 : len;
    }
    /* final event without a trailing blank line */
    if (dlen > 0 && cb) {
        cb(data, dlen, user);
    }
    free(data);
}
