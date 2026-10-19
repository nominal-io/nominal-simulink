/*
 * nm_profile.c — read a connection profile from Nominal's config file.
 *
 * Ported from nominal-matlab's readProfile.m, and like it, not a YAML parser: it reads
 * the shape the nom CLI writes and nothing else.
 *
 *     version: 2
 *     profiles:
 *       default:
 *         base_url: https://api.gov.nominal.io/api
 *         token: nominal_api_key_...
 *         workspace_rid: null
 *
 * A key at column zero is top level. Under `profiles:`, an indented key with no value
 * opens a profile and the keys after it belong to that profile. A key this file doesn't
 * know is skipped, since the CLI may write more than the block reads.
 *
 * In C rather than MATLAB so the same code can run where there is no MATLAB, such as a
 * real-time target with a profile file deployed beside the application.
 */

#include "nominal_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Long enough for a line holding the longest token nm_profile keeps. */
#define LINE_BYTES 8192

static void default_path(char *buf, size_t len)
{
#if defined(_WIN32)
    const char *home = getenv("USERPROFILE");
    const char *sep  = "\\";
#else
    const char *home = getenv("HOME");
    const char *sep  = "/";
#endif
    if (home == NULL) {
        home = "";
    }
    snprintf(buf, len, "%s%s.config%snominal%sconfig.yml", home, sep, sep, sep);
}

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                       end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}

/* A hand-edited file may quote a value. The CLI doesn't. */
static char *unquote(char *s)
{
    size_t n = strlen(s);

    if (n >= 2 && s[0] == s[n - 1] && (s[0] == '"' || s[0] == '\'')) {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

/* PyYAML writes an unset optional field as `null`, so a profile created without a
 * workspace really does say "workspace_rid: null". */
static int is_null(const char *s)
{
    return strcmp(s, "null") == 0 || strcmp(s, "Null") == 0 || strcmp(s, "NULL") == 0 ||
           strcmp(s, "~") == 0;
}

/* Copy a value into a field, or return -1 if it doesn't fit. */
static int keep(char *field, size_t len, const char *value)
{
    size_t n = strlen(value);

    if (n >= len) {
        return -1;
    }
    memcpy(field, value, n + 1);
    return 0;
}

nm_profile_status nm_profile_read(const char *path, const char *name, nm_profile *out,
                                  char *path_used, size_t path_used_len)
{
    char              resolved[1024];
    char             *line;
    FILE             *f;
    int               in_profiles = 0, in_ours = 0, found = 0;
    nm_profile_status st = NM_PROFILE_OK;

    if (path == NULL || path[0] == '\0') {
        default_path(resolved, sizeof resolved);
        path = resolved;
    }
    if (path_used != NULL && path_used_len > 0) {
        snprintf(path_used, path_used_len, "%s", path);
    }
    if (out == NULL || name == NULL) {
        return NM_PROFILE_NOT_FOUND;
    }
    memset(out, 0, sizeof *out);

    f = fopen(path, "r");
    if (f == NULL) {
        return NM_PROFILE_NO_FILE;
    }
    line = malloc(LINE_BYTES);
    if (line == NULL) {
        fclose(f);
        return NM_PROFILE_NO_FILE;
    }

    while (fgets(line, LINE_BYTES, f) != NULL) {
        char *text = trim(line);
        char *colon, *key, *value;

        if (text[0] == '\0' || text[0] == '#') {
            continue;
        }

        /* Column zero is top level: `version:` or `profiles:`. */
        if (line[0] != ' ' && line[0] != '\t') {
            in_profiles = strncmp(text, "profiles:", 9) == 0;
            in_ours     = 0;
            continue;
        }
        if (!in_profiles) {
            continue;
        }

        /* Split on the first colon only: URLs and tokens have colons of their own. */
        colon = strchr(text, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        key   = trim(text);
        value = unquote(trim(colon + 1));

        if (value[0] == '\0') {
            /* A key with no value opens a profile. */
            in_ours = strcmp(unquote(key), name) == 0;
            found   = found || in_ours;
            continue;
        }
        if (!in_ours) {
            continue;
        }
        if (is_null(value)) {
            value = "";
        }

        if (strcmp(key, "base_url") == 0) {
            if (keep(out->base_url, sizeof out->base_url, value) != 0) {
                st = NM_PROFILE_TOO_LONG;
            }
        } else if (strcmp(key, "token") == 0) {
            if (keep(out->token, sizeof out->token, value) != 0) {
                st = NM_PROFILE_TOO_LONG;
            }
        } else if (strcmp(key, "workspace_rid") == 0) {
            if (keep(out->workspace_rid, sizeof out->workspace_rid, value) != 0) {
                st = NM_PROFILE_TOO_LONG;
            }
        }
    }

    free(line);
    fclose(f);

    if (!found) {
        return NM_PROFILE_NOT_FOUND;
    }
    if (st != NM_PROFILE_OK) {
        return st;
    }
    if (out->token[0] == '\0') {
        return NM_PROFILE_NO_TOKEN;
    }
    if (out->base_url[0] == '\0') {
        return NM_PROFILE_NO_BASE_URL;
    }
    return NM_PROFILE_OK;
}
