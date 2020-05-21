/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/cellular.h>

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "at-common.h"
#include "debug.h"

/* Defines -------------------------------------------------------------------*/
DBG_SET_LEVEL(DBG_LEVEL_I);

#define AUTOBAUD_ATTEMPTS         5
#define WAITACK_TIMEOUT           24        // Retransmission mechanism: 1.5 + 3 + 6 + 12 = 22.5
#define SGACT_TIMEOUT             (150 + 3) // According to the AT_Command_Manual
#define TCP_CONNECT_TIMEOUT       40        // According to the AT_Command_Manual
#define PWROFF_TIMEOUT            (10 + 3)  // According to the AT_Command_Manual
#define UE866_NSOCKETS             7        // According to the AT_Command_Manual

enum socket_status {
    SOCKET_STATUS_ERROR = -1,
    SOCKET_STATUS_UNKNOWN = 0,
    SOCKET_STATUS_CONNECTED = 1,
};

enum socket_error {
    SOCKET_ERROR_DNS = 1,
    SOCKET_ERROR_TIMEOUT,
};

static const char *const ue866_urc_responses[] = {
    "SRING: ",        /* Socket data received */
    "#MONI: ",
    NULL
};

static const char *const init_strings[] = {
    "AT+CGMM",                      /* Model version. */
    "AT+CGMR",                      /* Firmware version. */
    "AT+CMEE=2",                    /* Enable extended error reporting. */
    NULL
};

struct cellular_ue866 {
    struct cellular dev;
    enum socket_status socket_status[UE866_NSOCKETS];
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
//    struct cellular_ue866 *priv = arg;

    if (at_prefix_in_table(line, ue866_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    (void) len;
    /* struct cellular_ue866 *priv = arg; */

    DBG_D("U> %s\r\n", line);

    /* int connid = -1; */
    /* if(sscanf(line, "UUSOCL: %d", &connid) == 1) { */
    /*     if(connid < UE866_NSOCKETS) { */
    /*         priv->socket_status[connid] = SOCKET_STATUS_UNKNOWN; */
    /*     } */
    /* } */
}

static const struct at_callbacks ue866_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};




static int ue866_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &ue866_callbacks, (void *) modem);

    at_set_delay(modem->at, 30);
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    /* Perform autobauding. */
    const char *response = NULL;
    for (int i=0; i<AUTOBAUD_ATTEMPTS; i++) {
        response = at_command(modem->at, "ATE0");
        if (response != NULL && *response == '\0') {
            break;
        }
    }
    if(response == NULL || *response != '\0') {
        return -2;
    }

    /* Delay 2 seconds to continue */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Initialize modem. */
    for (const char *const *command=init_strings; *command; command++) {
        response = at_command(modem->at, "%s", *command);
        if(response == NULL) {
            return -2;
        }
    }

    return 0;
}

static int ue866_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int ue866_pdp_open(struct cellular *modem, const char *apn)
{
    int active = 0;
    /* Skip the configuration if context is already open. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT#SGACT?");
    if(strlen(response)) {
        at_simple_scanf(response, "#SGACT: 1,%d", &active);
    }
    if(active) {
        return 0;
    }
    /* Skip the configuration if context is already open. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    /* Configure and open internal pdp context. */
    at_command_simple(modem->at, "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
    at_set_timeout(modem->at, SGACT_TIMEOUT);
    response = at_command(modem->at, "AT#SGACT=1,1");
    at_simple_scanf(response, "#SGACT: %*d.%*d.%*d.%d", &active);

    return 0;
}

static int ue866_pdp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_command_simple(modem->at, "AT#SAGCT=1,0");

    return 0;
}

static int ue866_shutdown(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT");

    at_set_timeout(modem->at, PWROFF_TIMEOUT);
    at_command_simple(modem->at, "AT#SHDN");

    return 0;
}

static int ue866_op_reset(struct cellular *modem)
{
    // Reboot
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_command(modem->at, "AT#ENHRST=1,0");
    vTaskDelay(pdMS_TO_TICKS(8000));
    for(int i = 0; i < 22; i++) {
        const char* resp = at_command(modem->at, "ATE0");
        if(resp && *resp == '\0') {
            break;
        }
    }
    if(at_command(modem->at, "ATE0") == NULL) {
        return -2;
    } else {
        /* Delay 2 seconds to continue */
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* Initialize modem. */
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        for (const char *const *command=init_strings; *command; command++) {
            at_command_simple(modem->at, "%s", *command);
        }
    }

    return 0;
}

