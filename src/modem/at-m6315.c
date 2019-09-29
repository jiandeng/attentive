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

#define printf(...)

/*
 * M6315 probably holds the highly esteemed position of the world's worst
 * behaving GSM modem, ever. The following quirks have been spotted so far:
 * - response continues after OK (AT+CIPSTATUS)
 * - response without a final OK (AT+CIFSR)
 * - freeform URCs coming at random moments like "DST: 1" (AT+CLTS=1)
 * - undocumented URCs like "+CIEV: ..." (AT+CLTS=1)
 * - text-only URCs like "NORMAL POWER DOWN"
 * - suffix-based URCs like "1, CONNECT OK" (AT+CIPSTART)
 * - bizarre OK responses like "SHUT OK" (AT+CIPSHUT)
 * - responses without a final OK (sic!) (AT+CIFSR)
 * - no response at all (AT&K0)
 * We work it all around, but it makes the code unnecessarily complex.
 */

#define M6315_AUTOBAUD_ATTEMPTS     10
#define M6315_CONFIG_RETRIES        10
#define M6315_WAITACK_TIMEOUT       24        // Retransmission mechanism: 1.5 + 3 + 6 + 12 = 22.5
#define M6315_CGACT_TIMEOUT         (45 + 10)  // According to the AT_Command_Manual
#define M6315_TCP_CONNECT_TIMEOUT   (75 + 10)  // According to the AT_Command_Manual
#define M6315_TCP_CONNECT_RETRIES   3         // According to the AT_Command_Manual
#define M6315_QIDEACT_TIMEOUT       (40 + 10) // According to the AT_command_Manual
#define M6315_NSOCKETS              8
#define NTP_BUF_SIZE                4

enum m6315_socket_status {
    M6315_SOCKET_STATUS_ERROR = -1,
    M6315_SOCKET_STATUS_UNKNOWN = 0,
    M6315_SOCKET_STATUS_CONNECTED = 1,
};



static const char *const m6315_urc_responses[] = {
    "+QIRDI:",          /* Incoming socket data notification */
    "+PDP: DEACT",      /* PDP disconnected */
    "+SAPBR 1: DEACT",  /* PDP disconnected (for SAPBR apps) */
    "*PSNWID: ",        /* AT+CLTS network name */
    "*PSUTTZ: ",        /* AT+CLTS time */
    "+CTZV: ",          /* AT+CLTS timezone */
    "DST: ",            /* AT+CLTS dst information */
    "+CIEV: ",          /* AT+CLTS undocumented indicator */
    "RDY",              /* Assorted crap on newer firmware releases. */
    "+CFUN:",
    "+CPIN:",
    "Call Ready",
    "SMS Ready",
    "NORMAL POWER DOWN",
    "UNDER-VOLTAGE POWER DOWN",
    "UNDER-VOLTAGE WARNNING",
    "OVER-VOLTAGE POWER DOWN",
    "OVER-VOLTAGE WARNNING",
    "Operator",
    NULL
};

struct cellular_m6315 {
    struct cellular dev;

    enum m6315_socket_status socket_status[M6315_NSOCKETS];
    enum m6315_socket_status spp_status;
    int spp_connid;
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
    struct cellular_m6315 *priv = arg;

    if (at_prefix_in_table(line, m6315_urc_responses))
        return AT_RESPONSE_URC;

    /* Socket status notifications in form of "%d, <status>". */
    if (line[0] >= '0' && line[0] <= '0' + M6315_NSOCKETS && line[1] == ',')
    {
        int socket = line[0] - '0';
        if(line[2] == ' ') {
            line += 3;
        } else {
            line += 2;
        }

        if (!strcmp(line, "CONNECT OK"))
        {
            priv->socket_status[socket] = M6315_SOCKET_STATUS_CONNECTED;
            return AT_RESPONSE_URC;
        }

        if (!strcmp(line, "CONNECT FAIL") ||
            !strcmp(line, "ALREADY CONNECT") ||
            !strcmp(line, "CLOSED"))
        {
            priv->socket_status[socket] = M6315_SOCKET_STATUS_ERROR;
            return AT_RESPONSE_URC;
        }
    }

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    // struct cellular_m6315 *priv = arg;

    DBG_D("U> %s\r\n", line);
}

static const struct at_callbacks m6315_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};

