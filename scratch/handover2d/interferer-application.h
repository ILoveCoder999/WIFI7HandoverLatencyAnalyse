//
// Created by matteo on 29/05/24.
//

#ifndef INTERFERENCE_APPLICATION_H
#define INTERFERENCE_APPLICATION_H

#include "ns3/socket.h"
#include "ns3/application.h"
#include "ns3/random-variable-stream.h"

namespace ns3 {

class InterfererApplication : public Application {
public:
  InterfererApplication();
  ~InterfererApplication() override;

  static TypeId GetTypeId();

private:
  void StartApplication() override;
  void StopApplication() override;
  void SendPacket();

  Ptr<RandomVariableStream> m_offTime;
  Ptr<RandomVariableStream> m_burstSize;
  Time m_burstPacketsInterval;
  uint32_t m_burstPacketsSize;
  uint32_t m_packetNumber;
  uint32_t m_currentBurstSize;
  Ipv4Address m_peerAddress;

  EventId m_sendEvent;
  Ptr<Socket> m_socket;
};

} // ns3

#endif //INTERFERENCE_APPLICATION_H
