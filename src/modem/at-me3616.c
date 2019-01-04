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
#include "dev_comm.h"
#include "hal_cfg.h"

/* Defines -------------------------------------------------------------------*/
DBG_SET_LEVEL(DBG_LEVEL_E);

#define AUTOBAUD_ATTEMPTS         10
#define NUMBER_SOCKETS            7
#define RESUME_TIMEOUT            60
#define SOCKET_RECV_TIMEOUT       20
#define IOT_CONNECT_TIMEOUT       30

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

static const char *const me3616_urc_responses[] = {
    /* "+NPSMR:", */
    /* "+CSCON:", */
    /* "+NSONMI:", */
    /* "+NNMI:", */
    /* "+NPING:", */

     "*MNBIOTEVENT:",
     "+M2MCLI:",
    //"+M2MCLIRECV:",
     //"+ESONMI=",
    NULL,
};

struct cellular_me3616 {
    struct cellular dev;
    struct modem_state state;
    struct socket_info sockets[NUMBER_SOCKETS];
    dev_comm_t *dev_obj;
    struct socket_info iot_sock;
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    if (at_prefix_in_table(line, me3616_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    return AT_RESPONSE_UNKNOWN;
}
/*
static uint32_t hex_to_bytes(uint8_t *bytes, const char * hex, uint32_t len) {
    if(len == 0) {
        len = strlen(hex);
    }
    if(len % 2) {
        return 0;
    }
    uint8_t *p = bytes;
    for(int i = 0; i < len; i+=2) {
        uint8_t low = toupper(hex[i+1]);
        uint8_t high = toupper(hex[i]);
        if(low > 0x39) {
            low -= 0x37;
        } else {
            low -= 0x30;
        }
        if(high> 0x39) {
            high-= 0x37;
        } else {
            high-= 0x30;
        }
        *p = (high<<4) |low;
        p++;
    }
    return len/2;
}
*/
static void handle_urc(const char *line, size_t len, void *arg)
{
    struct cellular_me3616 *modem = (struct cellular_me3616*) arg;
    (void) len;
    DBG_D("URC> %s\r\n", line);
    char *p_res = NULL;
    p_res = strstr(line,"+M2MCLI:\0");
    if(p_res != NULL)
    {
        if(strstr(p_res,"observe success\0"))
        {
            modem->iot_sock.status = SOCKET_STATUS_CONNECTED;
        }
        else if(strstr(p_res,"deregister success\0"))
        {
            modem->iot_sock.status = SOCKET_STATUS_UNKNOWN;
        }
    }

}
/*static void handle_urc(const char *line, size_t len, void *arg)
{
    struct cellular_me3616 *modem = (struct cellular_me3616*) arg;
    (void) len;
    int state = 0;
    int connid = -1;
    uint8_t *data = NULL;
    uint32_t data_size = 0;
    DBG_D("URC> %s\r\n", line);
    if(!strncmp(me3616_urc_responses[2],line,strlen(me3616_urc_responses[2]))) {
        DBG_D("%d,%s\r\n",len,line +strlen(me3616_urc_responses[2]));
        if((len - strlen(me3616_urc_responses[2])) % 2) {
            DBG_E("Invalid response");
            return;
        }
        data = (uint8_t *)pvPortMalloc((len - strlen(me3616_urc_responses))/2);
        if(!data) {
            DBG_E("Malloc error");
            return;
        }
        data_size = hex_to_bytes(data,line + strlen(me3616_urc_responses[2]),len - strlen(me3616_urc_responses[2]));
        connid = CELLULAR_NB_CONNID;
   }
   else if(!strncmp(me3616_urc_responses[3],line,strlen(me3616_urc_responses[3]))) {


        if(sscanf(line + strlen(me3616_urc_responses[3]), "%d,%d", &connid, &data_size)) {
            char *p = strrchr(line + strlen(me3616_urc_responses[3]),',');

            if(p){
                data = (uint8_t *)pvPortMalloc(data_size);
                hex_to_bytes(data,p+1,data_size * 2);
            }
        }
   }
    //extern void hook_me3616(dev_comm_t *obj,int connid,void *, int32_t size); // fix me
    // if(connid >= 0) {
    //     hook_me3616(modem->dev_obj, connid ,data, data_size);
    // }
    
}
*/
static const struct at_callbacks me3616_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};


static int me3616_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &me3616_callbacks, (void *) modem);
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    vTaskDelay(pdMS_TO_TICKS(2000));
    //at_command(modem->at,"AT+ZRST");
    
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
        "AT+CFUN=1",
        "AT+CMEE=1",
        /* "AT+CREG=2", */
        "AT+CPSMS=1,,,\"01011111\",\"00000101\"",
        "AT+ZSLR=1",   //系统启动后处于可睡眠状态
        "AT*MNBIOTEVENT=1,1",
        //"AT+ESOREADEN=1",
        NULL
    };
    DBG_D("me3616_attach: AT command simple\r\n");
    for (const char *const *command=init_strings; *command; command++)
        at_command_simple(modem->at, "%s", *command);
    at_command(modem->at,"AT+M2MCLIDEL",10);

    return 0;
}