static int m6315_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &m6315_callbacks, (void *) modem);

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    /* Perform autobauding. */
    for (int i=0; i<M6315_AUTOBAUD_ATTEMPTS; i++) {
        const char *response = at_command(modem->at, "AT");
        if (response != NULL)
            /* Modem replied. Good. */
            break;
    }

    /* Disable local echo. */
    at_command(modem->at, "ATE0");

    /* Disable local echo again; make sure it was disabled successfully. */
    at_command(modem->at, "ATE0");

    /* Delay 2 seconds to continue */
    vTaskDelay(pdMS_TO_TICKS(2000));
    at_command(modem->at, "AT+CGMM");
    at_command(modem->at, "AT+CGMR");

    /* Initialize modem. */
    static const char *const init_strings[] = {
//        "AT+IPR=0",                     /* Enable autobauding if not already enabled. */
//        "AT+IFC=0,0",                   /* Disable hardware flow control. */
        "AT+CMEE=2",                    /* Enable extended error reporting. */
        "AT+QIURC=0",                   /* Disable "Call Ready" URC. */
//        "AT&W0",                        /* Save configuration. */
        NULL
    };
    for (const char *const *command=init_strings; *command; command++) {
        at_command(modem->at, "%s", *command);
    }

    /* Enable full functionality */
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    for (int i=0; i < M6315_CONFIG_RETRIES; i++) {
        const char* response = at_command(modem->at, "AT+CFUN=1");
        if(response == NULL) {
            // timeout
            return -2;
        } else if(*response == '\0') {
            // ok
            break;
        } else {
            // error
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    return 0;
}

static int m6315_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int m6315_suspend(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command(modem->at, "AT+QSCLK=2");
    at_suspend(modem->at);

    return 0;
}

static int m6315_resume(struct cellular *modem)
{
    at_resume(modem->at);
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command(modem->at, "AT+QSCLK=0");
    at_command_simple(modem->at, "AT+QSCLK=0");

    return 0;
}

//static int m6315_clock_gettime(struct cellular *modem, struct timespec *ts)
//{
//    /* TODO: See CYC-1255. */
//    return -1;
//}

//static int m6315_clock_settime(struct cellular *modem, const struct timespec *ts)
//{
//    /* TODO: See CYC-1255. */
//    return -1;
//}

//static int m6315_clock_ntptime(struct cellular *modem, struct timespec *ts)
//{
//    /* network stuff. */
//    int socket = 2;

//    if (modem->ops->socket_connect(modem, socket, "time-nw.nist.gov", 37) == 0) {
//        printf("m6315: connect successful\n");
//    } else {
//        perror("m6315: connect failed");
//        goto close_conn;
//    }

//    int len = 0;
//    char buf[NTP_BUF_SIZE];
//    while ((len = modem->ops->socket_recv(modem, socket, buf, NTP_BUF_SIZE, 0)) >= 0)
//    {
//        if (len > 0)
//        {
//            printf("Received: >\x1b[1m");
//            for (int i = 0; i<len; i++)
//            {
//                printf("%02x", buf[i]);
//            }
//            printf("\x1b[0m<\n");

//            if (len == 4)
//            {
//                ts->tv_sec = 0;
//                for (int i = 0; i<4; i++)
//                {
//                    ts->tv_sec = (long int)buf[i] + ts->tv_sec*256;
//                }
//                printf("m6315: catched UTC timestamp -> %d\n", ts->tv_sec);
//                ts->tv_sec -= 2208988800L;        //UTC to UNIX time conversion
//                printf("m6315: final UNIX timestamp -> %d\n", ts->tv_sec);
//                goto close_conn;
//            }

//            len = 0;
//        }
//        else
//            vTaskDelay(pdMS_TO_TICKS(1000));
//    }

//#if 0
//    // FIXME: It's wrong. It should be removed
//    while (modem->ops->socket_recv(modem, socket, NULL, 1, 0) != 0)
//    {} //flush
//#endif

//close_conn:
//    if (modem->ops->socket_close(modem, socket) == 0)
//    {
//        printf("m6315: close successful\n");
//    } else {
//        perror("m6315: close");
//    }

//    return 0;
//}

static int m6315_pdp_open(struct cellular *modem, const char *apn)
{
    /* Configure IP application. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    /* Switch to multiple connections mode; it's less buggy. */
    if (at_config(modem->at, "QIMUX", "1", M6315_CONFIG_RETRIES) != 0) {
        return -1;
    }
    /* Receive data manually. */
    if (at_config(modem->at, "QINDI", "1", M6315_CONFIG_RETRIES) != 0) {
        return -1;
    }
    /* Enable send data echo. */
    if (at_config(modem->at, "QISDE", "0", M6315_CONFIG_RETRIES) != 0) {
        return -1;
    }

    int active = 0;
    /* Skip the configuration if context is already open. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CGACT?");
    if(strlen(response)) {
        at_simple_scanf(response, "+CGACT: %*d,%d", &active);
    }
    if(active) {
        return 0;
    }
    /* Skip the configuration if context is already open. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    /* Configure and open internal pdp context. */
    at_command_simple(modem->at, "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
    at_set_timeout(modem->at, M6315_CGACT_TIMEOUT);
    at_command_simple(modem->at, "AT+CGACT=1,1");

    return 0;
}


static enum at_response_type scanner_qideact(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;
    if (!strcmp(line, "DEACT OK"))
        return AT_RESPONSE_FINAL_OK;
    return AT_RESPONSE_UNKNOWN;
}

static int m6315_pdp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, M6315_QIDEACT_TIMEOUT);
    at_set_command_scanner(modem->at, scanner_qideact);
    at_command_simple(modem->at, "AT+QIDEACT");

    return 0;
}

