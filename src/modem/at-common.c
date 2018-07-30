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

#include "at-common.h"


#define PDP_RETRY_THRESHOLD_INITIAL     3
#define PDP_RETRY_THRESHOLD_MULTIPLIER  2

/*
 * PDP management logic.
 *
 * 1. PDP contexts cannot be activated too often. Common GSM etiquette requires
 *    that some kind of backoff strategy should be implemented to avoid hammering
 *    the network with requests. Here we use a simple exponential backoff which
 *    is reset every time a connection succeeds.
 *
 * 2. Contexts can get stuck sometimes; the modem reports active context but no
 *    data can be transmitted. Telit modems are especially prone to this if
 *    AT+CGDCONT is invoked while the context is active. Our logic should handle
 *    this after a few connection failures.
 */

int cellular_pdp_request(struct cellular *modem)
{
    if (modem->pdp_failures >= modem->pdp_threshold) {
        /* Possibly stuck PDP context; close it. */
        modem->ops->pdp_close(modem);
        /* Perform exponential backoff. */
        modem->pdp_threshold *= (1+PDP_RETRY_THRESHOLD_MULTIPLIER);
    }

    if (modem->ops->pdp_open(modem, modem->apn) != 0) {
        cellular_pdp_failure(modem);
        return -1;
    }

    return 0;
}

void cellular_pdp_success(struct cellular *modem)
{
    modem->pdp_failures = 0;
    modem->pdp_threshold = PDP_RETRY_THRESHOLD_INITIAL;
}

void cellular_pdp_failure(struct cellular *modem)
{
    modem->pdp_failures++;
}


int cellular_op_imei(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CGSN");
    if(response == NULL) {
        return -2;
    }

    if(len > CELLULAR_IMEI_LENGTH && sscanf(response, "+CGSN:%16s", buf) == 1) {

    } else if(len > CELLULAR_IMEI_LENGTH && strlen(response) == CELLULAR_IMEI_LENGTH) {
        strncpy(buf, response, len);
    } else {
        return -1;
    }

    return 0;
}

int cellular_op_iccid(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    const char *response = at_command(modem->at, "AT+CCID");
    if(response == NULL) {
        return -2;
    }

    if(len > CELLULAR_ICCID_LENGTH && sscanf(response, "+CCID:%20s", buf) == 1) {

    } else if(len > CELLULAR_ICCID_LENGTH && strlen(response) == CELLULAR_ICCID_LENGTH) {
        strncpy(buf, response, len);
    } else {
        return -1;
    }

    return 0;
}

int cellular_op_imsi(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CIMI");
    if(response == NULL) {
        return -2;
    }
    if(strlen(response) == CELLULAR_IMSI_LENGTH && len > CELLULAR_IMSI_LENGTH) {
        strncpy(buf, response, len);
    } else {
        return -1;
    }

    return 0;
}

int cellular_op_creg(struct cellular *modem)
{
    int creg;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CREG?");
    at_simple_scanf(response, "+CREG: %*d,%d", &creg);

    return creg;
}

int cellular_op_cgatt(struct cellular *modem)
{
    int cgatt;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CGATT?");
    at_simple_scanf(response, "+CGATT: %d", &cgatt);

    return cgatt;
}

int cellular_op_rssi(struct cellular *modem)
{
    int rssi, ber;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CSQ");
    at_simple_scanf(response, "+CSQ: %d,%d", &rssi, &ber);

    return rssi | (ber << 16);
}

int cellular_op_cops(struct cellular *modem)
{
    int ops = -1;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+COPS=3,2");
    const char *response = at_command(modem->at, "AT+COPS?");
    at_simple_scanf(response, "+COPS: %*d,%*d,\"%d\"", &ops);

    return ops;
}

int cellular_op_test(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT");

    return 0;
}

const char* cellular_op_command(struct cellular *modem, char *cmd, int timeout)
{
    at_set_timeout(modem->at, timeout);
    return at_command(modem->at, cmd);
}

int cellular_op_sms(struct cellular *modem, char* num, char* msg, size_t len)
{
    // Check SMS length
    if(len > 160) {
      return -1;
    }
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    // // SMS Center
    // at_config_simple(modem->at, "CSCA", "\"+8613010811500\"", 30);
    // Text mode
    at_command_simple(modem->at, "AT+CMGF=1");
    // SMS command
    at_expect_dataprompt(modem->at, "> ");
    at_command_simple(modem->at, "AT+CMGS=\"%s\"", num);
    // SMS data
    at_set_timeout(modem->at, AT_TIMEOUT_SMS);
    at_send_raw(modem->at, msg, len);
    char c = 0x1A;
    const char* response = at_command_raw(modem->at, &c, sizeof(c));
    if(response == NULL) {
        return -2;
    }
    if(strncmp(response, "+CMGS:", strlen("+CMGS:"))) {
        return -1;
    }

    return 0;
}

int cellular_op_cnum(struct cellular *modem, char *buf, size_t len)
{
    memset(buf, 0, len);

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CNUM");
    if(response == NULL) {
        return -2;
    }
    if(*response != '\0') {
        at_simple_scanf(response, "+CNUM: %*[^,],\"%[^\"]\"%*s", buf);
    }

    return 0;

}

int cellular_op_onum(struct cellular *modem, char *num)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+CPBS=\"ON\"");
    at_command_simple(modem->at, "AT+CPBW=1,\"%s\"", num);

    return 0;
}

//int cellular_op_clock_gettime(struct cellular *modem, struct timespec *ts)
//{
//    struct tm tm;

//    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
//    const char *response = at_command(modem->at, "AT+CCLK?");
//    memset(&tm, 0, sizeof(struct tm));
//    at_simple_scanf(response, "+CCLK: \"%d/%d/%d,%d:%d:%d%*d\"",
//            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
//            &tm.tm_hour, &tm.tm_min, &tm.tm_sec);

//    /* Most modems report some starting date way in the past when they have
//     * no date/time estimation. */
//    if (tm.tm_year < 14) {
//        return 1;
//    }

//    /* Adjust values and perform conversion. */
//    tm.tm_year += 2000 - 1900;
//    tm.tm_mon -= 1;
//    time_t unix_time = timegm(&tm);
//    if (unix_time == -1) {
//        return -1;
//    }

//    /* All good. Return the result. */
//    ts->tv_sec = unix_time;
//    ts->tv_nsec = 0;
//    return 0;
//}

//int cellular_op_clock_settime(struct cellular *modem, const struct timespec *ts)
//{
//    /* Convert time_t to broken-down UTC time. */
//    struct tm tm;
//    gmtime_r(&ts->tv_sec, &tm);

//    /* Adjust values to match 3GPP TS 27.007. */
//    tm.tm_year += 1900 - 2000;
//    tm.tm_mon += 1;

//    /* Set the time. */
//    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
//    at_command_simple(modem->at, "AT+CCLK=\"%02d/%02d/%02d,%02d:%02d:%02d+00\"",
//            tm.tm_year, tm.tm_mon, tm.tm_mday,
//            tm.tm_hour, tm.tm_min, tm.tm_sec);

//    return 0;
//}

/* vim: set ts=4 sw=4 et: */
