/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#ifndef ATTENTIVE_AT_H
#define ATTENTIVE_AT_H

#include <attentive/parser.h>

/*
 * Publicly accessible fields. Platform-specific implementations may add private
 * fields at the end of this struct.
 */
struct at {
    struct at_parser *parser;
    const struct at_callbacks *cbs;
    void *arg;
    at_line_scanner_t command_scanner;
};

struct at_callbacks {
    at_line_scanner_t scan_line;
    at_response_handler_t handle_urc;
};

/**
 * Create an AT channel instance.
 *
 * NOTE: This is a generic interface; you should include a platform-specific
 *       <at-platform.h> file and call at_alloc_platform() variant instead.
 *
 * @returns Instance pointer on success, NULL and sets errno on failure.
 */
struct at *at_alloc(void);

/**
 * Open the AT channel.
 *
 * @param at AT channel instance.
 * @returns Zero on success, -1 and sets errno on failure.
 */
int at_open(struct at *at);

/**
 * Close the AT channel.
 *
 * @param at AT channel instance.
 * @returns Zero on success, -1 and sets errno on failure.
 */
int at_close(struct at *at);

/**
 * Suspend the AT channel.
 *
 * @param at AT channel instance.
 * @returns Zero on success, -1 and sets errno on failure.
 */
int at_suspend(struct at *at);

/**
 * Resume the AT channel.
 *
 * @param at AT channel instance.
 * @returns Zero on success, -1 and sets errno on failure.
 */
int at_resume(struct at *at);

/**
 * Close and free an AT channel instance.
 *
 * @param at AT channel instance.
 */
void at_free(struct at *at);

/**
 * Set AT channel callbacks.
 *
 * @param at AT channel instance.
 * @param cbs Set of callbacks. Not copied.
 * @param arg Private argument passed to callbacks.
 */
void at_set_callbacks(struct at *at, const struct at_callbacks *cbs, void *arg);

/**
 * Set custom per-command line scanner for the next command.
 *
 * @param at AT channel instance.
 * @param scanner Line scanner callback.
 */
void at_set_command_scanner(struct at *at, at_line_scanner_t scanner);

/**
 * Set custom per-character handler for the next command.
 *
 * @param at AT channel instance.
 * @param handler Character handler.
 */
void at_set_character_handler(struct at *at, at_character_handler_t handler);

/**
 * Expect "> " or "@" dataprompt as a response for the next command.
 *
 * @param at AT channel instance.
 */
void at_expect_dataprompt(struct at *at, const char *prompt);

/**
 * Set command timeout.
 *
 * @param at AT channel instance.
 * @param timeout Timeout in seconds (zero to disable).
 */
void at_set_timeout(struct at *at, int timeout);

/**
 * Set command delay.
 *
 * @param at AT channel instance.
 * @param delay Delay in milliseconds (zero to disable).
 */
void at_set_delay(struct at *at, int delay);

/**
 * Send an AT command and receive a response. Accepts printf-compatible
 * format and arguments.
 *
 * @param at AT channel instance.
 * @param format printf-comaptible format.
 * @returns Pointer to response (valid until next at_command) or NULL
 *          if a timeout occurs. Response is newline-delimited and does
 *          not include the final "OK".
 */
__attribute__ ((format (printf, 2, 3)))
const char *at_command(struct at *at, const char *format, ...);

/**
 * Send raw data over the AT channel.
 *
 * @param at AT channel instance.
 * @param data Raw data to send.
 * @param size Data size in bytes.
 * @returns Pointer to response (valid until next at_command) or NULL
 *          if a timeout occurs.
 */
const char *at_command_raw(struct at *at, const void *data, size_t size);

/**
 * Send an AT command. Accepts printf-compatible format and arguments.
 *
 * @param at AT channel instance.
 * @param format printf-comaptible format.
 * @returns True if success.
 */
__attribute__ ((format (printf, 2, 3)))
bool at_send(struct at *at, const char *format, ...);

/**
 * Send raw data over the AT channel.
 *
 * @param at AT channel instance.
 * @param data Raw data to send.
 * @param size Data size in bytes.
 * @returns True if success.
 */
bool at_send_raw(struct at *at, const void *data, size_t size);

/**
 * Send hex data over the AT channel.
 *
 * @param at AT channel instance.
 * @param data Hex data to send.
 * @param size Data size in bytes.
 * @returns True if success.
 */
bool at_send_hex(struct at *at, const void *data, size_t size);

/**
 * Check and config an option.
 *
 * @param at AT channel instance.
 * @param option the option to config.
 * @param value the value to set for the option.
 * @param attempts the number of attempts allowed.
 * @returns Zero on success, -1 and sets errno on failure.
 */
int at_config(struct at *at, const char *option, const char *value, int attempts);

/**
 * Send an AT command and return:
 *   -2 if it doesn't response
 *   -1 if it doesn't return OK.
 */
#define at_command_simple(at, cmd...)                                       \
    do {                                                                    \
        const char *_response = at_command(at, cmd);                        \
        if (!_response)                                                     \
            return -2;                                                      \
        if (strcmp(_response, "")) {                                        \
            return -1;                                                      \
        }                                                                   \
    } while (0)

/**
 * Send raw data and return:
 *   -2 if it doesn't response
 *   -1 if it doesn't return OK.
 */
#define at_command_raw_simple(at, cmd...)                                   \
    do {                                                                    \
        const char *_response = at_command_raw(at, cmd);                    \
        if (!_response)                                                     \
            return -2;                                                      \
        if (strcmp(_response, "")) {                                        \
            return -1;                                                      \
        }                                                                   \
    } while (0)

/**
 * Count macro arguments. Source:
 * http://stackoverflow.com/questions/2124339/c-preprocessor-va-args-number-of-arguments
 */
#define _NUMARGS(...) (sizeof((void *[]){0, ##__VA_ARGS__})/sizeof(void *)-1)

/**
 * Scanf a response and return:
 *   -2 if the response is null
 *   -1 if scanf fails.
 */
#define at_simple_scanf(_response, format, ...)                             \
    do {                                                                    \
        if (!_response)                                                     \
            return -2;                                                      \
        if (sscanf(_response, format, __VA_ARGS__) != _NUMARGS(__VA_ARGS__)) { \
            return -1;                                                      \
        }                                                                   \
    } while (0)

#endif

/* vim: set ts=4 sw=4 et: */
