// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************


#ifndef __LAN_H__
#define __LAN_H__



int LAN_init(void);
void start_udp_receive_task();
void start_osc_recive_task();
void start_ftp_task();
void start_mdns_task();
void start_mqtt_task();
int udplink_send(int slot_num, const char * message);
const char * networkGetStatusString();

#endif // #define __LAN_H__