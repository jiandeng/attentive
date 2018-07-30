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

#define AUTOBAUD_ATTEMPTS         10
#define NUMBER_SOCKETS            7
#define RESUME_TIMEOUT            60
#define TUP_REGISTER_TIMEOUT      20

enum socket_status {
    SOCKET_STATUS_ERROR = -1,
    SOCKET_STATUS_UNKNOWN = 0,
    SOCKET_STATUS_CONNECTED = 1,
};

struct socket_info {
    enum socket_status status;
    const char* host;
    int port;
    int local_port;
};

struct modem_state {
    int power_saving : 1;
    int radio_connected: 1;
};

static const char *const nb501_urc_responses[] = {
    "+NPSMR:",
    "+CSCON:",
    "+NSONMI:",
    "+NNMI:",
    "+NPING:",
    "+NPINGERR:",
    NULL,
};

static const char *const nb501_init_commands[] = {
    "AT+CMEE=1",
    "AT+CSCON=1",
    "AT+NPSMR=1",
    NULL,
};

struct cellular_nb501 {
    struct cellular dev;
    struct modem_state state;
    struct socket_info sockets[NUMBER_SOCKETS];
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    if (at_prefix_in_table(line, nb501_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    struct cellular_nb501 *modem = (struct cellular_nb501*) arg;
    (void) len;
    int state = 0;

    if(sscanf(line, "+CSCON:%*d,%d", &state) == 1) {
        modem->state.radio_connected = state;
    } else if(sscanf(line, "+CSCON:%d", &state) == 1) {
        modem->state.radio_connected = state;
    } else if(sscanf(line, "+NPSMR:%*d,%d", &state) == 1) {
        modem->state.power_saving = state;
    } else if(sscanf(line, "+NPSMR:%d", &state) == 1) {
        modem->state.power_saving = state;
    }

    DBG_D("U> %s\r\n", line);
}

static const struct at_callbacks nb501_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};




static int nb501_attach(struct cellular *modem)
{
    const char* response = NULL;

    at_set_callbacks(modem->at, &nb501_callbacks, (void *) modem);

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    /* Perform autobauding. */
    for (int i=0; i<AUTOBAUD_ATTEMPTS; i++) {
        response = at_command(modem->at, "AT");
        if (response && *response) {
            break;
        }
    }
    if(!response) {
        return -2;
    }

    /* Delay 2 seconds to continue */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Initialize modem. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    response = at_command(modem->at, "AT+NCDP?");
    if(!response) {
        return -2;
    } else if(strncmp(response, "+NCDP:180.101.147.115", strlen("+NCDP:180.101.147.115"))) {
        int nb501_op_reset(struct cellular *modem);
        return nb501_op_reset(modem);
    }

    for (const char *const *command=nb501_init_commands; *command; command++) {
        at_command_simple(modem->at, "%s", *command);
    }
    at_command_simple(modem->at, "AT+CGDCONT=1,\"IP\",\"%s\"", modem->apn);
    at_command_simple(modem->at, "AT+CPSMS=1,,,01011111,00000000");

    return 0;
}

static int nb501_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int nb501_pdp_open(struct cellular *modem, const char *apn)
{
    return 0;
}

static int nb501_pdp_close(struct cellular *modem)
{
    return 0;
}

static int nb501_shutdown(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+CFUN=0");

    return 0;
}

static int nb501_get_free_port(struct cellular *modem) {
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;

    for(int port = 444; port < 444 + NUMBER_SOCKETS; port++) {
        bool available = true;

        for(int i = 0; i < NUMBER_SOCKETS; i++) {
            struct socket_info *info = &priv->sockets[i];
            if(info->status == SOCKET_STATUS_CONNECTED && info->local_port == port) {
                available = false;
                break;
            }
        }

        if(available) {
            return port;
        }
    }

    return -1;
}

static int nb501_socket_connect(struct cellular *modem, const char *host, uint16_t port)
{
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;
    int connid = -1;

    if(host == NULL || *host == 0 || port == 0) {
        return CELLULAR_NB_CONNID;
    } else {
        /* Create an udp socket. */
        int local_port = nb501_get_free_port(modem);
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        const char *response = at_command(modem->at, "AT+NSOCR=DGRAM,17,%d", local_port);
        at_simple_scanf(response, "%d", &connid);
        if(connid >= NUMBER_SOCKETS) {
            return -1;
        } else {
            struct socket_info *info = &priv->sockets[connid];
            info->host = host;
            info->port = port;
            info->local_port = local_port;
            info->status = SOCKET_STATUS_CONNECTED;
        }
    }

    return connid;
}

static ssize_t nb501_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;
    (void) flags;

