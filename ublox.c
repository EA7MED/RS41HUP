//
// Created by Admin on 2016-12-27.
//

#include <stm32f10x_usart.h>
#include <string.h>
#include "ublox.h"
#include "delay.h"
#include "init.h"

GPSEntry currentGPSData;
volatile uint8_t active = 0;
void _sendSerialByte(uint8_t message) {
  while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {
  }
  USART_SendData(USART1, message);
}

void send_ublox(uint8_t msgClass, uint8_t msgId, uint8_t *payload, uint16_t payloadSize) {
  uBloxChecksum chksum = ublox_calc_checksum(msgClass, msgId, payload, payloadSize);

  _sendSerialByte(0xB5);
  _sendSerialByte(0x62);
  _sendSerialByte(msgClass);
  _sendSerialByte(msgId);
  _sendSerialByte((uint8_t) (payloadSize & 0xff));
  _sendSerialByte((uint8_t) (payloadSize >> 8));

  uint16_t i;
  for (i = 0; i < payloadSize; ++i) {
    _sendSerialByte(payload[i]);
  }
  _sendSerialByte(chksum.ck_a);
  _sendSerialByte(chksum.ck_b);
}

void send_ublox_packet(uBloxPacket * packet){
  send_ublox(packet->header.messageClass, packet->header.messageId, (uint8_t*)&packet->data, packet->header.payloadSize);
}

uBloxChecksum ublox_calc_checksum(const uint8_t msgClass, const uint8_t msgId, const uint8_t *message, uint16_t size) {
  uBloxChecksum ck = {0, 0};
  uint8_t i;
  ck.ck_a += msgClass;
  ck.ck_b += ck.ck_a;
  ck.ck_a += msgId;
  ck.ck_b += ck.ck_a;

  ck.ck_a += size & 0xff;
  ck.ck_b += ck.ck_a;
  ck.ck_a += size >> 8;
  ck.ck_b += ck.ck_a;


  for (i =0;i<size;i++){
    ck.ck_a += message[i];
    ck.ck_b += ck.ck_a;
  }
  return ck;
}

void ublox_get_last_data(GPSEntry * gpsEntry){
  __disable_irq();
  memcpy(gpsEntry, &currentGPSData, sizeof(GPSEntry));
  __enable_irq();
}

void ublox_init(){
  uBloxPacket msfcgprt = {.header = {0xb5, 0x62, .messageClass=0x06, .messageId=0x00, .payloadSize=sizeof(uBloxCFGPRTPayload)},
      .data.cfgprt = {.portID=1, .reserved1=0, .txReady=0, .mode=0b00100011000000, .baudRate=9600,
          .inProtoMask=1, .outProtoMask=1, .flags=0, .reserved2={0,0}}};
  send_ublox_packet(&msfcgprt);
  _delay_ms(10);
//  init_usart_gps(57600);
//  _delay_ms(10);

  uBloxPacket msgcfgmsg = {.header = {0xb5, 0x62, .messageClass=0x06, .messageId=0x01, .payloadSize=sizeof(uBloxCFGMSGPayload)},
    .data.cfgmsg = {.msgClass=0x01, .msgID=0x02, .rate=1}};
  send_ublox_packet(&msgcfgmsg);
  _delay_ms(10);

  msgcfgmsg.data.cfgmsg.msgID = 0x6;
  send_ublox_packet(&msgcfgmsg);
  _delay_ms(10);

  msgcfgmsg.data.cfgmsg.msgID = 0x21;
  send_ublox_packet(&msgcfgmsg);
  _delay_ms(10);

  uBloxPacket msgcfgnav5 = {.header = {0xb5, 0x62, .messageClass=0x06, .messageId=0x24, .payloadSize=sizeof(uBloxCFGNAV5Payload)},
    .data.cfgnav5={.mask=0b00000001111111111, .dynModel=8, .fixMode=3, .fixedAlt=0, .fixedAltVar=10000, .minElev=2, .drLimit=0, .pDop=0xfa, .tDop=0xfa,
                   .pAcc=100, .tAcc=300, .staticHoldThresh=0, .dgnssTimeout=0, .cnoThreshNumSVs=0, .cnoThresh=0, .reserved1={0,0}, .staticHoldMaxDist=0,
                   .utcStandard=0, .reserved2={0,0,0,0,0}}};
  send_ublox_packet(&msgcfgnav5);
  _delay_ms(10);
}

