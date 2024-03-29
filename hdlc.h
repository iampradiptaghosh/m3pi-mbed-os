/**
 * Copyright (c) 2016, Autonomous Networks Research Group. All rights reserved.
 * Developed by:
 * Autonomous Networks Research Group (ANRG)
 * University of Southern California
 * http://anrg.usc.edu/
 *
 * Contributors:
 * Jason A. Tran
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy 
 * of this software and associated documentation files (the "Software"), to deal
 * with the Software without restriction, including without limitation the 
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or 
 * sell copies of the Software, and to permit persons to whom the Software is 
 * furnished to do so, subject to the following conditions:
 * - Redistributions of source code must retain the above copyright notice, this
 *     list of conditions and the following disclaimers.
 * - Redistributions in binary form must reproduce the above copyright notice, 
 *     this list of conditions and the following disclaimers in the 
 *     documentation and/or other materials provided with the distribution.
 * - Neither the names of Autonomous Networks Research Group, nor University of 
 *     Southern California, nor the names of its contributors may be used to 
 *     endorse or promote products derived from this Software without specific 
 *     prior written permission.
 * - A citation to the Autonomous Networks Research Group must be included in 
 *     any publications benefiting from the use of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH 
 * THE SOFTWARE.
 */

/**
 * @ingroup     examples
 * @{
 *
 * @file
 * @brief       Full duplex hdlc implementation.
 *
 * This implementation leverages yahdlc, an open source library. The current 
 * implementation is stop & wait.
 *
 * @author      Jason A. Tran <jasontra@usc.edu>
 *
 * @}
 */

#ifndef HDLC_H_
#define HDLC_H_

#include "yahdlc.h"
#include "rtos.h"
#include "mbed.h"
#include "uart_pkt.h"

#define RTRY_TIMEO_USEC         100000
#define RETRANSMIT_TIMEO_USEC   50000
#define HDLC_MAX_PKT_SIZE       64
#define HDLC_MAILBOX_SIZE       100

typedef struct {
    yahdlc_control_t control;
    char *data;
    unsigned int length;
} hdlc_buf_t;

typedef struct {
    osThreadId sender_pid;    
    void *source_mailbox;
    uint16_t type;              /**< Type field. */
    union {
        void *ptr;              /**< Pointer content field. */
        uint32_t value;         /**< Value content field. */
    } content;                  /**< Content of the message. */
} msg_t;

/* struct for other threads to pass to hdlc thread via IPC */
typedef struct {
    char *data;
    unsigned int length;
} hdlc_pkt_t;

/* HDLC thread messages */
enum {
    HDLC_MSG_REG_DISPATCHER,
    HDLC_MSG_RECV,
    HDLC_MSG_SND,
    HDLC_MSG_RESEND,
    HDLC_MSG_SND_ACK,
    HDLC_RESP_RETRY_W_TIMEO,
    HDLC_RESP_SND_SUCC,
    HDLC_PKT_RDY
};
typedef struct hdlc_entry {
    struct hdlc_entry *next;
    uint16_t port;
    Mail<msg_t, HDLC_MAILBOX_SIZE> *mailbox;
} hdlc_entry_t;


int hdlc_pkt_release(hdlc_buf_t *buf);
Mail<msg_t, HDLC_MAILBOX_SIZE> *hdlc_init(osPriority priority);
Mail<msg_t, HDLC_MAILBOX_SIZE> *get_hdlc_mailbox();
void buffer_cpy(hdlc_buf_t* dst, hdlc_buf_t* src);
void hdlc_register(hdlc_entry_t *entry);
void hdlc_unregister(hdlc_entry_t *entry);
int hdlc_send_command(hdlc_pkt_t *pkt, Mail<msg_t, HDLC_MAILBOX_SIZE> *sender_mailbox, riot_to_mbed_t reply);

#endif /* HDLC_H_ */