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
#define printf(...)
#include "debug.h"

/* Defines -------------------------------------------------------------------*/
DBG_SET_LEVEL(DBG_LEVEL_V);

#define AUTOBAUD_ATTEMPTS         10
#define NUMBER_SOCKETS            7
#define RESUME_TIMEOUT            60
#define SOCKET_RECV_TIMEOUT       20

enum socket_status {
    SOCKET_STATUS_ERROR = -1,
    SOCKET_STATUS_UNKNOWN = 0,
    SOCKET_STATUS_CONNECTED = 1,
};

struct socket_info {
    enum socket_status status;
};

struct modem_state {
    int power_saving : 1;
    int radio_connected: 1;
};

static const char *const sim7020_urc_responses[] = {
    /* "+NPSMR:", */
    /* "+CSCON:", */
    /* "+NSONMI:", */
    /* "+NNMI:", */
    /* "+NPING:", */
    /* "+NPINGERR:", */
    NULL,
};

struct cellular_sim7020 {
    struct cellular dev;
    struct modem_state state;
    struct socket_info sockets[NUMBER_SOCKETS];
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    if (at_prefix_in_table(line, sim7020_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    struct cellular_sim7020 *modem = (struct cellular_sim7020*) arg;
    (void) len;
    int state = 0;

    /* if(sscanf(line, "+CSCON:%*d,%d", &state) == 1) { */
    /*     modem->state.radio_connected = state; */
    /* } else if(sscanf(line, "+CSCON:%d", &state) == 1) { */
    /*     modem->state.radio_connected = state; */
    /* } else if(sscanf(line, "+NPSMR:%*d,%d", &state) == 1) { */
    /*     modem->state.power_saving = state; */
    /* } else if(sscanf(line, "+NPSMR:%d", &state) == 1) { */
    /*     modem->state.power_saving = state; */
    /* } */

    DBG_V("U> %s\r\n", line);
}

static const struct at_callbacks sim7020_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};




static int sim7020_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &sim7020_callbacks, (void *) modem);

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    /* Perform autobauding. */
    for (int i=0; i<AUTOBAUD_ATTEMPTS; i++) {
        const char *response = at_command(modem->at, "ATE0");
        if (response != NULL && *response == '\0') {
            break;
        }
    }

    /* Delay 2 seconds to continue */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Initialize modem. */
    static const char *const init_strings[] = {
        "AT+CMEE=1",
        /* "AT+CREG=2", */
        "AT+CPSMS=1,,,\"01011111\",\"00000000\"",
        NULL
    };
    for (const char *const *command=init_strings; *command; command++)
        at_command_simple(modem->at, "%s", *command);

    return 0;
}

static int sim7020_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int sim7020_pdp_open(struct cellular *modem, const char *apn)
{
    return 0;
}

static int sim7020_pdp_close(struct cellular *modem)
{
    return 0;
}

static int sim7020_shutdown(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+CFUN=0");

    return 0;
}

static int sim7020_socket_connect(struct cellular *modem, const char *host, uint16_t port)
{
    struct cellular_sim7020 *priv = (struct cellular_sim7020 *) modem;
    int connid = -1;

    if(host == NULL && port == 0) {
        return CELLULAR_NB_CONNID;
    } else {
        /* Create an udp socket. */
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        const char *response = at_command(modem->at, "AT+CSOC=1,2,1");
        at_simple_scanf(response, "+CSOC:%d", &connid);
        if(connid >= NUMBER_SOCKETS) {
            return -1;
        }
        at_command_simple(modem->at, "AT+CSOCON=%d,%d,%s", connid, port, host);
        struct socket_info *info = &priv->sockets[connid];
        info->status = SOCKET_STATUS_CONNECTED;
    }

    return connid;
}

static ssize_t sim7020_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    struct cellular_sim7020 *priv = (struct cellular_sim7020 *) modem;
    (void) flags;

    if(connid == CELLULAR_NB_CONNID) { // TODO: fixme
        amount = amount > 512 ? 512 : amount;
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        at_send(modem->at, "AT+NMGS=%d,", amount);
        at_send_hex(modem->at, buffer, amount);
        at_command_simple(modem->at, "");
        return amount;
    } else if(connid < NUMBER_SOCKETS) {
        struct socket_info *info = &priv->sockets[connid];
        if(info->status == SOCKET_STATUS_CONNECTED) {
            amount = amount > 512 ? 512 : amount;
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            at_send(modem->at, "AT+CSOSEND=%d,%d,", connid, amount * 2);
            at_send_hex(modem->at, buffer, amount);
            at_command_simple(modem->at, "");
            return amount;
        }
    }

    return 0;
}

