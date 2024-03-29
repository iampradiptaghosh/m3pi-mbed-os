/**
 * Copyright (c) 2016, Autonomous Networks Research Group. All rights reserved.
 * Developed by:
 * Autonomous Networks Research Group (ANRG)
 * University of Southern California
 * http://anrg.usc.edu/
 *
 * Contributors:
 * Jason A. Tran
 * Pradipta Ghosh
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
 * @file        hdlc.cpp
 * @brief       Full duplex hdlc implementation for mbed-os.
 *
 * This implementation leverages yahdlc, an open source library. The current 
 * implementation is stop & wait.
 *
 * @author      Pradipta Ghosh <pradiptg@usc.edu>
 * @author      Jason A. Tran <jasontra@usc.edu>
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include "mbed.h"
#include "platform/CircularBuffer.h"
#include "hdlc.h"
#include "rtos.h"
#include "uart_pkt.h"
#include "utlist.h"

#define DEBUG 0

#if (DEBUG) 
    #define PRINTF(...) pc.printf(__VA_ARGS__)
    extern Serial pc;
#else
    #define PRINTF(...)
#endif /* (DEBUG) */

#define UART_BUFSIZE            (512U)

DigitalOut led2(LED2);

static osThreadId sender_pid;

static unsigned char HDLC_STACK[DEFAULT_STACK_SIZE];

Thread hdlc(osPriorityNormal, 
    (uint32_t) DEFAULT_STACK_SIZE, (unsigned char *)HDLC_STACK); 

Serial uart2(p28,p27, 115200);


static hdlc_entry_t *hdlc_reg;



void hdlc_register(hdlc_entry_t *entry)
{
    LL_PREPEND(hdlc_reg, entry);
}

void hdlc_unregister(hdlc_entry_t *entry)
{
    LL_DELETE(hdlc_reg, entry);
}


static Mail<msg_t, HDLC_MAILBOX_SIZE> *sender_mailbox_ptr;
Mail<msg_t, HDLC_MAILBOX_SIZE> hdlc_mailbox;
Semaphore   recv_buf_mutex(1);
Semaphore   recv_buf_cpy_mutex(1); 
// Mutex recv_buf_mutex;
Timer       global_time;
Timer       uart_lock_time;


CircularBuffer<char, UART_BUFSIZE> circ_buf;

void write_hdlc(uint8_t *,int);

static char hdlc_recv_data[HDLC_MAX_PKT_SIZE];
static char hdlc_recv_data_cpy[HDLC_MAX_PKT_SIZE];

static char hdlc_send_frame[2 * (HDLC_MAX_PKT_SIZE + 2 + 2 + 2)];
static char hdlc_ack_frame[2 + 2 + 2 + 2];


static hdlc_buf_t recv_buf; // the initialization is done in the hdlc init function
static hdlc_buf_t recv_buf_cpy; // the initialization is done in the hdlc init function
static hdlc_buf_t send_buf;
static hdlc_buf_t ack_buf;



/* uart access control lock */
static bool uart_lock = 0;

static void rx_cb(void)//(void *arg, uint8_t data)
{
    unsigned char data;

    while (uart2.readable()) {
        data = uart2.getc();     // Get an character from the Serial
        circ_buf.push(data);     // Put to the ring/circular buffer

        if (data == YAHDLC_FLAG_SEQUENCE) {
            
            // wakeup hdlc thread 
            msg_t *msg = hdlc_mailbox.alloc();
            if(msg == NULL)
            {
                  PRINTF("hdlc: rx_cb no more space available on mailbox\n");
                  return;
            }
            msg->sender_pid = osThreadGetId();
            msg->type = HDLC_MSG_RECV;
            msg->source_mailbox = &hdlc_mailbox;
            hdlc_mailbox.put(msg); 
        }
    }

}

