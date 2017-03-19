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
 * @brief       Example using hdlc
 *
 * @author      Jason A. Tran <jasontra@usc.edu>
 * @author      Pradipta Ghosh <pradiptg@usc.edu>
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
#include "dispatcher.h"
#define DEBUG   1

#if (DEBUG) 
#define PRINTF(...) pc.printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif /* (DEBUG) & DEBUG_PRINT */

/* the only instance of pc -- debug statements in other files depend on it */
Serial pc(USBTX,USBRX,115200);
DigitalOut myled3(LED3); //to notify when a character was received on mbed

DigitalOut myled(LED1);
// Mail<msg_t, HDLC_MAILBOX_SIZE> dispatcher_mailbox;
Mail<msg_t, HDLC_MAILBOX_SIZE> thread1_mailbox;
Mail<msg_t, HDLC_MAILBOX_SIZE> main_thr_mailbox;

void _thread1()
{
/* Initial Direction of the Antenna*/

    char thread1_frame_no = 0;
    msg_t *msg;
    char send_data[HDLC_MAX_PKT_SIZE];
    char recv_data[HDLC_MAX_PKT_SIZE];
    hdlc_pkt_t pkt = { .data = send_data, .length = 0 };
    hdlc_pkt_t *recv_pkt;
    int exit = 0;
    static int port_no=register_thread(&thread1_mailbox);

    osEvent evt;

    while (true) 
    {

        myled3=!myled3;
        pkt.data[0] = thread1_frame_no;
        pkt.data[1] = port_no;
        for(int i = 2; i < HDLC_MAX_PKT_SIZE; i++) {
            pkt.data[i] = (char) ( rand() % 0x7E);
        }

        pkt.length = HDLC_MAX_PKT_SIZE;

        /* send pkt */
        hdlc_send_pkt(&pkt, &thread1_mailbox);
        PRINTF("thread1: sending pkt no %d \n", thread1_frame_no);

        while(1)
        {
            evt = thread1_mailbox.get();
            if (evt.status == osEventMail) 
            {
                msg = (msg_t*)evt.value.p;
                switch (msg->type)
                {
                    case HDLC_PKT_RDY:
                        recv_pkt = (hdlc_pkt_t *)msg->content.ptr;   
                        memcpy(recv_data, recv_pkt->data, recv_pkt->length);
                        thread1_mailbox.free(msg);
                        hdlc_pkt_release(recv_pkt);
                        PRINTF("thread1: received pkt %d; thr %d\n", recv_data[0], recv_data[1]);
                        break;
                    default:
                        thread1_mailbox.free(msg);
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

        thread1_frame_no++;
        Thread::wait(500);
    }
}

int main(void)
{
    myled=1;
    // PRINTF("In main");
   
    dispacher_init();

    msg_t *msg;
    char frame_no = 0;
    char send_data[HDLC_MAX_PKT_SIZE];
    char recv_data[HDLC_MAX_PKT_SIZE];
    hdlc_pkt_t pkt = { .data = send_data, .length = 0 };
    hdlc_pkt_t *recv_pkt;
    PRINTF("In main\n");
    static int port_no = register_thread(&main_thr_mailbox);
    // PT
    // Thread thread1(_thread1);
    PRINTF("main_thread: port no %d \n", port_no);
    Thread thr;
    thr.start(_thread1);

    int exit = 0;
    osEvent evt;
    while(1)
    {

        // Thread::wait(100);
        // PRINTF("In main\n");

        myled=!myled;
        pkt.data[0] = frame_no;
        // pkt.data[1] = frame_no;
        pkt.data[1] = port_no;

        for(int i = 2; i < HDLC_MAX_PKT_SIZE; i++) {
            pkt.data[i] = (char) ( rand() % 0x7E);
        }

        pkt.length = HDLC_MAX_PKT_SIZE;

        /* send pkt */
        hdlc_send_pkt(&pkt, &main_thr_mailbox);

        PRINTF("main_thread: sending pkt no %d \n", frame_no);

        while(1)
        {
            myled=!myled;
            evt = main_thr_mailbox.get();

            if (evt.status == osEventMail) 
            {
mail_check:     msg = (msg_t*)evt.value.p;

                switch (msg->type)
                {
                    case HDLC_PKT_RDY:
                        recv_pkt = (hdlc_pkt_t *)msg->content.ptr;   
                        memcpy(recv_data, recv_pkt->data, recv_pkt->length);
                        main_thr_mailbox.free(msg);
                        hdlc_pkt_release(recv_pkt);
                        printf("main_thr: received pkt %d ; thr %d\n", recv_data[0], recv_data[1]);
                        break;
                    default:
                        main_thr_mailbox.free(msg);
                        /* error */
                        //LED3_ON;
                        break;
                }
            }    
            if(exit) {
                // evt = main_thr_mailbox.get(100);
                // if (evt.status == osEventMail) 
                // {
                //     goto mail_check;
                // }
                exit = 0;
                break;
            }
        }

        frame_no++;
        Thread::wait(360);
    }
    /* should be never reached */
    PRINTF("Reached Exit");
}
