/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/at.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "FreeRTOS_IO.h"
#include "task.h"
#include "semphr.h"
#define printf(...)

// Remove once you refactor this out.
#define AT_COMMAND_LENGTH 80

struct at_freertos {
    struct at at;
    int timeout;            /**< Command timeout in seconds. */
    const char *response;

    TaskHandle_t xTask;
    /*SemaphoreHandle_t xMutex;*/
    SemaphoreHandle_t xSem;
    Peripheral_Descriptor_t xUART;

    bool running : 1;       /**< Reader thread should be running. */
    bool open : 1;          /**< FD is valid. Set/cleared by open()/close(). */
    bool busy : 1;          /**< FD is in use. Set/cleared by reader thread. */
    bool waiting : 1;       /**< Waiting for response callback to arrive. */
};

void at_reader_thread(void *arg);

static void handle_response(const char *buf, size_t len, void *arg)
{
    struct at_freertos *priv = (struct at_freertos *) arg;

    /* The mutex is held by the reader thread; don't reacquire. */
    priv->response = buf;
    (void) len;
    priv->waiting = false;
    xSemaphoreGive(priv->xSem);
}

static void handle_urc(const char *buf, size_t len, void *arg)
{
    struct at *at = (struct at *) arg;

    /* Forward to caller's URC callback, if any. */
    if (at->cbs->handle_urc)
        at->cbs->handle_urc(buf, len, at->arg);
}

enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    struct at *at = (struct at *) arg;

    enum at_response_type type = AT_RESPONSE_UNKNOWN;
    if (at->command_scanner)
        type = at->command_scanner(line, len, at->arg);
    if (!type && at->cbs && at->cbs->scan_line)
        type = at->cbs->scan_line(line, len, at->arg);
    return type;
}

static const struct at_parser_callbacks parser_callbacks = {
    .handle_response = handle_response,
    .handle_urc = handle_urc,
    .scan_line = scan_line,
};

struct at *at_alloc_freertos(void)
{
    /* allocate instance */
    struct at_freertos *priv = malloc(sizeof(struct at_freertos));
    if (!priv) {
        return NULL;
    }
    memset(priv, 0, sizeof(struct at_freertos));

    /* allocate underlying parser */
    priv->at.parser = at_parser_alloc(&parser_callbacks, 512, (void *) priv);
    if (!priv->at.parser) {
        free(priv);
        return NULL;
    }

    /* initialize and start reader thread */
    priv->running = true;
    /*priv->xMutex = xSemaphoreCreateBinary();*/
    priv->xSem = xSemaphoreCreateBinary();
    xTaskCreate(at_reader_thread, "ATReadTask", configMINIMAL_STACK_SIZE * 2, priv, 4, &priv->xTask);

    return (struct at *) priv;
}

int at_open(struct at *at)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    priv->xUART = FreeRTOS_open(boardGSM_SIM800_UART, 0);
    if(priv->xUART == NULL) {
        return -1;
    } else {
        FreeRTOS_ioctl(priv->xUART, ioctlUSE_DMA_TX, (void*)0);
        FreeRTOS_ioctl(priv->xUART, ioctlUSE_CIRCULAR_BUFFER_RX, (void*)512);
        FreeRTOS_ioctl(priv->xUART, ioctlSET_TX_TIMEOUT, (void*)pdMS_TO_TICKS(200));
        FreeRTOS_ioctl(priv->xUART, ioctlSET_RX_TIMEOUT, (void*)pdMS_TO_TICKS(50));
    }

    priv->open = true;
    /*xSemaphoreGive(priv->xMutex);*/
    xSemaphoreTake(priv->xSem, 0);
    return 0;
}

int at_close(struct at *at)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    /* Mark the port descriptor as invalid. */
    priv->open = false;

    FreeRTOS_close(priv->xUART);
    priv->xUART = NULL;

    return 0;
}

void at_free(struct at *at)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    /* make sure the channel is closed */
    at_close(at);

    /* ask the reader thread to terminate */
    priv->running = false;
    if(priv->xTask != NULL) {
        vTaskDelete(priv->xTask);
    }

    /* free up resources */
    free(priv->at.parser);
    free(priv);
}