static int me3616_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int me3616_pdp_open(struct cellular *modem, const char *apn)
{
    return 0;
}

static int me3616_pdp_close(struct cellular *modem)
{
    return 0;
}

static int me3616_shutdown(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+CFUN=0");

    return 0;
}

static int me3616_socket_connect(struct cellular *modem, const char *host, uint16_t port)
{
    struct cellular_me3616 *priv = (struct cellular_me3616 *) modem;
    int connid = -1;
    if(host == NULL || *host == 0 || port == 0) {
        /*//const char *response = at_command(modem->at, "AT+CGSN=1");
        char imei[CELLULAR_IMEI_LENGTH+1];
        int ret = modem->ops->imei(modem,imei,sizeof(imei));
        if(ret)
        {
            return connid;
        }
        DBG_D("net connecting to host:180.101.147.115,port:5683\r\n");
        at_command_simple(modem->at,"AT+M2MCLINEW=180.101.147.115,5683,\"%s\",90",imei);

        return CELLULAR_NB_CONNID;
        */
        struct socket_info *info = &priv->iot_sock;
        if(info->status != SOCKET_STATUS_CONNECTED) {
            char IMEI[CELLULAR_IMEI_LENGTH+1];
            memset(IMEI,0,sizeof(IMEI));
            int ret = modem->ops->imei(modem,IMEI,sizeof(IMEI));
            if(ret==0){
            //if(cellular_op_imei(modem, (char*)IMEI, sizeof(IMEI)) == 0) {
                at_set_timeout(modem->at, IOT_CONNECT_TIMEOUT);
                at_command_simple(modem->at, "AT+M2MCLINEW=180.101.147.115,5683,\"%s\",90", IMEI);
                DBG_I("socket_connect:connecting to host: 180.101.147.115,port = 5683");
                for(int i = 0; i < IOT_CONNECT_TIMEOUT; i++) {
                    if(SOCKET_STATUS_CONNECTED == info->status) {
                        return CELLULAR_NB_CONNID;
                    }
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            else
            {
                DBG_E("\r\nsocket_connect:read imei failed of %d!%s\r\n",ret,IMEI);
            }
        }
        else{DBG_E("\r\nsocket_connect:net has connected!\r\n");}
        return -1;
    } else {
        /* Create an udp socket. */
        DBG_D("net connecting to host:%s,port:%d",host,port);
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        const char *response = at_command(modem->at, "AT+ESOC=1,2,1");
        at_simple_scanf(response, "+ESOC=%d", &connid);
        if(connid >= NUMBER_SOCKETS) {
            return -1;
        }
        at_command(modem->at, "AT+ESOCON=%d,%d,\"%s\"", connid, port, host);
        struct socket_info *info = &priv->sockets[connid];
        info->status = SOCKET_STATUS_CONNECTED;
    }
    return connid;
}

static ssize_t me3616_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    struct cellular_me3616 *priv = (struct cellular_me3616 *) modem;
    (void) flags;
    DBG_D("at-me3616: sending to connid %d",connid);
    if(connid == CELLULAR_NB_CONNID) { // TODO: fixme
        amount = amount > HAL_CFG_CELL_MTU ? HAL_CFG_CELL_MTU : amount;
        at_set_timeout(modem->at, AT_TIMEOUT_LONG);
        at_send(modem->at, "AT+M2MCLISEND=");
        at_send_hex(modem->at, buffer, amount);
        //at_command_simple(modem->at, "");
        const char *_response = at_command(modem->at, "");
        DBG_D("at-me3616: at command response> %s",_response);  
        if (!_response)
            return -2;
        if (strcmp(_response, "")) {
            return -1;
        }
        return amount;
    } else if(connid < NUMBER_SOCKETS) {
        struct socket_info *info = &priv->sockets[connid];
        if(info->status == SOCKET_STATUS_CONNECTED) {
            amount = amount > 512 ? 512 : amount;
            at_set_timeout(modem->at, AT_TIMEOUT_LONG);
            at_send(modem->at, "AT+ESOSEND=%d,%d,", connid, amount);
            at_send_hex(modem->at, buffer, amount);
            at_command_simple(modem->at, "");
            return amount;
        }
    }
    return 0;
}
static int clirecv_len = 0; 
static int scanner_clirecv(char *line, size_t len, void *arg)
{
    (void) arg;

    if (at_prefix_in_table(line, me3616_urc_responses)) {
        return AT_RESPONSE_URC;
    }
    //line[len] = 0;//for debug
    //DBG_D("scanner_clirecv:%s",line);
    // if (sscanf(line, "%d", &len) == 1) {
    //     if (len > 0) {
    //         return AT_RESPONSE_HEXDATA_FOLLOWS(len);
    //     }
    // }
    
    static bool reading = false;
    if (strstr(line, "+M2MCLIRECV:")) {
        //DBG_D("scanner_clirecv:received +M2MCLIRECV:");
        reading = true;
        return AT_RESPONSE_HEXDATA_FOLLOWS(0);
    } else if(reading) {
        DBG_D("scanner_clirecv:received complete\r\n");
        clirecv_len = len;
        return AT_RESPONSE_FINAL;
    }

    return AT_RESPONSE_UNKNOWN;
}

static char character_handler_clirecv(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;
    line[len] = 0;
    //DBG_D("character_handler_clirecv:%s",line);
    // if(ch == ':') {
    //     line[len] = '\0';
    //     at_set_character_handler(priv, NULL);
    //     ch = '\n';
    // }
    
    if(ch == ':') {
        DBG_D("character_handler_clirecv:received :");
        line[len] = '\0';
        if (strstr(line, "+M2MCLIRECV:")) {
            at_set_character_handler(priv, NULL);
            ch = '\n';
        }
    }
    return ch;
}

static int scanner_csonmi(const char *line, size_t len, void *arg)
{
    (void) arg;

    if (at_prefix_in_table(line, me3616_urc_responses)) {
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
static ssize_t me3616_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    struct cellular_me3616 *priv = (struct cellular_me3616 *) modem;
    (void) flags;
    DBG_I("at-me3616:%s",__FUNCTION__);
    if(connid == CELLULAR_NB_CONNID) {
        struct socket_info *info = &priv->iot_sock;
        if(info->status == SOCKET_STATUS_CONNECTED) {
            DBG_I("at-me3616:CELLULAR_NB receiving");
            /* Perform the read. */
            at_set_timeout(modem->at, SOCKET_RECV_TIMEOUT);
            at_set_character_handler(modem->at, character_handler_clirecv);
            at_set_command_scanner(modem->at, scanner_clirecv);
            const char *response = at_command(modem->at, "");
            if (response == NULL) {
                DBG_W(">>>>NO RESPONSE\r\n");
                return -2;
            }
            if (*response == '\0') {
                return 0;
            }
            /* Find the header line. */
            if(strncmp(response, "+M2MCLIRECV:", strlen("+M2MCLIRECV:"))) {
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
            int read = clirecv_len;
            memcpy((char *)buffer, data, read); // TODO: fixme
            DBG_I("--------at-me3616:received len = %d-------",read);
            // for(uint16_t i=0;i<read;i++)
            // {
            //     NRF_LOG_RAW_INFO("%02x",data[i]);
            // }
            // NRF_LOG_RAW_INFO("\r\n");
            return read;
        }
        else
        {
            DBG_I("SOCKET STATUS not connect,%d",info->status );
        }
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

static int me3616_socket_waitack(struct cellular *modem, int connid)
{
    return 0;
}

static int me3616_socket_close(struct cellular *modem, int connid)
{
    struct cellular_me3616 *priv = (struct cellular_me3616 *) modem;

    if(connid == CELLULAR_NB_CONNID) {
        struct socket_info *info = &priv->iot_sock;
        if(info->status == SOCKET_STATUS_CONNECTED) {
            info->status = SOCKET_STATUS_UNKNOWN;
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            at_command_simple(modem->at, "AT+M2MCLIDEL");
        }
    } else if(connid < NUMBER_SOCKETS) {
        DBG_E("CLOSE %d",SOCKET_STATUS_CONNECTED);
        struct socket_info *info = &priv->sockets[connid];
        if(info->status == SOCKET_STATUS_CONNECTED) {
            info->status = SOCKET_STATUS_UNKNOWN;
            at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
            at_command_simple(modem->at, "AT+ESOCL=%d", connid);
        }
    }

    return 0;
}

static int me3616_op_creg(struct cellular *modem)
{
    int creg;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CEREG?");
    printf("--->%s\n",response);
    at_simple_scanf(response, "+CEREG: %*d,%d",&creg);
    return creg;
}

static int me3616_op_cops(struct cellular *modem)
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

static int me3616_op_reset(struct cellular *modem)
{
    DBG_I("AT-3616: Resetting\r\n");
    struct cellular_me3616 *priv = (struct cellular_me3616 *) modem;

    // Cleanup
    memset(&priv->state, 0, sizeof(priv->state));
    memset(priv->sockets, 0, sizeof(priv->sockets));
    memset(&priv->iot_sock, 0, sizeof(priv->iot_sock));

    at_command(modem->at,"AT+ZRST");

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    for (int i=0; i<10; i++) {
        const char *response = at_command(modem->at, "ATE0");
        if (response != NULL && *response == '\0') {
            break;
        }
    }
    // Set CDP
    {
        /* Delay 2 seconds to continue */
        vTaskDelay(pdMS_TO_TICKS(2000));

        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        at_command_simple(modem->at,"AT");
        //at_command_simple(modem->at,"AT*MNBIOTEVENT=1,1");
        at_command_simple(modem->at, "AT+CMEE=1");
        at_command_simple(modem->at,"AT+CEREG=1");
        at_command_simple(modem->at, "AT+CPSMS=1,,,\"01011111\",\"00000101\""); //8S后睡眠
        at_command_simple(modem->at,"AT+ZSLR=1");   //系统启动后处于可睡眠状态
    }

    return 0;
}

static int me3616_suspend(struct cellular *modem)
{
    //at_suspend(modem->at);
    return 0;
}

static int me3616_resume(struct cellular *modem)
{
    DBG_I("AT-3616: Resuming...\r\n");
    struct cellular_me3616 *priv = (struct cellular_me3616 *) modem;

    //at_resume(modem->at);
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    
    at_command_simple(modem->at, "AT+CFUN=1");
    at_command_simple(modem->at, "AT+CMEE=1");
    //at_command_simple(modem->at, "AT+CSCON=1");
    //at_command_simple(modem->at, "AT+NPSMR=1");
    //at_command_simple(modem->at, "AT+CSCON?");
    //at_command_simple(modem->at, "AT+NPSMR?");
    at_command_simple(modem->at, "AT+CPSMS=1,,,\"01011111\",\"00000101\""); //10S后睡眠
    at_command_simple(modem->at,"AT+ZSLR=1");   //系统启动后处于可睡眠状态
    int wake_count = 0;
    DBG_I("AT-3616: Ping www.baidu.com\r\n");
    const char* response = at_command(modem->at, "AT+NPING=www.baidu.com");
    if(response || *response == '\0') {
        DBG_I("AT-3616: Ping rsp %s\r\n",response);
        for(int i = 0; i < RESUME_TIMEOUT; i++) {
            DBG_I("AT-3616: cycle performing ping\r\n");
            wake_count += !priv->state.power_saving;
            if(priv->state.radio_connected) {
                DBG_I("AT-3616: priv->state.radio_connected=0\r\n");
                return 0;
            } else if(wake_count && priv->state.power_saving) {
                break;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    return me3616_op_reset(modem);
}

static int me3616_op_imei(struct cellular *modem, char *buf, size_t len)
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

static int me3616_op_iccid(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT*MICCID");
    if(response == NULL) {
        return -2;
    }
    if(len > CELLULAR_ICCID_LENGTH && sscanf(response, "*MICCID:%20s", buf) == 1) {

    } else {
        return -1;
    }
    return 0;
}

static const struct cellular_ops me3616_ops = {
    .reset = me3616_op_reset,
    .attach = me3616_attach,
    .detach = me3616_detach,
    .suspend = me3616_suspend,
    .resume = me3616_resume,

    .pdp_open = me3616_pdp_open,
    .pdp_close = me3616_pdp_close,
    .shutdown = me3616_shutdown,

    .imei = me3616_op_imei,
    .iccid = me3616_op_iccid,
    .imsi = cellular_op_imsi,
    .creg = me3616_op_creg,
    .cgatt = cellular_op_cgatt,
    .rssi = cellular_op_rssi,
    .cops = me3616_op_cops,
    .test = cellular_op_test,
    .command = cellular_op_command,
    .sms = cellular_op_sms,
    .cnum = cellular_op_cnum,
    .onum = cellular_op_onum,
    .socket_connect = me3616_socket_connect,
    .socket_send = me3616_socket_send,
    .socket_recv = me3616_socket_recv,
    .socket_waitack = me3616_socket_waitack,
    .socket_close = me3616_socket_close,
};

static struct cellular_me3616 cellular;

struct cellular *cellular_alloc(void)
{
    struct cellular_me3616 *modem = &cellular;
    memset(modem, 0, sizeof(*modem));

    modem->dev.ops = &me3616_ops;

    return (struct cellular *) modem;
}
/*
struct cellular *cellular_alloc_me3616(dev_comm_t *dev_obj)
{
    DBG_I("%s", __FUNCTION__);
    struct cellular_me3616 *modem = &cellular;

    memset(modem, 0, sizeof(*modem));
    modem->dev.ops = &me3616_ops;
    modem->dev_obj = dev_obj;
    return (struct cellular *) modem;
}
*/
void cellular_free(struct cellular *modem)
{
}

/* vim: set ts=4 sw=4 et: */