static void _hdlc_receive(unsigned int *recv_seq_no, unsigned int *send_seq_no)
{
    msg_t *msg, *ack_msg;
    int ret;
    char c;
    uart_pkt_hdr_t hdr;
    hdlc_entry_t *entry;
    
    while(1) {
        if (!circ_buf.pop(c)) {
            return;
        }
        recv_buf_mutex.wait();
        ret = yahdlc_get_data(&recv_buf.control, &c, 1, recv_buf.data, 
                                &recv_buf.length);
        recv_buf_mutex.release();

        if (ret == -ENOMSG) {
            continue; //full packet not yet parsed
        }

        if (ret == -EIO) {
            PRINTF("FCS ERROR OR INVALID FRAME!\n");
            recv_buf.control.frame = (yahdlc_frame_t)0;
            recv_buf.control.seq_no = 0;
            return;
        }

        if (recv_buf.length > 0 && 
            (recv_buf.control.seq_no == *recv_seq_no % 8 ||
            recv_buf.control.seq_no == (*recv_seq_no - 1) % 8)) {
            /* valid data frame received */
            PRINTF("hdlc: received data frame w/ seq_no: %d\n", recv_buf.control.seq_no);

            /* always send ack. This maybe bogging down the mailbox */
            ack_msg = hdlc_mailbox.alloc();
            if (ack_msg == NULL)
            {
              PRINTF("hdlc: ACK no more space available on mailbox\n");
              return;
            }
            ack_msg->sender_pid = osThreadGetId();
            ack_msg->type = HDLC_MSG_SND_ACK;
            ack_msg->content.value = recv_buf.control.seq_no;
            ack_msg->source_mailbox = &hdlc_mailbox;
            hdlc_mailbox.put(ack_msg); 
            PRINTF("hdlc: received data frame w/ seq_no: %d\n", recv_buf.control.seq_no);

            /* pass on packet to thread */
            if (recv_buf.control.seq_no == (*recv_seq_no % 8)){
                /* lock pkt until thread makes a copy and unlocks */
                PRINTF("hdlc: received data frame w/ seq_no: %d\n", recv_buf.control.seq_no);
                fflush(stdout);
                recv_buf_cpy_mutex.wait();
                recv_buf_mutex.wait();
                buffer_cpy(&recv_buf_cpy,&recv_buf);
                recv_buf_mutex.release();
                uart_pkt_parse_hdr(&hdr, recv_buf_cpy.data, recv_buf_cpy.length);
                LL_SEARCH_SCALAR(hdlc_reg, entry, port, hdr.dst_port);
                PRINTF("hdlc: received packet for port %d\n", hdr.dst_port);

                (*recv_seq_no)++;

                if (entry) {
                    msg = entry->mailbox->alloc();
                    if( msg == NULL)
                        break;
                    msg->sender_pid = osThreadGetId();
                    msg->type = HDLC_PKT_RDY;
                    msg->content.ptr = &recv_buf_cpy;
                    msg->source_mailbox = &hdlc_mailbox;
                    entry->mailbox->put(msg);
                } else {
                    PRINTF("hdlc: no thread subscribed to port!\n");
                    hdlc_pkt_release(&recv_buf_cpy);
                }
            }

            recv_buf.control.frame = (yahdlc_frame_t)0;
            recv_buf.control.seq_no =  0;
            return;

        } else if (recv_buf.length == 0 &&
                    (recv_buf.control.frame == YAHDLC_FRAME_ACK ||
                     recv_buf.control.frame == YAHDLC_FRAME_NACK)) {
            PRINTF("hdlc: received ACK/NACK w/ seq_no: %d\n", recv_buf.control.seq_no);

            if(recv_buf.control.seq_no == (*send_seq_no % 8)) {
                msg=sender_mailbox_ptr->alloc();
                if (msg == NULL)
                    return;

                uart_lock = 0;
                (*send_seq_no)++;
                msg->sender_pid = osThreadGetId();
                msg->type = HDLC_RESP_SND_SUCC;
                msg->content.value = (uint32_t) 0;
                msg->source_mailbox = &hdlc_mailbox;
                sender_mailbox_ptr->put(msg);
                PRINTF("hdlc: sender_pid is %d\n", sender_pid);
            }
                                
            recv_buf.control.frame = (yahdlc_frame_t)0;
            recv_buf.control.seq_no = 0;
            return;
        }
    }
}