static enum at_response_type scanner_shutdown(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    if (!strcmp(line, "NORMAL POWER DOWN"))
        return AT_RESPONSE_FINAL_OK;
    return AT_RESPONSE_UNKNOWN;
}

static int m6315_shutdown(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT");

    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_set_command_scanner(modem->at, scanner_shutdown);
    at_command_simple(modem->at, "AT+QPOWD=1");

    return 0;
}

static enum at_response_type scanner_qiclose(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int connid;
    char last;
    if (sscanf(line, "%d, CLOSE O%c", &connid, &last) == 2 && last == 'K')
        return AT_RESPONSE_FINAL_OK;
    return AT_RESPONSE_UNKNOWN;
}

int m6315_socket_close(struct cellular *modem, int connid)
{
    struct cellular_m6315 *priv = (struct cellular_m6315 *) modem;

    if(connid >= 0 && connid < M6315_NSOCKETS) {
      at_set_timeout(modem->at, AT_TIMEOUT_LONG);
      at_set_command_scanner(modem->at, scanner_qiclose);
      at_command_simple(modem->at, "AT+QICLOSE=%d", connid);

      priv->socket_status[connid] = M6315_SOCKET_STATUS_UNKNOWN;
    }
    return 0;
}

static int m6315_socket_connect(struct cellular *modem, const char *host, uint16_t port)
{
    struct cellular_m6315 *priv = (struct cellular_m6315 *) modem;
    int connid = -1;

    if(host == NULL && port == 0) {
        return -1;
    } else {
        for(int i = 0; i < M6315_NSOCKETS; i++) {
            if(M6315_SOCKET_STATUS_UNKNOWN == priv->socket_status[i]) {
                connid = i;
                break;
            }
        }
        if(connid < 0) {
            return -1;
        }
        /* Send connection request. */
        int i = 0;
        int n = 0;
        do {
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            priv->socket_status[connid] = M6315_SOCKET_STATUS_UNKNOWN;
            at_command_simple(modem->at, "AT+QIOPEN=%d,\"TCP\",\"%s\",%d", connid, host, port);
            /* Wait for socket status URC. */
            while(i++ < M6315_TCP_CONNECT_TIMEOUT) {
                if (priv->socket_status[connid] == M6315_SOCKET_STATUS_CONNECTED) {
                    return connid;
                } else if (priv->socket_status[connid] == M6315_SOCKET_STATUS_ERROR) {
                    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
                    at_set_command_scanner(modem->at, scanner_qiclose);
                    at_command_simple(modem->at, "AT+QICLOSE=%d", connid);
                    break;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        } while(++n < M6315_TCP_CONNECT_RETRIES);
    }

    return -1;
}

static enum at_response_type scanner_qisend(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int connid, amount;
    char last;
    if (sscanf(line, "DATA ACCEPT:%d,%d", &connid, &amount) == 2)
        return AT_RESPONSE_FINAL_OK;
    if (sscanf(line, "%d, SEND O%c", &connid, &last) == 2 && last == 'K')
        return AT_RESPONSE_FINAL_OK;
    if (sscanf(line, "%d, SEND FAI%c", &connid, &last) == 2 && last == 'L')
        return AT_RESPONSE_FINAL;
    if (!strcmp(line, "SEND OK"))
        return AT_RESPONSE_FINAL_OK;
    if (!strcmp(line, "SEND FAIL"))
        return AT_RESPONSE_FINAL;
    return AT_RESPONSE_UNKNOWN;
}

static ssize_t m6315_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    struct cellular_m6315 *priv = (struct cellular_m6315 *) modem;
    (void) flags;
    if(connid >= 0 && connid < M6315_NSOCKETS) {
      if(priv->socket_status[connid] != M6315_SOCKET_STATUS_CONNECTED) {
        return -1;
      }
      amount = amount > 1460 ? 1460 : amount;
      /* Request transmission. */
      at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
      at_expect_dataprompt(modem->at, "> ");
      at_command_simple(modem->at, "AT+QISEND=%d,%d", connid, amount);

      /* Send raw data. */
      at_set_command_scanner(modem->at, scanner_qisend);
      at_command_raw_simple(modem->at, buffer, amount);
    } else {
      return 0;
    }

    return amount;
}

static int scanner_qird(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int read; // TODO: fixme
    if (sscanf(line, "+QIRD: %*[^,],TCP,%d", &read) == 1)
        if (read > 0)
            return AT_RESPONSE_RAWDATA_FOLLOWS(read);

    return AT_RESPONSE_UNKNOWN;
}

static ssize_t m6315_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    struct cellular_m6315 *priv = (struct cellular_m6315 *) modem;
    (void) flags;

    int cnt = 0;
    // TODO its dumb and exceptions should be handled in other right way
    // FIXME: It has to be changed. Leave for now
    if(connid >= 0 && connid < M6315_NSOCKETS) {
      if(priv->socket_status[connid] != M6315_SOCKET_STATUS_CONNECTED) {
        DBG_I(">>>>DISCONNECTED\r\n");
        return -1;
      }
      char tries = 4;
      while ( (cnt < (int) length) && tries-- ){
          int chunk = (int) length - cnt;
          /* Limit read size to avoid overflowing AT response buffer. */
          chunk = chunk > 480 ? 480 : chunk;

          /* Perform the read. */
          at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
          at_set_command_scanner(modem->at, scanner_qird);
          const char *response = at_command(modem->at, "AT+QIRD=0,1,%d,%d", connid, chunk);
          if (response == NULL) {
              DBG_W(">>>>NO RESPONSE\r\n");
              return -2;
          }
          /* Find the header line. */
          int read; // data read from the receive buffer
          // TODO: connid is not checked
          if(sscanf(response, "+QIRD: %*[^,],TCP,%d", &read) != 1) {
              DBG_I(">>>>BAD RESPONSE\r\n");
              return -1;
          }

          /* Bail out if we're out of data. */
          /* FIXME: We should maybe block until we receive something? */
          if (read == 0) {
               break;
          }

          /* Locate the payload. */
          /* TODO: what if no \n is in input stream?
           * should use strnchr at least */
          const char *data = strchr(response, '\n');
          if (data++ == NULL) {
              DBG_I(">>>>NO DATA\r\n");
              return -1;
          }

          /* Copy payload to result buffer. */
          memcpy((char *)buffer + cnt, data, read);
          cnt += read;
      }
    }

    return cnt;
}