static int scanner_nmgr(const char *line, size_t len, void *arg)
{
    (void) arg;

    if (at_prefix_in_table(line, sim7020_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    if (sscanf(line, "%d", &len) == 1) {
        if (len > 0) {
            return AT_RESPONSE_HEXDATA_FOLLOWS(len);
        }
    }

    return AT_RESPONSE_UNKNOWN;
}

static char character_handler_nmgr(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;

    if(ch == ',') {
        line[len] = '\0';
        if (sscanf(line, "%d,", &len) == 1) {
            at_set_character_handler(priv, NULL);
            ch = '\n';
        }
    }

    return ch;
}

static int scanner_csonmi(const char *line, size_t len, void *arg)
{
    (void) arg;

    if (at_prefix_in_table(line, sim7020_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    static size_t read = 0;
    if (sscanf(line, "+CSONMI: %*d,%d", &read) == 1) {
        read /= 2;
        if (read > 0) {
            return AT_RESPONSE_HEXDATA_FOLLOWS(read);
        }
    } else if(len == read) {
        return AT_RESPONSE_FINAL;
    }

    read = 0;
    return AT_RESPONSE_UNKNOWN;
}

static char character_handler_csonmi(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;

    if(ch == ',') {
        line[len] = '\0';
        if (sscanf(line, "+CSONMI: %*d,%d,", &len) == 1) {
            at_set_character_handler(priv, NULL);
            ch = '\n';
        }
    }

    return ch;
}

static ssize_t sim7020_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    struct cellular_sim7020 *priv = (struct cellular_sim7020 *) modem;
    (void) flags;

    if(connid == CELLULAR_NB_CONNID) {
        /* Perform the read. */
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        at_set_character_handler(modem->at, character_handler_nmgr);
        at_set_command_scanner(modem->at, scanner_nmgr);
        const char *response = at_command(modem->at, "AT+NMGR");
        if (response == NULL) {
            DBG_W(">>>>NO RESPONSE\r\n");
            return -2;
        }
        if (*response == '\0') {
            return 0;
        }
        /* Find the header line. */
        int read;
        if(sscanf(response, "%d,", &read) != 1) {
            DBG_I(">>>>BAD RESPONSE\r\n");
            return -1;
        }

        /* Locate the payload. */
        const char *data = strchr(response, '\n');
        if (data++ == NULL) {
            DBG_I(">>>>NO DATA\r\n");
            return -1;
        }

        /* Copy payload to result buffer. */
        memcpy((char *)buffer, data, read);

        return read;
    } else if(connid < NUMBER_SOCKETS) {
        struct socket_info *info = &priv->sockets[connid];
        if(info->status == SOCKET_STATUS_CONNECTED) {
            /* Perform the read. */
            at_set_timeout(modem->at, SOCKET_RECV_TIMEOUT);
            at_set_character_handler(modem->at, character_handler_csonmi);
            at_set_command_scanner(modem->at, scanner_csonmi);
            const char *response = at_command(modem->at, "");
            if (response == NULL) {
                DBG_W(">>>>NO RESPONSE\r\n");
                return -2;
            }
            if (*response == '\0') {
                return 0;
            }
            /* Find the header line. */
            int read;
            if(sscanf(response, "+CSONMI: %*d,%d", &read) != 1) {
                DBG_I(">>>>BAD RESPONSE\r\n");
                return -1;
            }

            /* Locate the payload. */
            const char *data = strchr(response, '\n');
            if (data++ == NULL) {
                DBG_I(">>>>NO DATA\r\n");
                return -1;
            }

            /* Copy payload to result buffer. */
            read /= 2;
            memcpy((char *)buffer, data, read); // TODO: fixme

            return read;
        }
    }

    return 0;
}

static int sim7020_socket_waitack(struct cellular *modem, int connid)
{
    return 0;
}

static int sim7020_socket_close(struct cellular *modem, int connid)
{
    struct cellular_sim7020 *priv = (struct cellular_sim7020 *) modem;

    if(connid == CELLULAR_NB_CONNID) {
        return 0;
    } else if(connid < NUMBER_SOCKETS) {
        struct socket_info *info = &priv->sockets[connid];

        if(info->status == SOCKET_STATUS_CONNECTED) {
            info->status = SOCKET_STATUS_UNKNOWN;
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            at_command_simple(modem->at, "AT+CSOCL=%d", connid);
        }
    }

    return 0;
}

static int sim7020_op_creg(struct cellular *modem)
{
    int creg;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CREG?");
    at_simple_scanf(response, "+CREG: %*d,%d", &creg);

    return creg;
}

static int sim7020_op_cops(struct cellular *modem)
{
    int ops = -1;
    int rat = -1;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+COPS?");
    if(response == NULL) {
        return -2;
    }
    int ret = sscanf(response, "+COPS: %*d,%*d,\"%d\",%d", &ops, &rat);
    if(ret == 2) {
        ops |= rat << 24;
    }

    return ops;
}

static char character_handler_nrb(char ch, char *line, size_t len, void *arg) {
    (void) arg;

    if(ch > 0x1F && ch < 0x7F) {

    } else if(ch == '\r' || ch == '\n') {

    } else {
        ch = ' ';
        line[len - 1] = ch;
    }

    return ch;
}

static int sim7020_op_reset(struct cellular *modem)
{
    struct cellular_sim7020 *priv = (struct cellular_sim7020 *) modem;

    // Cleanup
    memset(&priv->state, 0, sizeof(priv->state));
    memset(priv->sockets, 0, sizeof(priv->sockets));

    // Set CDP
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    /* at_command_simple(modem->at, "AT+CFUN=0"); */
    /* at_command_simple(modem->at, "AT+NCDP=180.101.147.115"); */

    // Reboot
    /* at_set_timeout(modem->at, AT_TIMEOUT_LONG); */
    /* at_set_character_handler(modem->at, character_handler_nrb); */
    /* if(at_command(modem->at, "AT+NRB") == NULL) { */
    /*     return -2; */
    /* } else { */
    {
        /* Delay 2 seconds to continue */
        vTaskDelay(pdMS_TO_TICKS(2000));

        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        at_command_simple(modem->at, "AT+CMEE=1");
        /* at_command_simple(modem->at, "AT+CGDCONT=1,\"IP\",\"%s\"", modem->apn); */
        at_command_simple(modem->at, "AT+CPSMS=1,,,01011111,00000000");
    }

    return 0;
}

static int sim7020_suspend(struct cellular *modem)
{
    at_suspend(modem->at);

    return 0;
}

static int sim7020_resume(struct cellular *modem)
{
    struct cellular_sim7020 *priv = (struct cellular_sim7020 *) modem;

    at_resume(modem->at);
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    at_command_simple(modem->at, "AT+CMEE=1");
    at_command_simple(modem->at, "AT+CSCON=1");
    at_command_simple(modem->at, "AT+NPSMR=1");
    at_command_simple(modem->at, "AT+CSCON?");
    at_command_simple(modem->at, "AT+NPSMR?");
    at_command_simple(modem->at, "AT+CPSMS=1,,,01011111,00000000");

    int wake_count = 0;
    const char* response = at_command(modem->at, "AT+NPING=192.168.1.1");
    if(response || *response == '\0') {
        for(int i = 0; i < RESUME_TIMEOUT; i++) {
            wake_count += !priv->state.power_saving;
            if(priv->state.radio_connected) {
                return 0;
            } else if(wake_count && priv->state.power_saving) {
                break;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    return sim7020_op_reset(modem);
}

static const struct cellular_ops sim7020_ops = {
    .reset = sim7020_op_reset,
    .attach = sim7020_attach,
    .detach = sim7020_detach,
    .suspend = sim7020_suspend,
    .resume = sim7020_resume,

    .pdp_open = sim7020_pdp_open,
    .pdp_close = sim7020_pdp_close,
    .shutdown = sim7020_shutdown,

    .imei = cellular_op_imei,
    .iccid = cellular_op_iccid,
    .imsi = cellular_op_imsi,
    .creg = sim7020_op_creg,
    .cgatt = cellular_op_cgatt,
    .rssi = cellular_op_rssi,
    .cops = sim7020_op_cops,
    .test = cellular_op_test,
    .command = cellular_op_command,
    .sms = cellular_op_sms,
    .cnum = cellular_op_cnum,
    .onum = cellular_op_onum,
    .socket_connect = sim7020_socket_connect,
    .socket_send = sim7020_socket_send,
    .socket_recv = sim7020_socket_recv,
    .socket_waitack = sim7020_socket_waitack,
    .socket_close = sim7020_socket_close,
};

static struct cellular_sim7020 cellular;

struct cellular *cellular_alloc(void)
{
    struct cellular_sim7020 *modem = &cellular;

    memset(modem, 0, sizeof(*modem));
    modem->dev.ops = &sim7020_ops;

    return (struct cellular *) modem;
}

void cellular_free(struct cellular *modem)
{
}

/* vim: set ts=4 sw=4 et: */
