#include "rssi.h"

Mail<int, 1> rssi_mailbox;
Mail<int, 1> range_mailbox;

/**
 * @brief clears the rssi buffer/queue
 */
void clear_rssi()
{
    osEvent evt = rssi_mailbox.get(1); // Clear the queue 
    if(evt.status != osEventMail) 
        return;
    int *message = (int*)evt.value.p;
    rssi_mailbox.free(message);
}

/**
 * @brief returns the current rssi value
 * @param millisec      wait time in milisecond
 * @return              [the current rssi]
 */

int get_rssi(uint32_t millisec)
{ 
    // printf("In get rssi\n");
    osEvent evt = rssi_mailbox.get(millisec); // Clear the queue 
    if(evt.status != osEventMail) 
        return -1;

    int *message = (int*)evt.value.p;
    int rssi_data= *message; 
    rssi_mailbox.free(message);

    return rssi_data;
}

/**
 * @brief puts the recent rssi value in the buffer
 * @param rssi          rssi_value
 * @return              [ ]
 */
void put_rssi(int rssi)
{ 
    clear_rssi();
    // printf("In put rssi\n");

    int *message = rssi_mailbox.alloc();
    if(message == NULL)
        return;
    *message = rssi; 
    rssi_mailbox.put(message); 
}

/**
 * @brief clears the range buffer/queue
 */
void clear_range()
{
    osEvent evt = range_mailbox.get(1); // Clear the queue 
    if(evt.status != osEventMail) 
        return;
    int *message = (int*)evt.value.p;
    range_mailbox.free(message);
}

/**
 * @brief returns the current range value
 * @param millisec      wait time in milisecond
 * @return              [the current range]
 */

int get_range(uint32_t millisec)
{ 
    osEvent evt = range_mailbox.get(millisec); // Clear the queue 
    if(evt.status != osEventMail) 
        return 0;

    int *message = (int*)evt.value.p;
    int range_data= *message; 
    range_mailbox.free(message);

    return range_data;
}

/**
 * @brief puts the recent range value in the buffer
 * @param range          range
 * @return              [ ]
 */
void put_range(int range)
{ 
    clear_range();

    int *message = range_mailbox.alloc();
    if(message == NULL)
        return;
    *message = range; 
    range_mailbox.put(message); 
}