/*
 * FILE: rdt_sender.cc
 * DESCRIPTION: Reliable data transfer sender.
 * NOTE: This implementation assumes there is no packet loss, corruption, or 
 *       reordering.  You will need to enhance it to deal with all these 
 *       situations.  In this implementation, the packet format is laid out as 
 *       the following:
 *       
 *       |<-  2 byte  ->|<-   2 byte   ->|<-    1 byte    ->|<-            the rest           ->|
 *       |<- checksum ->|<- pkt number ->|<- payload size ->|<-            payload            ->|
 *
 *       The first byte of each packet indicates the size of the payload
 *       (excluding this single-byte header)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>

#include "rdt_struct.h"
#include "rdt_sender.h"

/* predefined variables */
#define TIMEOUT 2
#define WINDOWSIZE 4

using namespace std;

/* sender ack flag */
bool sender_ack;

/* sender pkt sequence */
unsigned short pkt_num;

/* pkt buffer */
deque<struct packet> pkt_buffer;
deque<unsigned short> num_buffer;

/* sender initialization, called once at the very beginning */
void Sender_Init()
{
    fprintf(stdout, "At %.2fs: sender initializing ...\n", GetSimulationTime());
    sender_ack = false;
    pkt_num = 0;
}

/* sender finalization, called once at the very end.
   you may find that you don't need it, in which case you can leave it blank.
   in certain cases, you might want to take this opportunity to release some 
   memory you allocated in Sender_init(). */
void Sender_Final()
{
    fprintf(stdout, "At %.2fs: sender finalizing ...\n", GetSimulationTime());
}

/* sender checksum */
unsigned short Sender_Checksum(struct packet *pkt)
{
    unsigned short checksum = 0;
    unsigned int tmp = 0;
    /* since the RDT_PKTSIZE is a even number, this will cover the whole pkt */
    for (int i = 2; i < RDT_PKTSIZE; i += 2)
    {
        tmp += *(unsigned short *)(&(pkt->data[i]));
    }
    tmp = (tmp >> 16) + (tmp & 0xffff);
    tmp += (tmp >> 16);
    checksum = ~tmp;
    return checksum;
}

void Sender_HandleBufferChange(struct packet *pkt, unsigned int pkt_num)
{
    /* add new packet and its number to their buffer */
    pkt_buffer.push_back(*pkt);
    num_buffer.push_back(pkt_num);

    /* if buffer is not filled before insertion, send the newly added packet */
    if (num_buffer.size() <= WINDOWSIZE)
    {
        Sender_StartTimer(TIMEOUT);
        Sender_ToLowerLayer(&pkt_buffer.back());
    }
}

/* event handler, called when a message is passed from the upper layer at the 
   sender */
void Sender_FromUpperLayer(struct message *msg)
{
    /* 5-byte header indicating the checksum and sequence of the packet and the size of the payload */
    int header_size = 5;

    /* maximum payload size */
    int maxpayload_size = RDT_PKTSIZE - header_size;

    /* split the message if it is too big */

    /* reuse the same packet data structure */
    packet pkt;

    /* initiate checksum */
    unsigned short checksum;

    /* the cursor always points to the first unsent byte in the message */
    int cursor = 0;

    while (msg->size - cursor > maxpayload_size)
    {
        /* fill in the packet */
        pkt.data[4] = maxpayload_size;
        memcpy(pkt.data + header_size, msg->data + cursor, maxpayload_size);
        memcpy(&pkt.data[2], &pkt_num, sizeof(unsigned short));

        /* add checksum to the packet */
        checksum = Sender_Checksum(&pkt);
        memcpy(pkt.data, &checksum, sizeof(unsigned short));

        /* send it out through the lower layer */
        /* Sender_ToLowerLayer(&pkt); */

        /* add it to buffer */
        Sender_HandleBufferChange(&pkt, pkt_num);

        /* move the cursor */
        cursor += maxpayload_size;

        /* increase the pkt number */
        pkt_num++;
    }

    /* send out the last packet */
    if (msg->size > cursor)
    {
        /* fill in the packet */
        pkt.data[4] = msg->size - cursor;
        memcpy(pkt.data + header_size, msg->data + cursor, pkt.data[4]);
        memcpy(&pkt.data[2], &pkt_num, sizeof(unsigned short));

        /* add checksum to the packet */
        checksum = Sender_Checksum(&pkt);
        memcpy(pkt.data, &checksum, sizeof(unsigned short));

        /* send it out through the lower layer */
        /* Sender_ToLowerLayer(&pkt); */

        /* add it to buffer */
        Sender_HandleBufferChange(&pkt, pkt_num);

        /* increase the pkt number */
        pkt_num++;
    }
}

/* event handler, called when a packet is passed from the lower layer at the 
   sender */
void Sender_FromLowerLayer(struct packet *pkt)
{
    /* perform checksum before further operation */
    unsigned short checksum;
    checksum = Sender_Checksum(pkt);
    if (memcmp(&checksum, pkt, sizeof(unsigned short)) != 0)
    {
        fprintf(stdout, "At %.2fs: sender receives a corrupted ACK\n", GetSimulationTime());
        return;
    }

    /* extract ack_num from ACK packet */
    unsigned short ack_num;
    memcpy(&ack_num, &pkt->data[2], sizeof(unsigned short));
    fprintf(stdout, "At %.2fs: sender receives ACK %u\n", GetSimulationTime(), ack_num);

    /* if the first packet in the buffer got its ACK */
    if (ack_num == num_buffer.front())
    {
        /* remove it from the buffer and shift the window */
        num_buffer.pop_front();
        pkt_buffer.pop_front();

        /* if new packet appears, send it, if not, do nothing */
        if (num_buffer.size() >= WINDOWSIZE)
        {
            Sender_StartTimer(TIMEOUT);
            Sender_ToLowerLayer(&pkt_buffer[WINDOWSIZE - 1]);
        }
    }
}

/* event handler, called when the timer expires */
void Sender_Timeout()
{
    fprintf(stdout, "At %.2fs: sender times out\n", GetSimulationTime());
    if (num_buffer.size() == 0)
    {
        fprintf(stdout, "At %.2fs: no packet in buffer, timer stops\n", GetSimulationTime());
        return;
    }

    /* restart the timer */
    Sender_StartTimer(TIMEOUT);

    /* resend all packet in buffered area */
    int buffered_pkt_num = WINDOWSIZE > num_buffer.size() ? num_buffer.size() : WINDOWSIZE;
    for (int i = 0; i < buffered_pkt_num; i++)
    {
        fprintf(stdout, "At %.2fs: sender resend packet %u\n", GetSimulationTime(), num_buffer[i]);
        Sender_ToLowerLayer(&pkt_buffer[i]);
    }
}