static int ue866_socket_connect(struct cellular *modem, const char *host, uint16_t port)
{
    struct cellular_ue866 *priv = (struct cellular_ue866 *) modem;
    /* Open pdp context */
    if(cellular_pdp_request(modem) != 0) {
      return -1;
    }
    /* Create a tcp socket. */
    int connid = -1;
    for(int i = 1; i < UE866_NSOCKETS; i++) {
        if(SOCKET_STATUS_UNKNOWN == priv->socket_status[i]) {
            connid = i;
            break;
        }
    }
    if(connid < 0) {
        return -1;
    }

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT#SCFG=%d,1,1024,60,%d,50", connid, TCP_CONNECT_TIMEOUT * 10);
    priv->socket_status[connid] = SOCKET_STATUS_UNKNOWN;

    int retries = 3;
    do {
        at_set_timeout(modem->at, TCP_CONNECT_TIMEOUT + 3);
        const char* response = at_command(modem->at, "AT#SD=%d,0,%d,\"%s\",0,0,1", connid, port, host);
        if(!response) {
            return -2;
        } else if(!*response) {
            priv->socket_status[connid] = SOCKET_STATUS_CONNECTED;
            return connid;
        } else {
            int err = 0;
            if(!strncmp(response, "+CME ERROR: timeout in opening socket", strlen("+CME ERROR: timeout in opening socket"))) {
                err = SOCKET_ERROR_TIMEOUT;
            // } else if(!strncmp(response, "+CME ERROR: can not resolve DN", strlen("+CME ERROR: can not resolve DN"))) {
            //     err = SOCKET_ERROR_DNS;
            }
            at_set_timeout(modem->at, AT_TIMEOUT_LONG);
            at_command_simple(modem->at, "AT#SH=%d", connid);
            if(err == SOCKET_ERROR_TIMEOUT) {
                at_command_simple(modem->at, "AT+CGATT=0");
                at_command_simple(modem->at, "AT+CGATT=1");
                response = at_command(modem->at, "AT#SGACT=1,1");
                at_simple_scanf(response, "#SGACT: %*d.%*d.%*d.%d", &err);
            }
        }
    } while(--retries);

    return -1;
}

static ssize_t ue866_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    (void) flags;
    struct cellular_ue866 *priv = (struct cellular_ue866 *) modem;

    amount = amount > 1024 ? 1024 : amount;
    if(priv->socket_status[connid] != SOCKET_STATUS_CONNECTED) {
      return -1;
    }

    /* Request transmission. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_expect_dataprompt(modem->at, "> ");
    at_command_simple(modem->at, "AT#SSENDEXT=%d,%d", connid, amount);
    const char* resp = at_command_raw(modem->at, buffer, amount);
    if(resp == NULL) {
      return -2;
    } else if(!strncmp(resp, "+CME ERROR:", strlen("+CME ERROR:"))) {
      return -1;
    }

    return amount;
}

static int scanner_si(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int connid;
    if (sscanf(line, "SRING: %d", &connid) == 1) {
        return AT_RESPONSE_FINAL_OK;
    } else if (!strncmp(line, "NO CARRIER", strlen("NO CARRIER"))) {
        return AT_RESPONSE_FINAL_OK;
    }

    return AT_RESPONSE_UNKNOWN;
}

static int scanner_srecv(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int read;
    if (sscanf(line, "#SRECV: %*d,%d", &read) == 1) {
        if (read > 0) {
            return AT_RESPONSE_RAWDATA_FOLLOWS(read);
        }
    } else if (!strncmp(line, "NO CARRIER", strlen("NO CARRIER"))) {
        return AT_RESPONSE_FINAL_OK;
    }

    return AT_RESPONSE_UNKNOWN;
}

static ssize_t ue866_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    (void) flags;

    struct cellular_ue866 *priv = (struct cellular_ue866 *) modem;
    int cnt = 0;

    // TODO its dumb and exceptions should be handled in other right way
    if(priv->socket_status[connid] != SOCKET_STATUS_CONNECTED) {
        DBG_I(">>>>DISCONNECTED\r\n");
        return -1;
    }
    char tries = 4;
    while ((cnt < (int)length) && tries--) {
        int chunk = (int) length - cnt;
        /* Limit read size to avoid overflowing AT response buffer. */
        chunk = chunk > 480 ? 480 : chunk;

        /* Perform the read. */
        int read = 0;
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        at_set_command_scanner(modem->at, scanner_si);
        const char *response = at_command(modem->at, "AT#SI=%d", connid);
        if(response) {
            response = strstr(response, "#SI: ");
            if(response) {
                sscanf(response, "#SI: %*d,%*d,%*d,%d,%*d", &read);
            }
            if(read > 0) {
                at_set_command_scanner(modem->at, scanner_srecv);
                response = at_command(modem->at, "AT#SRECV=%d,%d", connid, chunk);
            } else {
                break;
            }
        }
        if (response == NULL) {
            DBG_W(">>>>NO RESPONSE\r\n");
            return -2;
        }
        /* Find the header line. */
        // TODO:
        // 1. connid is not checked
        // 2. there is possible a bug here.
        if(!*response) { // Retry
            at_set_command_scanner(modem->at, scanner_srecv);
            response = at_command(modem->at, "AT#SRECV=%d,%d", connid, chunk);
        }

        if(sscanf(response, "#SRECV: %*d,%d", &read) != 1) {
            DBG_D(">>>>BAD RESPONSE\r\n");
            return -1;
        }

        /* Bail out if we're out of data. */
        /* FIXME: We should maybe block until we receive something? */
        if (read == 0) {
            break;
        }

        /* Locate the payload. */
        const char *data = strchr(response, '\n');
        if (data++ == NULL) {
            DBG_I(">>>>NO DATA\r\n");
            return -1;
        }

        /* Copy payload to result buffer. */
        memcpy((char *)buffer + cnt, data, read);
        cnt += read;
    }

    return cnt;
}

