//
// Created by matteo on 26/07/24.
//

/* FROM:
- https://groups.google.com/g/ns-3-users/c/L_ZfZ9L1_b0/m/dCuzc859ENcJ
- https://gist.github.com/SzymonSzott/de5c431d687f7b3a0b10743af6ac7ce2
*/

#include "utils.h"

#include "ns3/pointer.h"
#include "ns3/arp-cache.h"
#include "ns3/ipv4-interface.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/object-vector.h"

using namespace ns3;

void
PopulateArpCache()
{
    // Creates ARP Cache object
    Ptr<ArpCache> arp = CreateObject<ArpCache>();

    // Set ARP Timeout
    arp->SetAliveTimeout(Seconds(3600 * 24 * 365)); // 1-year

    // Populates ARP Cache with information from all nodes
    for (auto i = NodeList::Begin(); i != NodeList::End(); ++i)
    {
        // Get an interactor to Ipv4L3Protocol instance
        Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol>();
        NS_ASSERT(ip != nullptr);

        // Get interfaces list from Ipv4L3Protocol iteractor
        ObjectVectorValue interfaces;
        ip->GetAttribute("InterfaceList", interfaces);

        // For each interface
        for (auto j = interfaces.Begin(); j != interfaces.End(); ++j)
        {
            // Get an interactor to Ipv4L3Protocol instance
            Ptr<Ipv4Interface> ipIface = j->second->GetObject<Ipv4Interface>();
            NS_ASSERT(ipIface != nullptr);

            // Get interfaces list from Ipv4L3Protocol iteractor
            Ptr<NetDevice> device = ipIface->GetDevice();
            NS_ASSERT(device != nullptr);

            // Get MacAddress assigned to this device
            Mac48Address addr = Mac48Address::ConvertFrom(device->GetAddress());

            // For each Ipv4Address in the list of Ipv4Addresses assign to this interface...
            for (uint32_t k = 0; k < ipIface->GetNAddresses(); k++)
            {
                // Get Ipv4Address
                Ipv4Address ipAddr = ipIface->GetAddress(k).GetLocal();

                // If Loopback address, go to the next
                if (ipAddr == Ipv4Address::GetLoopback())
                {
                    continue;
                }

                // Creates an ARP entry for this Ipv4Address and adds it to the ARP Cache
                ArpCache::Entry* entry = arp->Add(ipAddr);

                /*CLUSTER VERSION (fails in DEBUG because the function asserts that the entry is in WAIT_REPLY)*/                
                //entry->MarkAlive(addr);

                /*ORIGINAL VERSION (simulates the waiting for an ARP response, then marks entry alive)*/
				// Ipv4Header ipv4Hdr;
				// ipv4Hdr.SetDestination (ipAddr);
				// Ptr<Packet> p = Create<Packet> (100);
				// entry->MarkWaitReply (ArpCache::Ipv4PayloadHeaderPair (p, ipv4Hdr));
				// entry->MarkAlive (addr);               

                /*MATTEO VERSION NEW (adds the entry as static)*/
                entry->SetMacAddress(addr);
                entry->MarkPermanent();

                NS_LOG_UNCOND("Arp Cache: Adding the pair (" << addr << "," << ipAddr << ")");
            }
        }
    }

    // Assign ARP Cache to each interface of each node
    for (auto i = NodeList::Begin(); i != NodeList::End(); ++i)
    {
        Ptr<Ipv4L3Protocol> ip = (*i)->GetObject<Ipv4L3Protocol>();
        NS_ASSERT(ip != nullptr);
        ObjectVectorValue interfaces;
        ip->GetAttribute("InterfaceList", interfaces);
        for (auto j = interfaces.Begin(); j != interfaces.End(); ++j)
        {
            Ptr<Ipv4Interface> ipIface = j->second->GetObject<Ipv4Interface>();
            ipIface->SetAttribute("ArpCache", PointerValue(arp));
        }
    }
}