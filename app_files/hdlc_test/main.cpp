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
 * @file        main.cpp
 * @brief       Full-duplex hdlc test using a single thread (run on both sides).
 *
 * @author      Pradipta Ghosh <pradiptg@usc.edu>
 * 
 * In this test, the main thread and thread2 thread will contend for the same
 * UART line to communicate to another MCU also running a main and thread2 
 * thread. It seems as though stability deteriorates if the hdlc thread is given
 * a higher priority than the two application threads (RIOT's MAC layer priority
 * is well below the default priority for the main thread. Note that two threads
 * are equally contending for the UART line, one thread may starve the other to 
 * the point where the other thread will continue to retry. Increasing the msg 
 * queue size of hdlc's thread may also increase stability. Since this test can
 * easily stress the system, carefully picking the transmission rates (see below)
 * and tuning the RTRY_TIMEO_USEC and RETRANSMIT_TIMEO_USEC timeouts in hdlc.h
 * may lead to different stability results. The following is one known stable
 * set of values for running this test:
 *
 * -100ms interpacket intervals in xtimer_usleep() below
 * -RTRY_TIMEO_USEC = 100000
 * -RETRANSMIT_TIMEO_USEC 50000
 *
 */

#include "mbed.h"
#include "rtos.h"
#include "hdlc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "yahdlc.h"
#include "fcs16.h"
#include "uart_pkt.h"
#include "main-conf.h"

#define DEBUG   1

#if (DEBUG) 
#define PRINTF(...) pc.printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif /* (DEBUG) & DEBUG_PRINT */

/* the only instance of pc -- debug statements in other files depend on it */
Serial                          pc(USBTX,USBRX,115200);
DigitalOut                      myled3(LED3); //to notify when a character was received on mbed
DigitalOut                      myled(LED1);

Mail<msg_t, HDLC_MAILBOX_SIZE>  thread2_mailbox;
Mail<msg_t, HDLC_MAILBOX_SIZE>  main_thr_mailbox;


#define MAIN_THR_PORT   1234
#define THREAD2_PORT    5678
#define NULL_PKT_TYPE   0xFF 
#define PKT_FROM_MAIN_THR   0
#define PKT_FROM_THREAD2    1

void _thread2()
{
/* Initial Direction of the Antenna*/

    char thread2_frame_no = 0;
    msg_t *msg, *msg2;
    char send_data[HDLC_MAX_PKT_SIZE];
    char recv_data[HDLC_MAX_PKT_SIZE];
    hdlc_pkt_t pkt;
    pkt.data = send_data;
    pkt.length = 0;
    hdlc_buf_t *buf;
    uart_pkt_hdr_t recv_hdr;
    uart_pkt_hdr_t send_hdr = { THREAD2_PORT, THREAD2_PORT, PKT_FROM_THREAD2 };
    Mail<msg_t, HDLC_MAILBOX_SIZE> *hdlc_mailbox_ptr;
    hdlc_mailbox_ptr = get_hdlc_mailbox();
    int exit = 0;
    hdlc_entry_t thread2 = { NULL, THREAD2_PORT, &thread2_mailbox };
    hdlc_register(&thread2);

    osEvent evt;

    while (1) 
    {

        myled3 =! myled3;
        uart_pkt_insert_hdr(pkt.data, HDLC_MAX_PKT_SIZE, &send_hdr); 

        pkt.data[UART_PKT_DATA_FIELD] = thread2_frame_no;
        for(int i = UART_PKT_DATA_FIELD + 1; i < HDLC_MAX_PKT_SIZE; i++) {
            pkt.data[i] = (char) ( rand() % 0x7E);
        }

        pkt.length = HDLC_MAX_PKT_SIZE;

        /* send pkt */
        msg = hdlc_mailbox_ptr->alloc();
        msg->type = HDLC_MSG_SND;
        msg->content.ptr = &pkt;
        msg->sender_pid = osThreadGetId();
        msg->source_mailbox = &thread2_mailbox;
        hdlc_mailbox_ptr->put(msg);
        PRINTF("thread2: sending pkt no %d \n", thread2_frame_no);

        while(1)
        {
            evt = thread2_mailbox.get();
            if (evt.status == osEventMail) 
            {
                msg = (msg_t*)evt.value.p;
                switch (msg->type)
                {
                    case HDLC_RESP_SND_SUCC:
                        PRINTF("thread2: sent frame_no %d!\n", thread2_frame_no);
                        exit = 1;
                        thread2_mailbox.free(msg);
                        break;    
                    case HDLC_RESP_RETRY_W_TIMEO:
                        Thread::wait(msg->content.value/1000);
                        PRINTF("main_thr: retry frame_no %d \n", thread2_frame_no);
                        msg2 = hdlc_mailbox_ptr->alloc();
                        if (msg2 == NULL) {
                            Thread::wait(50);
                            while (msg2 == NULL)
                            {
                                msg2 = thread2_mailbox.alloc();  
                                Thread::wait(10);
                            }
                            msg2->type = HDLC_RESP_RETRY_W_TIMEO;
                            msg2->content.value = (uint32_t) RTRY_TIMEO_USEC;
                            msg2->sender_pid = osThreadGetId();
                            msg2->source_mailbox = &thread2_mailbox;
                            thread2_mailbox.put(msg2);
                            thread2_mailbox.free(msg);
                            break;
                        }
                        msg2->type = HDLC_MSG_SND;
                        msg2->content.ptr = &pkt;
                        msg2->sender_pid = osThreadGetId();
                        msg2->source_mailbox = &thread2_mailbox;
                        hdlc_mailbox_ptr->put(msg2);
                        thread2_mailbox.free(msg);
                        break;
                    case HDLC_PKT_RDY:
                        buf = (hdlc_buf_t *)msg->content.ptr;   
                        uart_pkt_parse_hdr(&recv_hdr, buf->data, buf->length);
                        if (recv_hdr.pkt_type == PKT_FROM_THREAD2) {
                            memcpy(recv_data, buf->data, buf->length);
                            printf("thread2: received pkt %d ; dst_port %d\n", 
                            recv_data[UART_PKT_DATA_FIELD], recv_hdr.dst_port);
                        }
                        thread2_mailbox.free(msg);
                        hdlc_pkt_release(buf);
                        break;
                    default:
                        thread2_mailbox.free(msg);
                        /* error */
                        break;
                }
            }    
            if(exit) {
                exit = 0;
                break;
            }
        }

        thread2_frame_no++;
        Thread::wait(100);
    }
}


int main(void)
{
    myled = 1;
    Mail<msg_t, HDLC_MAILBOX_SIZE> *hdlc_mailbox_ptr;
    hdlc_mailbox_ptr = hdlc_init(osPriorityRealtime);
   
    msg_t *msg, *msg2;
    char frame_no = 0;
    char send_data[HDLC_MAX_PKT_SIZE];
    char recv_data[HDLC_MAX_PKT_SIZE];
    hdlc_pkt_t pkt;
    pkt.data = send_data;
    pkt.length = 0;
    hdlc_buf_t *buf;
    uart_pkt_hdr_t recv_hdr;
    uart_pkt_hdr_t send_hdr = { MAIN_THR_PORT, MAIN_THR_PORT, PKT_FROM_MAIN_THR };
    PRINTF("In main\n");
    hdlc_entry_t main_thr = { NULL, MAIN_THR_PORT, &main_thr_mailbox };
    hdlc_register(&main_thr);

    Thread thr;
    thr.start(_thread2);

    int exit = 0;
    osEvent evt;
    while(1)
    {
        myled=!myled;
        uart_pkt_insert_hdr(pkt.data, HDLC_MAX_PKT_SIZE, &send_hdr); 

        pkt.data[UART_PKT_DATA_FIELD] = frame_no;
        for(int i = UART_PKT_DATA_FIELD + 1; i < HDLC_MAX_PKT_SIZE; i++) {
            pkt.data[i] = (char) ( rand() % 0x7E);
        }

        pkt.length = HDLC_MAX_PKT_SIZE;

        /* send pkt */
        msg = hdlc_mailbox_ptr->alloc();
        msg->type = HDLC_MSG_SND;
        msg->content.ptr = &pkt;
        msg->sender_pid = osThreadGetId();
        msg->source_mailbox = &main_thr_mailbox;
        hdlc_mailbox_ptr->put(msg);

        PRINTF("main_thread: sending pkt no %d \n", frame_no);

        while(1)
        {
            myled=!myled;
            evt = main_thr_mailbox.get();

            if (evt.status == osEventMail) 
            {
                msg = (msg_t*)evt.value.p;

                switch (msg->type)
                {
                    case HDLC_RESP_SND_SUCC:
                        PRINTF("main_thread: sent frame_no %d!\n", frame_no);
                        exit = 1;
                        main_thr_mailbox.free(msg);
                        break;
                    case HDLC_RESP_RETRY_W_TIMEO:
                        Thread::wait(msg->content.value/1000);
                        PRINTF("main_thr: retry frame_no %d \n", frame_no);
                        msg2 = hdlc_mailbox_ptr->alloc();
                        if (msg2 == NULL) {
                            // Thread::wait(50);
                            while(msg2==NULL)
                            {
                                msg2 = main_thr_mailbox.alloc();  
                                Thread::wait(10);
                            }
                            msg2->type = HDLC_RESP_RETRY_W_TIMEO;
                            msg2->content.value = (uint32_t) RTRY_TIMEO_USEC;
                            msg2->sender_pid = osThreadGetId();
                            msg2->source_mailbox = &main_thr_mailbox;
                            main_thr_mailbox.put(msg2);
                            main_thr_mailbox.free(msg);
                            break;
                        }
                        msg2->type = HDLC_MSG_SND;
                        msg2->content.ptr = &pkt;
                        msg2->sender_pid = osThreadGetId();
                        msg2->source_mailbox = &main_thr_mailbox;
                        hdlc_mailbox_ptr->put(msg2);
                        main_thr_mailbox.free(msg);
                        break;
                    case HDLC_PKT_RDY:
                        buf = (hdlc_buf_t *)msg->content.ptr;   
                        uart_pkt_parse_hdr(&recv_hdr, buf->data, buf->length);
                        if (recv_hdr.pkt_type == PKT_FROM_MAIN_THR) {
                            memcpy(recv_data, buf->data, buf->length);
                            printf("main_thr: received pkt %d ; dst_port %d\n", 
                            recv_data[UART_PKT_DATA_FIELD], recv_hdr.dst_port);
                        }
                        main_thr_mailbox.free(msg);
                        hdlc_pkt_release(buf);
                        break;
                    default:
                        main_thr_mailbox.free(msg);
                        /* error */
                        //LED3_ON;
                        break;
                }
            }    
            if(exit) {
                exit = 0;
                break;
            }
        }

        frame_no++;
        Thread::wait(100);
    }
    PRINTF("Reached Exit");
    /* should be never reached */
    return 0;
}