static int m6315_socket_waitack(struct cellular *modem, int connid)
{
    const char *response;
    if(connid >= 0 && connid < M6315_NSOCKETS) {
      at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
      for (int i=0; i<M6315_WAITACK_TIMEOUT*2; i++) {
          /* Read number of bytes waiting. */
          int nacklen;
          response = at_command(modem->at, "AT+QISACK=%d", connid);
          at_simple_scanf(response, "+QISACK: %*d,%*d,%d", &nacklen);

          /* Return if all bytes were acknowledged. */
          if (nacklen == 0)
              return 0;

          vTaskDelay(pdMS_TO_TICKS(500));
      }
    }
    return -1;
}

static const struct cellular_ops m6315_ops = {
    .attach = m6315_attach,
    .detach = m6315_detach,

    .suspend = m6315_suspend,
    .resume = m6315_resume,

    .pdp_open = m6315_pdp_open,
    .pdp_close = m6315_pdp_close,
    .shutdown = m6315_shutdown,

    .imei = cellular_op_imei,
    .iccid = cellular_op_iccid,
    .imsi = cellular_op_imsi,
    .creg = cellular_op_creg,
    .cgatt = cellular_op_cgatt,
    .rssi = cellular_op_csq,
    .cops = cellular_op_cops,
    .test = cellular_op_test,
    .command = cellular_op_command,
    .sms = cellular_op_sms,
    .cnum = cellular_op_cnum,
    .onum = cellular_op_onum,
//    .clock_gettime = m6315_clock_gettime,
//    .clock_settime = m6315_clock_settime,
//    .clock_ntptime = m6315_clock_ntptime,
    .socket_connect = m6315_socket_connect,
    .socket_send = m6315_socket_send,
    .socket_recv = m6315_socket_recv,
    .socket_waitack = m6315_socket_waitack,
    .socket_close = m6315_socket_close,
};

static struct cellular_m6315 cellular;

struct cellular *cellular_alloc(void)
{
    struct cellular_m6315 *modem = &cellular;
    memset(modem, 0, sizeof(*modem));

    modem->dev.ops = &m6315_ops;

    return (struct cellular *) modem;
}

void cellular_free(struct cellular *modem)
{
}

/* vim: set ts=4 sw=4 et: */
