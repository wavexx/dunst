/* copyright 2013 Sascha Kruse and contributors (see LICENSE for licensing information) */

#define _GNU_SOURCE
#include <stdbool.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <glib.h>

#include "dunst.h"
#include "utils.h"
#include "settings.h"
#include "dbus.h"

        /*
         * Exctract all urls from a given string.
         *
         * Return: a string of urls separated by \n
         *
         */
char *extract_urls(const char *to_match)
{
        static bool is_initialized = false;
        static regex_t cregex;

        if (!is_initialized) {
                char *regex =
                    "\\b(https?://|ftps?://|news://|mailto:|file://|www\\.)"
                    "[-[:alnum:]_\\@;/?:&=%$.+!*\x27,~#]*"
                    "(\\([-[:alnum:]_\\@;/?:&=%$.+!*\x27,~#]*\\)|[-[:alnum:]_\\@;/?:&=%$+*~])+";
                int ret = regcomp(&cregex, regex, REG_EXTENDED | REG_ICASE);
                if (ret != 0) {
                        printf("failed to compile regex\n");
                        return NULL;
                } else {
                        is_initialized = true;
                }
        }

        char *urls = NULL;

        const char *p = to_match;
        regmatch_t m;

        while (1) {
                int nomatch = regexec(&cregex, p, 1, &m, 0);
                if (nomatch) {
                        return urls;
                }
                int start;
                int finish;
                if (m.rm_so == -1) {
                        break;
                }
                start = m.rm_so + (p - to_match);
                finish = m.rm_eo + (p - to_match);

                char *match = strndup(to_match + start, finish - start);

                urls = string_append(urls, match, "\n");

                free(match);

                p += m.rm_eo;
        }
        return urls;
}

        /*
         * Open url in browser.
         *
         */
void open_browser(const char *url)
{
        int browser_pid1 = fork();

        if (browser_pid1) {
                int status;
                waitpid(browser_pid1, &status, 0);
        } else {
                int browser_pid2 = fork();
                if (browser_pid2) {
                        exit(0);
                } else {
                        char *browser_cmd =
                            string_append(settings.browser, url, " ");
                        char **cmd = g_strsplit(browser_cmd, " ", 0);
                        execvp(cmd[0], cmd);
                }
        }
}

    /*
     * Notify the corresponding client
     * that an action has been invoked
     */
void invoke_action(const char *action)
{
        notification *invoked = NULL;
        char *action_identifier = NULL;

        char *appname_begin = strchr(action, '[');
        if (!appname_begin) {
                printf("invalid action: %s\n", action);
                return;
        }
        appname_begin++;
        int appname_len = strlen(appname_begin) - 1; // remove ]
        int action_len = strlen(action) - appname_len - 3; // remove space, [, ]

        for (GList * iter = g_queue_peek_head_link(displayed); iter;
             iter = iter->next) {
                notification *n = iter->data;
                if (g_str_has_prefix(appname_begin, n->appname) && strlen(n->appname) == appname_len) {
                        if (!n->actions)
                                continue;

                        for (int i = 0; i < n->actions->count; i += 2) {
                                char *a_identifier = n->actions->actions[i];
                                char *name = n->actions->actions[i + 1];
                                if (g_str_has_prefix(action, name) && strlen(name) == action_len) {
                                        invoked = n;
                                        action_identifier = a_identifier;
                                        break;
                                }
                        }
                }
        }

        if (invoked && action_identifier) {
                actionInvoked(invoked, action_identifier);
        }
}

    /*
     * Dispatch whatever has been returned
     * by the menu.
     */
void dispatch_menu_result(const char *input)
{
        char *in = strdup(input);
        g_strstrip(in);
        switch (in[0]) {
            case '#':
                invoke_action(in + 1);
                break;
            case '[': // named url. skip name and continue
                in = strchr(in, ']');
                if (in == NULL)
                    break;
            default:
                {   // test and open url
                    char *maybe_url = extract_urls(in);
                    if (maybe_url) {
                            open_browser(maybe_url);
                            free(maybe_url);
                            break;
                    }
                }
        }
        free(in);
}

        /*
         * Open the context menu that let's the user
         * select urls/actions/etc
         */
void context_menu(void)
{
        char *dmenu_input = NULL;

        for (GList * iter = g_queue_peek_head_link(displayed); iter;
             iter = iter->next) {
                notification *n = iter->data;
                dmenu_input = string_append(dmenu_input, n->urls, "\n");
                if (n->actions)
                        dmenu_input =
                            string_append(dmenu_input, n->actions->dmenu_str,
                                          "\n");
        }

        if (!dmenu_input)
                return;

        char buf[1024] = {0};
        int child_io[2];
        int parent_io[2];
        if (pipe(child_io) != 0) {
                PERR("pipe()", errno);
                return;
        }
        if (pipe(parent_io) != 0) {
                PERR("pipe()", errno);
                return;
        }
        int pid = fork();

        if (pid == 0) {
                close(child_io[1]);
                close(parent_io[0]);
                close(0);
                if (dup(child_io[0]) == -1) {
                        PERR("dup()", errno);
                        exit(EXIT_FAILURE);
                }
                close(1);
                if (dup(parent_io[1]) == -1) {
                        PERR("dup()", errno);
                        exit(EXIT_FAILURE);
                }
                execvp(settings.dmenu_cmd[0], settings.dmenu_cmd);
        } else {
                close(child_io[0]);
                close(parent_io[1]);
                size_t wlen = strlen(dmenu_input);
                if (write(child_io[1], dmenu_input, wlen) != wlen) {
                        PERR("write()", errno);
                }
                close(child_io[1]);

                size_t len = read(parent_io[0], buf, 1023);
                if (len == 0)
                        return;

                int status;
                waitpid(pid, &status, 0);
        }

        close(parent_io[0]);

        dispatch_menu_result(buf);
}
/* vim: set ts=8 sw=8 tw=0: */