static int ue866_socket_waitack(struct cellular *modem, int connid)
{
    struct cellular_ue866 *priv = (struct cellular_ue866 *) modem;

    if(priv->socket_status[connid] == SOCKET_STATUS_CONNECTED) {
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        for(int i = 0; i < WAITACK_TIMEOUT * 8; i++) {
            at_set_command_scanner(modem->at, scanner_si);
            const char *response = at_command(modem->at, "AT#SI=%d", connid);
            if(response && !*response) {
                vTaskDelay(pdMS_TO_TICKS(95));
                continue;
            }

            int nack = -1;
            at_simple_scanf(response, "#SI: %*d,%*d,%*d,%d", &nack);
            /* Return if all bytes were acknowledged. */
            if (nack == 0) {
                return 0;
            }
            vTaskDelay(pdMS_TO_TICKS(95));
        }
    }
    return -1;
}

static int ue866_socket_close(struct cellular *modem, int connid)
{
    struct cellular_ue866 *priv = (struct cellular_ue866 *) modem;

    if(priv->socket_status[connid] == SOCKET_STATUS_CONNECTED) {
        priv->socket_status[connid] = SOCKET_STATUS_UNKNOWN;
        at_set_timeout(modem->at, AT_TIMEOUT_LONG);
        at_command_simple(modem->at, "AT#SH=%d", connid);
    }

    return 0;
}

static const struct cellular_ops ue866_ops = {
    .reset = ue866_op_reset,
    .attach = ue866_attach,
    .detach = ue866_detach,

    .pdp_open = ue866_pdp_open,
    .pdp_close = ue866_pdp_close,
    .shutdown = ue866_shutdown,

    .imei = cellular_op_imei,
    .iccid = cellular_op_iccid,
    .imsi = cellular_op_imsi,
    .creg = cellular_op_cgreg,
    .cgatt = cellular_op_cgatt,
    .rssi = cellular_op_csq,
    .cops = cellular_op_cops,
    .test = cellular_op_test,
    .command = cellular_op_command,
    .sms = cellular_op_sms,
    .cnum = cellular_op_cnum,
    .onum = cellular_op_onum,
    .socket_connect = ue866_socket_connect,
    .socket_send = ue866_socket_send,
    .socket_recv = ue866_socket_recv,
    .socket_waitack = ue866_socket_waitack,
    .socket_close = ue866_socket_close,
};

static struct cellular_ue866 cellular;

struct cellular *cellular_alloc(void)
{
    struct cellular_ue866 *modem = &cellular;

    memset(modem, 0, sizeof(*modem));
    modem->dev.ops = &ue866_ops;

    return (struct cellular *) modem;
}

void cellular_free(struct cellular *modem)
{
}

/* vim: set ts=4 sw=4 et: */