static void _hdlc()
{
    msg_t *msg, *reply;
    unsigned int recv_seq_no = 0;
    unsigned int send_seq_no = 0;
    osEvent evt;

    while(1) {

        led2=!led2;
        // hdlc_ready=1;
        if(uart_lock) {
            // int uart_ll=(int)uart_lock_time.read_us();
            // if(uart_ll>10*RETRANSMIT_TIMEO_USEC)
            // {
            //     PRINTF("hdlc: UART is locked for %d us_seconds\n",uart_ll);
            //     uart_lock=0;
            //     goto getmail;
            // }    
            int timeout = (int)RETRANSMIT_TIMEO_USEC - (int) global_time.read_us();
            if(timeout < 0) {
                // PRINTF("hdlc: inside timeout negative\n");
                /* send message to self to resend msg */
                msg = hdlc_mailbox.alloc();
                if(msg == NULL) {
                    PRINTF("hdlc: no more space available on mailbox");
                    /* service mailbox and try again */
                    evt = hdlc_mailbox.get();
                }
                else {
                    msg->sender_pid = osThreadGetId();
                    msg->type = HDLC_MSG_RESEND;
                    msg->source_mailbox = &hdlc_mailbox;
                    hdlc_mailbox.put(msg); 
                    evt = hdlc_mailbox.get();
                }

            } else {
                evt = hdlc_mailbox.get(timeout / 1000);
                if(evt.status == osEventTimeout){
                    continue;
                }
            }
        } else {
            // PRINTF("hdlc: waiting for mail\n");
  getmail:          evt = hdlc_mailbox.get();
        }
       
        if (evt.status == osEventMail) 
        {          
            msg = (msg_t*)evt.value.p;

            switch (msg->type) {
                case HDLC_MSG_RECV:
                    PRINTF("hdlc: receiving msg...\n");
                    _hdlc_receive(&recv_seq_no, &send_seq_no);
                    hdlc_mailbox.free(msg);
                    break;
                case HDLC_MSG_SND:
                    PRINTF("hdlc: request to send received from pid %d\n", msg->sender_pid);
                    if (uart_lock) {
                        /* ask thread to try again in x usec */
                        PRINTF("hdlc: uart locked, telling thr to retry\n");
                        reply=((Mail<msg_t, HDLC_MAILBOX_SIZE>*)msg->source_mailbox)->alloc();
                        if(reply == NULL) {
                            PRINTF("hdlc: no space in thread mailbox. ERROR!!\n");
                        }
                        else {
                            reply->type = HDLC_RESP_RETRY_W_TIMEO;
                            reply->content.value = (uint32_t) RTRY_TIMEO_USEC;
                            reply->sender_pid = osThreadGetId();
                            ((Mail<msg_t, HDLC_MAILBOX_SIZE>*)msg->source_mailbox)->put(reply);
                        }
                    } else {
                        uart_lock = 1;
                        sender_pid = msg->sender_pid;
                        PRINTF("hdlc: sender_pid set to %d\n", sender_pid);
                        send_buf.control.frame = YAHDLC_FRAME_DATA;
                        send_buf.control.seq_no = send_seq_no % 8; 
                        hdlc_pkt_t *pkt = (hdlc_pkt_t*)msg->content.ptr;
                        yahdlc_frame_data(&(send_buf.control), pkt->data, 
                                pkt->length, send_buf.data, &send_buf.length);

                        sender_mailbox_ptr=(Mail<msg_t, HDLC_MAILBOX_SIZE>*)msg->source_mailbox;
                        PRINTF("hdlc: sending frame seq no %d, len %d\n", 
                            send_buf.control.seq_no,send_buf.length);

                        write_hdlc((uint8_t *)send_buf.data, send_buf.length);
                        global_time.reset();
                        uart_lock_time.reset();
                    }  
                    hdlc_mailbox.free(msg); 
                    break;
                case HDLC_MSG_SND_ACK:
                    /* send ACK */
                    ack_buf.control.frame = YAHDLC_FRAME_ACK;
                    ack_buf.control.seq_no = msg->content.value;
                    yahdlc_frame_data(&(ack_buf.control), NULL, 0, ack_buf.data, 
                        &(ack_buf.length));    
                    PRINTF("hdlc: sending ack w/ seq no %d, len %d\n", 
                        ack_buf.control.seq_no,ack_buf.length);
                    write_hdlc((uint8_t *)ack_buf.data, ack_buf.length);
                    // uart2.write((uint8_t *)ack_buf.data, ack_buf.length,0,0);   
                    hdlc_mailbox.free(msg);
                    break;
                case HDLC_MSG_RESEND:
                    PRINTF("hdlc: Resending frame w/ seq no %d (on send_seq_no %d)\n", 
                        send_buf.control.seq_no, send_seq_no);
                    write_hdlc((uint8_t *)send_buf.data, send_buf.length);
                    // uart2.write((uint8_t *)send_buf.data, send_buf.length,0,0);
                    global_time.reset();
                    hdlc_mailbox.free(msg); 
                    break;
                default:
                    PRINTF("INVALID HDLC MSG\n");
                    //LED3_ON;
                    hdlc_mailbox.free(msg); 
                    break;
            }
        }
    }

    /* this should never be reached */
}

/**
 * @brief Send @p pkt as an hdlc command packet over serial. This function blocks.
 * @param  pkt            Packet to be sent.
 * @param  sender_mailbox Pointer to sender's mailbox.
 * @return                [description]
 */
