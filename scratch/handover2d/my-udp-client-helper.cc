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

#include "my-udp-client-helper.h"

#include <ns3/string.h>
#include <ns3/uinteger.h>

namespace ns3
{

MyUdpClientHelper::MyUdpClientHelper()
    : ApplicationHelper(MyUdpClient::GetTypeId())
{
}

MyUdpClientHelper::MyUdpClientHelper(const Address& address)
    : MyUdpClientHelper()
{
    SetAttribute("RemoteAddress", AddressValue(address));
}

MyUdpClientHelper::MyUdpClientHelper(const Address& address, uint16_t port)
    : MyUdpClientHelper(address)
{
    SetAttribute("RemotePort", UintegerValue(port));
}

} // namespace ns3