void at_set_callbacks(struct at *at, const struct at_callbacks *cbs, void *arg)
{
    at->cbs = cbs;
    at->arg = arg;
}

void at_set_command_scanner(struct at *at, at_line_scanner_t scanner)
{
    at->command_scanner = scanner;
}

void at_set_timeout(struct at *at, int timeout)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    priv->timeout = timeout;
}

void at_set_character_handler(struct at *at, at_character_handler_t handler)
{
    at_parser_set_character_handler(at->parser, handler);
}

void at_expect_dataprompt(struct at *at)
{
    at_parser_expect_dataprompt(at->parser);
}

static const char *_at_command(struct at_freertos *priv, const void *data, size_t size)
{
    /*if(!xSemaphoreTake(priv->xMutex, pdMS_TO_TICKS(1000))) {*/
        /*return NULL;*/
    /*}*/

    /* Bail out if the channel is closing or closed. */
    if (!priv->open) {
        /*xSemaphoreGive(priv->xMutex);*/
        return NULL;
    }

    /* Prepare parser. */
    at_parser_await_response(priv->at.parser);

    /* Send the command. */
    // FIXME: handle interrupts, short writes, errors, etc.
    FreeRTOS_write(priv->xUART, data, size);

    /* Wait for the parser thread to collect a response. */
    priv->waiting = true;
    /*xSemaphoreGive(priv->xMutex);*/
    xSemaphoreTake(priv->xSem, 0);
    int timeout = priv->timeout;
    while (timeout-- && priv->open && priv->waiting) {
        if (xSemaphoreTake(priv->xSem, pdMS_TO_TICKS(1000))) {
            break;
        }
    }

    /*xSemaphoreTake(priv->xMutex, pdMS_TO_TICKS(1000));*/
    const char *result;
    if (!priv->open) {
        /* The serial port was closed behind our back. */
        result = NULL;
    } else if (priv->waiting) {
        /* Timed out waiting for a response. */
        at_parser_reset(priv->at.parser);
        result = NULL;
    } else {
        /* Response arrived. */
        result = priv->response;
    }

    /* Reset per-command settings. */
    priv->at.command_scanner = NULL;
    /*xSemaphoreGive(priv->xMutex);*/

    return result;
}

const char *at_command(struct at *at, const char *format, ...)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    /* Build command string. */
    va_list ap;
    va_start(ap, format);
    char line[AT_COMMAND_LENGTH];
    int len = vsnprintf(line, sizeof(line)-1, format, ap);
    va_end(ap);

    /* Bail out if we run out of space. */
    if (len >= (int)(sizeof(line)-1)) {
        return NULL;
    }

    printf("> %s\n", line);

    /* Append modem-style newline. */
    line[len++] = '\r';

    /* Send the command. */
    return _at_command(priv, line, len);
}

const char *at_command_raw(struct at *at, const void *data, size_t size)
{
    struct at_freertos *priv = (struct at_freertos *) at;

    printf("> [%zu bytes]\n", size);

    return _at_command(priv, data, size);
}

void at_reader_thread(void *arg)
{
    struct at_freertos *priv = (struct at_freertos *)arg;

//    printf("at_reader_thread: starting\n");

    while (true) {
        if (!priv->running) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        /* Wait for the port descriptor to be valid. */
        else if(!priv->open) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* Lock access to the port descriptor. */
        /*xSemaphoreTake(priv->xMutex, pdMS_TO_TICKS(1000));*/
        priv->busy = true;

        /* Attempt to read some data. */
        char ch;
        int result = FreeRTOS_read(priv->xUART, &ch, 1);

        /* Unlock access to the port descriptor. */
        priv->busy = false;
        /* Notify at_close() that the port is now free. */

        if (result == 1) {
            /* Data received, feed the parser. */
            at_parser_feed(priv->at.parser, &ch, 1);
        }
        /*xSemaphoreGive(priv->xMutex);*/
    }

//    printf("at_reader_thread: finished\n");

}

/* vim: set ts=4 sw=4 et: */