int hdlc_send_command(hdlc_pkt_t *pkt, Mail<msg_t, HDLC_MAILBOX_SIZE> *sender_mailbox, 
                                                    riot_to_mbed_t reply)
{
// /* TODO: should the second input not be a void pointer? */
// /* TODO: limit the number of retries */
    msg_t *msg, *msg2;
    hdlc_buf_t *buf;
    char recv_data_local[HDLC_MAX_PKT_SIZE];
    uart_pkt_hdr_t hdr;
    // Timer       command_send_time;
    /* send pkt */
    msg = hdlc_mailbox.alloc();
    msg->type = HDLC_MSG_SND;
    msg->content.ptr = pkt;
    msg->sender_pid = osThreadGetId();
    msg->source_mailbox = sender_mailbox;
    hdlc_mailbox.put(msg);
    PRINTF("hdlc: in hdlc send command\n");
    while(1)
    {
        osEvent evt = sender_mailbox->get(2000);
        if(evt.status == osEventTimeout){
                    return 0;
        }
            // if(uart_ll>10*RETRANSMIT_TIMEO_USEC)
        if (evt.status == osEventMail) 
        {
            msg = (msg_t*)evt.value.p;

            switch (msg->type)
            {
                case HDLC_RESP_SND_SUCC:
                    PRINTF("sent frame_no!\n");
                    sender_mailbox->free(msg);
                    break;
                case HDLC_RESP_RETRY_W_TIMEO:
                    Thread::wait(msg->content.value/1000);
                    msg2 = hdlc_mailbox.alloc();
                    if (msg2 == NULL) {
                        while(msg2 == NULL)
                        {
                            msg2 = sender_mailbox->alloc();  
                            Thread::wait(10);
                        }
                        /* TODO: this doesn't seem right... */
                        msg2->type = HDLC_RESP_RETRY_W_TIMEO;
                        msg2->content.value = (uint32_t) RTRY_TIMEO_USEC;
                        msg2->sender_pid = osThreadGetId();
                        msg2->source_mailbox = sender_mailbox;
                        sender_mailbox->put(msg2);
                        sender_mailbox->free(msg);
                        break;
                    }
                    msg2->type = HDLC_MSG_SND;
                    msg2->content.ptr = pkt;
                    msg2->sender_pid = osThreadGetId();
                    msg2->source_mailbox = sender_mailbox;
                    hdlc_mailbox.put(msg2);
                    sender_mailbox->free(msg);
                    break;
                case HDLC_PKT_RDY: 
                    buf = (hdlc_buf_t *)msg->content.ptr;   
                    uart_pkt_parse_hdr(&hdr, (void *)buf->data, (size_t) (buf->length));
                    hdlc_pkt_release(buf);
                    sender_mailbox->free(msg);
                    PRINTF("hdlc_send_command: received pkt\n");
                    if (hdr.pkt_type == reply)
                        return 1;
                    else
                        return 0;
                    break;
                default:
                    sender_mailbox->free(msg);
                    /* error */
                    break;
            }
        }    
    }
}

int hdlc_pkt_release(hdlc_buf_t *buf) 
{
    if(recv_buf_cpy_mutex.wait(0))
    {
        PRINTF("hdlc: Packet not locked. Might be empty!\n");
        recv_buf_cpy_mutex.release();
        return -1;
    }
    else
    {
        buf->control.frame = (yahdlc_frame_t)0;
        buf->control.seq_no = 0;
        PRINTF("hdlc: relesed lock!\n");

        recv_buf_cpy_mutex.release();
        return 0;
    }

}


Mail<msg_t, HDLC_MAILBOX_SIZE> *get_hdlc_mailbox()
{
    return &hdlc_mailbox;
}

void write_hdlc(uint8_t *ptr,int len)
{
    int count = 0;

    while ( count < len ) {
        if (uart2.writeable()) {
            uart2.putc(ptr[count]);   
            count++;
        }
    }
}
void buffer_cpy(hdlc_buf_t* dst, hdlc_buf_t* src)
{
    memcpy(dst->data,src->data,HDLC_MAX_PKT_SIZE);
    memcpy(&dst->control,&src->control,sizeof(yahdlc_control_t));
    dst->length=src->length;
}

Mail<msg_t, HDLC_MAILBOX_SIZE> *hdlc_init(osPriority priority) 
{
    led2 = 1;
    recv_buf.data = hdlc_recv_data;
    recv_buf_cpy.data= hdlc_recv_data_cpy;
    send_buf.data = hdlc_send_frame;
    ack_buf.data = hdlc_ack_frame;
    global_time.start();
    uart_lock_time.start();
    uart2.attach(&rx_cb,Serial::RxIrq);
    hdlc.set_priority(priority);
    hdlc.start(_hdlc);
    PRINTF("hdlc: thread  id %d\n",hdlc.gettid());
    return &hdlc_mailbox;
}

