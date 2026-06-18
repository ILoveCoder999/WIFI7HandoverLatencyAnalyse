#pragma once

#include "ns3/wifi-net-device.h"
#include "ns3/mobility-model.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/mac48-address.h"

#include <vector>
#include <string>
#include <map>
#include <tuple>

using namespace ns3;

class RoamingManager
{
public:
    RoamingManager(Ptr<WifiNetDevice> staDevice,
                   Ptr<MobilityModel> mobility,
                   std::vector<std::string> channel_list);

    void assocCallback(Mac48Address value);
    void deAssocCallback(Mac48Address value);
    void receivedBeaconInfoCallback(StaWifiMac::ApInfo apInfo);

    static std::string extractChannelString(Ptr<WifiNetDevice> staDevice);

private:
    void _buildScanOrder();
    void _scanNextChannel();
    void _onDwellComplete();
    void _finalizeScan();
    std::tuple<double, double, double> _currentPosition();

    Ptr<WifiNetDevice>        _staDevice;
    Ptr<MobilityModel>        _mobility;
    std::vector<std::string>  _channel_list;
    size_t                    _cur_channel_idx;

    // Scan state machine
    bool                      _scanning;
    int                       _scan_step;          // index into _scan_order
    int                       _best_channel_idx;   // index into _channel_list
    double                    _best_rssi;
    std::map<int, double>     _channel_rssi;       // per-scan: channel_idx → best SNR

    // Smart scan: ordered list of _channel_list indices for the current scan.
    // Built in _buildScanOrder() before each scan.
    std::vector<int>          _scan_order;

    // Persistent beacon history: channel_idx → best SNR ever seen on that channel
    // (accumulated across all scans; survives reassociation).
    std::map<int, double>     _beacon_history;
};