void ublox_handle_incoming_byte(uint8_t data){
  static uint8_t sync = 0;
  static uint8_t buffer_pos = 0;
  volatile static uBloxPacket incoming_packet;

  if (!sync){
    if (!buffer_pos && data == 0xB5){
      buffer_pos = 1;
      incoming_packet.header.sc1 = data;
    } else if (buffer_pos == 1 && data == 0x62){
      sync = 1;
      buffer_pos = 2;
      incoming_packet.header.sc2 = data;
    } else {
      buffer_pos = 0;
    }
  } else {
    ((uint8_t *)&incoming_packet)[buffer_pos] = data;
    if ((buffer_pos >= sizeof(uBloxHeader)-1) && (buffer_pos-1 == (incoming_packet.header.payloadSize + sizeof(uBloxHeader) + sizeof(uBloxChecksum)))){
      ublox_handle_packet((uBloxPacket *) &incoming_packet);
      buffer_pos = 0;
      sync = 0;
    } else {
      buffer_pos++;
      if (buffer_pos == 255) {
        buffer_pos = 0;
        sync = 0;
      }
    }
  }
}

void ublox_handle_packet(uBloxPacket *pkt) {
  uBloxChecksum cksum = ublox_calc_checksum(pkt->header.messageClass, pkt->header.messageId, (const uint8_t *) &pkt->data, pkt->header.payloadSize);
  uBloxChecksum *checksum = (uBloxChecksum *)(((uint8_t*)&pkt->data) + pkt->header.payloadSize);
  if (cksum.ck_a != checksum->ck_a || cksum.ck_b != checksum->ck_b) {
    currentGPSData.fix = 0xf;
  } else {
    if (pkt->header.messageClass == 0x01 && pkt->header.messageId == 0x07){
      currentGPSData.fix = pkt->data.navpvt.fixType;
      currentGPSData.lat_raw = pkt->data.navpvt.lat;
      currentGPSData.lon_raw = pkt->data.navpvt.lon;
      currentGPSData.alt_raw = pkt->data.navpvt.hMSL;
      currentGPSData.hours = pkt->data.navpvt.hour;
      currentGPSData.minutes = pkt->data.navpvt.min;
      currentGPSData.seconds = pkt->data.navpvt.sec;
      currentGPSData.sats_raw = pkt->data.navpvt.numSV;
      currentGPSData.speed_raw = pkt->data.navpvt.gSpeed;

    } else if (pkt->header.messageClass == 0x01 && pkt->header.messageId == 0x02){
      currentGPSData.lat_raw = pkt->data.navposllh.lat;
      currentGPSData.lon_raw = pkt->data.navposllh.lon;
      currentGPSData.alt_raw = pkt->data.navposllh.hMSL;
    } else if (pkt->header.messageClass == 0x01 && pkt->header.messageId == 0x06){
      currentGPSData.fix = pkt->data.navsol.gpsFix;
      currentGPSData.sats_raw = pkt->data.navsol.numSV;
    } else if (pkt->header.messageClass == 0x01 && pkt->header.messageId == 0x21){
      currentGPSData.hours = pkt->data.navtimeutc.hour;
      currentGPSData.minutes = pkt->data.navtimeutc.min;
      currentGPSData.seconds = pkt->data.navtimeutc.sec;
    } else if (pkt->header.messageClass == 0x05 && pkt->header.messageId == 0x01){
      currentGPSData.fix += 1;
    } else if (pkt->header.messageClass == 0x05 && pkt->header.messageId == 0x00){
      currentGPSData.fix += 30;
    }
  }

}


