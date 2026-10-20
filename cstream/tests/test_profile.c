/*
 * Profile reader tests, against config files written here in the shapes the nom CLI and
 * a hand edit produce. No network.
 */

#include "nominal_stream.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("ok:   %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static const char *const PATH = "test_profile_config.yml";

static void write_config(const char *text)
{
    FILE *f = fopen(PATH, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

/* What `nom config profile add` writes, two profiles, one without a workspace. */
static const char *const CLI_CONFIG =
    "version: 2\n"
    "profiles:\n"
    "  default:\n"
    "    base_url: https://api.gov.nominal.io/api\n"
    "    token: nominal_api_key_abc\n"
    "    workspace_rid: null\n"
    "  staging:\n"
    "    base_url: https://api-staging.gov.nominal.io/api\n"
    "    token: eyJ.with:colons\n"
    "    workspace_rid: ri.security.gov-staging.workspace.1\n";

static void test_reads_each_profile(void)
{
    nm_profile p;

    write_config(CLI_CONFIG);
    check(nm_profile_read(PATH, "default", &p, NULL, 0) == NM_PROFILE_OK &&
          strcmp(p.base_url, "https://api.gov.nominal.io/api") == 0 &&
          strcmp(p.token, "nominal_api_key_abc") == 0,
          "reads the default profile");
    check(p.workspace_rid[0] == '\0', "workspace_rid: null reads as unset");

    check(nm_profile_read(PATH, "staging", &p, NULL, 0) == NM_PROFILE_OK &&
          strcmp(p.base_url, "https://api-staging.gov.nominal.io/api") == 0 &&
          strcmp(p.token, "eyJ.with:colons") == 0 &&
          strcmp(p.workspace_rid, "ri.security.gov-staging.workspace.1") == 0,
          "reads a second profile, keeping colons inside a value");
}

static void test_reads_a_hand_edited_file(void)
{
    nm_profile p;

    write_config("# my settings\r\n"
                 "version: 2\r\n"
                 "profiles:\r\n"
                 "\tdefault:\r\n"
                 "\t\t# the cloud instance\r\n"
                 "\t\tbase_url: \"https://api.gov.nominal.io/api\"\r\n"
                 "\t\ttoken: 'quoted'\r\n"
                 "\t\tcolor: blue\r\n");
    check(nm_profile_read(PATH, "default", &p, NULL, 0) == NM_PROFILE_OK &&
          strcmp(p.base_url, "https://api.gov.nominal.io/api") == 0 &&
          strcmp(p.token, "quoted") == 0,
          "reads tabs, CRLF, comments, quotes and an unknown key");
}

/* YAML a hand edit can add that the CLI never writes: a comment after a value, a # inside
 * one, and a map nested under a profile. */
static void test_reads_comments_and_nested_blocks(void)
{
    nm_profile p;

    write_config("profiles:\n"
                 "  default:\n"
                 "    base_url: \"https://api.gov.nominal.io/api\" # quoted, then a comment\n"
                 "    token: nominal_api_key_abc#def # a comment, but not the first #\n"
                 "    extras:\n"
                 "      token: wrong\n"
                 "      nested:\n"
                 "        token: also wrong\n"
                 "    workspace_rid: ri.security.gov-staging.workspace.1\n"
                 "  other:\n"
                 "    base_url: https://other/api\n"
                 "    token: other\n");
    check(nm_profile_read(PATH, "default", &p, NULL, 0) == NM_PROFILE_OK &&
          strcmp(p.base_url, "https://api.gov.nominal.io/api") == 0 &&
          strcmp(p.token, "nominal_api_key_abc#def") == 0,
          "a comment after a value is left out, and a # inside a value is kept");
    check(strcmp(p.workspace_rid, "ri.security.gov-staging.workspace.1") == 0,
          "a key after a nested block still belongs to the profile");
    check(nm_profile_read(PATH, "extras", &p, NULL, 0) == NM_PROFILE_NOT_FOUND,
          "a map nested under a profile is not a profile");
    check(nm_profile_read(PATH, "other", &p, NULL, 0) == NM_PROFILE_OK &&
          strcmp(p.token, "other") == 0,
          "the profile after the nested block is read");
}

static void test_reports_each_problem(void)
{
    nm_profile p;
    char       used[256];

    check(nm_profile_read("no_such_dir/config.yml", "default", &p, used, sizeof used)
              == NM_PROFILE_NO_FILE &&
          strcmp(used, "no_such_dir/config.yml") == 0,
          "a missing file is reported, with the path it tried");

    write_config(CLI_CONFIG);
    check(nm_profile_read(PATH, "prod", &p, NULL, 0) == NM_PROFILE_NOT_FOUND,
          "a profile that isn't there is reported");

    write_config("profiles:\n  default:\n    base_url: https://x/api\n    token: null\n");
    check(nm_profile_read(PATH, "default", &p, NULL, 0) == NM_PROFILE_NO_TOKEN,
          "a profile without a token is reported");

    write_config("profiles:\n  default:\n    token: abc\n");
    check(nm_profile_read(PATH, "default", &p, NULL, 0) == NM_PROFILE_NO_BASE_URL,
          "a profile without a base_url is reported");

    /* A key named like a profile, outside profiles:, isn't one. */
    write_config("default:\n  token: abc\n  base_url: https://x/api\n");
    check(nm_profile_read(PATH, "default", &p, NULL, 0) == NM_PROFILE_NOT_FOUND,
          "only names under profiles: are profiles");
}

static void test_default_path(void)
{
    nm_profile p;
    char       used[1024];

    nm_profile_read(NULL, "default", &p, used, sizeof used);
    check(strstr(used, ".config") != NULL && strstr(used, "config.yml") != NULL,
          "with no path, it reads .config/nominal/config.yml in the home folder");
}

int main(void)
{
    test_reads_each_profile();
    test_reads_a_hand_edited_file();
    test_reads_comments_and_nested_blocks();
    test_reports_each_problem();
    test_default_path();
    remove(PATH);

    printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
