/*
 * Copyright (c) 2008 INRIA
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Mohamed Amine Ismail <amine.ismail@sophia.inria.fr>
 */

#ifndef MY_UDP_CLIENT_HELPER_H
#define MY_UDP_CLIENT_HELPER_H

#include <ns3/application-helper.h>
#include "my-udp-client.h"

#include <stdint.h>

namespace ns3
{
/**
 * \ingroup udpclientserver
 * \brief Create a client application which sends UDP packets carrying
 *  a 32bit sequence number and a 64 bit time stamp.
 *
 */
class MyUdpClientHelper : public ApplicationHelper
{
  public:
    /**
     * Create UdpClientHelper which will make life easier for people trying
     * to set up simulations with udp-client-server.
     *
     */
    MyUdpClientHelper();

    /**
     *  Create UdpClientHelper which will make life easier for people trying
     * to set up simulations with udp-client-server. Use this variant with
     * addresses that do not include a port value (e.g., Ipv4Address and
     * Ipv6Address).
     *
     * \param ip The IP address of the remote UDP server
     * \param port The port number of the remote UDP server
     */

    MyUdpClientHelper(const Address& ip, uint16_t port);
    /**
     *  Create UdpClientHelper which will make life easier for people trying
     * to set up simulations with udp-client-server. Use this variant with
     * addresses that do include a port value (e.g., InetSocketAddress and
     * Inet6SocketAddress).
     *
     * \param addr The address of the remote UDP server
     */

    MyUdpClientHelper(const Address& addr);
};

} // namespace ns3

#endif /* MY_UDP_CLIENT_HELPER_H */