    if(connid == CELLULAR_NB_CONNID) {
        amount = amount > 512 ? 512 : amount;
        at_set_timeout(modem->at, AT_TIMEOUT_LONG);
        at_send(modem->at, "AT+NMGS=%d,", amount);
        at_send_hex(modem->at, buffer, amount);
        const char* response = at_command(modem->at, "");
        if(!response) {
            return -2;
        } else if(!*response) {
            return amount;
        } else if(!strncmp(response, "+CME ERROR: 513", strlen("+CME ERROR: 513"))) {
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            for(int i = 0; i < TUP_REGISTER_TIMEOUT; i++) {
                const char* response = at_command(modem->at, "AT+NMSTATUS?");
                if(response == NULL) {
                    return -2;
                } else if(!strncmp(response, "+NMSTATUS:MO_DATA_ENABLED", strlen("+NMSTATUS:MO_DATA_ENABLED"))) {
                    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
                    at_send(modem->at, "AT+NMGS=%d,", amount);
                    at_send_hex(modem->at, buffer, amount);
                    at_command_simple(modem->at, "");
                    return amount;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        }
        return -1;
    } else if(connid < NUMBER_SOCKETS) {
        struct socket_info *info = &priv->sockets[connid];
        if(info->status == SOCKET_STATUS_CONNECTED) {
            amount = amount > 512 ? 512 : amount;
            at_set_timeout(modem->at, AT_TIMEOUT_LONG);
            at_send(modem->at, "AT+NSOST=%d,%s,%d,%d,", connid, info->host, info->port, amount);
            at_send_hex(modem->at, buffer, amount);
            const char* response = at_command(modem->at, "");
            at_simple_scanf(response, "%*d, %d", &amount);
            return amount;
        }
    }

    return 0;
}

static int scanner_nmgr(const char *line, size_t len, void *arg)
{
    (void) arg;

    if (at_prefix_in_table(line, nb501_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    if (sscanf(line, "%d,", &len) == 1) {
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

static int scanner_nsorf(const char *line, size_t len, void *arg)
{
    (void) arg;

    if (at_prefix_in_table(line, nb501_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    if (sscanf(line, "%*d,%*[^,],%*d,%d,", &len) == 1) {
        if (len > 0) {
            return AT_RESPONSE_HEXDATA_FOLLOWS(len);
        }
    }

    return AT_RESPONSE_UNKNOWN;
}

static char character_handler_nsorf(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;

    if(ch == ',') {
        line[len] = '\0';
        if (sscanf(line, "%*d,%*[^,],%*d,%d,", &len) == 1) {
            at_set_character_handler(priv, NULL);
            ch = '\n';
        }
    }

    return ch;
}

static ssize_t nb501_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;
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
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            at_set_character_handler(modem->at, character_handler_nsorf);
            at_set_command_scanner(modem->at, scanner_nsorf);
            const char *response = at_command(modem->at, "AT+NSORF=%d,%d", connid, length);
            if (response == NULL) {
                DBG_W(">>>>NO RESPONSE\r\n");
                return -2;
            }
            if (*response == '\0') {
                return 0;
            }
            /* Find the header line. */
            int read;
            if(sscanf(response, "%*d,%*[^,],%*d,%d", &read) != 1) {
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
        }
    }

    return 0;
}

static int nb501_socket_waitack(struct cellular *modem, int connid)
{
    return 0;
}

static int nb501_socket_close(struct cellular *modem, int connid)
{
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;

    if(connid == CELLULAR_NB_CONNID) {
        return 0;
    } else if(connid < NUMBER_SOCKETS) {
        struct socket_info *info = &priv->sockets[connid];

        if(info->status == SOCKET_STATUS_CONNECTED) {
            info->status = SOCKET_STATUS_UNKNOWN;
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            at_command_simple(modem->at, "AT+NSOCL=%d", connid);
        }
    }

    return 0;
}

static int nb501_op_creg(struct cellular *modem)
{
    int creg;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CEREG?");
    at_simple_scanf(response, "+CEREG: %*d,%d", &creg);

    return creg;
}

static int nb501_op_cops(struct cellular *modem)
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

static int nb501_op_imei(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CGSN=1");
    if(response == NULL) {
        return -2;
    }
    if(len > CELLULAR_IMEI_LENGTH && sscanf(response, "+CGSN:%16s", buf) == 1) {

    } else {
        return -1;
    }

    return 0;
}

static int nb501_op_nccid(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    const char *response = at_command(modem->at, "AT+NCCID");
    if(response == NULL) {
        return -2;
    }
    if(len > CELLULAR_ICCID_LENGTH && sscanf(response, "+NCCID:%21s", buf) == 1) {
        strncpy(buf, response, len);
    } else {
        return -1;
    }

    return 0;
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

static int nb501_op_reset(struct cellular *modem)
{
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;

    // Cleanup
    memset(&priv->state, 0, sizeof(priv->state));
    memset(priv->sockets, 0, sizeof(priv->sockets));

    // Set CDP
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+CFUN=0");
    at_command_simple(modem->at, "AT+NCDP=180.101.147.115");

    // Reboot
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_set_character_handler(modem->at, character_handler_nrb);
    if(at_command(modem->at, "AT+NRB") == NULL) {
        return -2;
    } else {
        /* Delay 2 seconds to continue */
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* Initialize modem. */
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        for (const char *const *command=nb501_init_commands; *command; command++) {
            at_command_simple(modem->at, "%s", *command);
        }
        at_command_simple(modem->at, "AT+CGDCONT=1,\"IP\",\"%s\"", modem->apn);
        at_command_simple(modem->at, "AT+CPSMS=1,,,01011111,00000000");
    }

    return 0;
}

static int nb501_suspend(struct cellular *modem)
{
    at_suspend(modem->at);

    return 0;
}

static int nb501_resume(struct cellular *modem)
{
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;

    at_resume(modem->at);

    /* Initialize modem. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    for (const char *const *command=nb501_init_commands; *command; command++) {
        at_command_simple(modem->at, "%s", *command);
    }
    at_command(modem->at, "AT+CGDCONT=1,\"IP\",\"%s\"", modem->apn);
    at_command(modem->at, "AT+CPSMS=1,,,01011111,00000000");

    at_command_simple(modem->at, "AT+CSCON?");
    at_command_simple(modem->at, "AT+NPSMR?");

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

    return nb501_op_reset(modem);
}

static const struct cellular_ops nb501_ops = {
    .reset = nb501_op_reset,
    .attach = nb501_attach,
    .detach = nb501_detach,
    .suspend = nb501_suspend,
    .resume = nb501_resume,

    .pdp_open = nb501_pdp_open,
    .pdp_close = nb501_pdp_close,
    .shutdown = nb501_shutdown,

    .imei = nb501_op_imei,
    .iccid = nb501_op_nccid,
    .imsi = cellular_op_imsi,
    .creg = nb501_op_creg,
    .cgatt = cellular_op_cgatt,
    .rssi = cellular_op_rssi,
    .cops = nb501_op_cops,
    .test = cellular_op_test,
    .command = cellular_op_command,
    .sms = cellular_op_sms,
    .cnum = cellular_op_cnum,
    .onum = cellular_op_onum,
    .socket_connect = nb501_socket_connect,
    .socket_send = nb501_socket_send,
    .socket_recv = nb501_socket_recv,
    .socket_waitack = nb501_socket_waitack,
    .socket_close = nb501_socket_close,
};

static struct cellular_nb501 cellular;

struct cellular *cellular_alloc(void)
{
    struct cellular_nb501 *modem = &cellular;

    memset(modem, 0, sizeof(*modem));
    modem->dev.ops = &nb501_ops;

    return (struct cellular *) modem;
}

void cellular_free(struct cellular *modem)
{
}

/* vim: set ts=4 sw=4 et: */
