#include "roaming-manager.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"
#include "ns3/string.h"
#include "ns3/log.h"
#include "ns3/sta-wifi-mac.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <sstream>

NS_LOG_COMPONENT_DEFINE("RoamingManager");

// Dwell time per channel during scan (ms).
// Must be > beacon interval (default ~102 ms). 150 ms = one beacon + margin.
static const uint32_t SCAN_DWELL_MS = 150;

// --------------------------------------------------------------------------
// Constructor
// --------------------------------------------------------------------------

RoamingManager::RoamingManager(Ptr<WifiNetDevice> staDevice,
                               Ptr<MobilityModel> mobility,
                               std::vector<std::string> channel_list)
    : _staDevice(staDevice),
      _mobility(mobility),
      _channel_list(channel_list),
      _scanning(false),
      _scan_step(0),
      _best_channel_idx(0),
      _best_rssi(-9999.0)
{
    std::string curr_channel = RoamingManager::extractChannelString(_staDevice);
    auto iter = std::find(_channel_list.begin(), _channel_list.end(), curr_channel);
    size_t index = std::distance(_channel_list.begin(), iter);
    if (index == _channel_list.size()) {
        _channel_list.insert(_channel_list.begin(), curr_channel);
        index = 0;
    }
    _cur_channel_idx = index;
    NS_LOG_INFO("RoamingManager init: current channel[" << index << "] = " << curr_channel);
}

// --------------------------------------------------------------------------
// Public callbacks
// --------------------------------------------------------------------------

void RoamingManager::assocCallback(Mac48Address value)
{
    if (_scanning) {
        NS_LOG_INFO("Associated with " << value << " during scan — stopping scan");
        _scanning = false;
    }
}

void RoamingManager::deAssocCallback(Mac48Address value)
{
    if (_scanning) {
        NS_LOG_INFO("DeAssoc from " << value << " while already scanning — ignored");
        return;
    }
    NS_LOG_INFO("DeAssoc from " << value << " — building smart scan order");

    _scanning = true;
    _channel_rssi.clear();
    _best_channel_idx = (int)_cur_channel_idx;   // safe fallback: stay on current
    _best_rssi        = -9999.0;
    _scan_step        = 0;

    _buildScanOrder();
    _scanNextChannel();
}

void RoamingManager::receivedBeaconInfoCallback(StaWifiMac::ApInfo apInfo)
{
    double snr = apInfo.m_snr;

    if (_scanning) {
        // Per-scan temporary record used to pick the best channel at the end
        int ch_idx = _scan_order[_scan_step];
        NS_LOG_INFO("Beacon on channel[" << ch_idx << "] (scan step "
                    << _scan_step << ") SNR=" << snr);

        auto it = _channel_rssi.find(ch_idx);
        if (it == _channel_rssi.end() || snr > it->second) {
            _channel_rssi[ch_idx] = snr;
        }

        // ★ Persist in history so future scans can prioritise this channel
        auto hit = _beacon_history.find(ch_idx);
        if (hit == _beacon_history.end() || snr > hit->second) {
            _beacon_history[ch_idx] = snr;
            NS_LOG_INFO("Beacon history updated: channel[" << ch_idx
                        << "] best SNR=" << snr);
        }
    }
}

// --------------------------------------------------------------------------
// Channel string helper
// --------------------------------------------------------------------------

std::string RoamingManager::extractChannelString(Ptr<WifiNetDevice> staDevice)
{
    Ptr<WifiPhy> phy = staDevice->GetPhy();
    std::string band;
    std::stringstream ss;

    switch (phy->GetPhyBand()) {
        case WIFI_PHY_BAND_2_4GHZ: band = "2_4GHZ";     break;
        case WIFI_PHY_BAND_5GHZ:   band = "5GHZ";       break;
        case WIFI_PHY_BAND_6GHZ:   band = "6GHZ";       break;
        default:                   band = "UNSPECIFIED"; break;
    }

    ss << "{"
       << static_cast<uint32_t>(phy->GetChannelNumber()) << ","
       << static_cast<uint32_t>(phy->GetChannelWidth())  << ","
       << "BAND_" << band
       << "," << static_cast<uint32_t>(phy->GetPrimary20Index()) << "}";

    NS_LOG_FUNCTION(ss.str());
    return ss.str();
}

// --------------------------------------------------------------------------
// Private: build smart scan order
//
// Priority rules (highest → lowest):
//   1. Channels with beacon history, sorted by best SNR descending
//   2. Channels with no history (in original config order)
//   3. Current channel — always last (it just lost its AP)
// --------------------------------------------------------------------------

void RoamingManager::_buildScanOrder()
{
    _scan_order.clear();

    // Separate channels into: has history / no history / current
    std::vector<int> with_history;
    std::vector<int> no_history;

    for (int i = 0; i < (int)_channel_list.size(); i++) {
        if (i == (int)_cur_channel_idx) continue;   // handle current last
        if (_beacon_history.count(i)) {
            with_history.push_back(i);
        } else {
            no_history.push_back(i);
        }
    }

    // Sort channels-with-history by SNR descending
    std::sort(with_history.begin(), with_history.end(), [&](int a, int b) {
        return _beacon_history.at(a) > _beacon_history.at(b);
    });

    for (int idx : with_history) _scan_order.push_back(idx);
    for (int idx : no_history)   _scan_order.push_back(idx);
    _scan_order.push_back((int)_cur_channel_idx);   // current channel scanned last

    // Log the order
    std::stringstream ss;
    for (int idx : _scan_order) {
        double snr = _beacon_history.count(idx) ? _beacon_history.at(idx) : -9999.0;
        ss << "  channel[" << idx << "]=" << _channel_list[idx]
           << " (history SNR=" << snr << ")\n";
    }
    NS_LOG_INFO("Smart scan order:\n" << ss.str());
}

// --------------------------------------------------------------------------
// Private: scan state machine
// --------------------------------------------------------------------------

void RoamingManager::_scanNextChannel()
{
    if (!_scanning) return;

    if (_scan_step >= (int)_scan_order.size()) {
        _finalizeScan();
        return;
    }

    int ch_idx = _scan_order[_scan_step];
    const std::string& ch = _channel_list[ch_idx];
    NS_LOG_INFO("Dwell on channel[" << ch_idx << "] = " << ch
                << "  (scan step " << _scan_step << "/"
                << _scan_order.size() << ")");

    _staDevice->GetPhy()->SetAttribute("ChannelSettings", StringValue(ch));

    Simulator::Schedule(MilliSeconds(SCAN_DWELL_MS),
                        &RoamingManager::_onDwellComplete, this);
}

void RoamingManager::_onDwellComplete()
{
    if (!_scanning) return;

    int ch_idx = _scan_order[_scan_step];
    auto it = _channel_rssi.find(ch_idx);
    if (it != _channel_rssi.end()) {
        double rssi = it->second;
        NS_LOG_INFO("Channel[" << ch_idx << "] best SNR=" << rssi);
        if (rssi > _best_rssi) {
            _best_rssi        = rssi;
            _best_channel_idx = ch_idx;
        }
    } else {
        NS_LOG_INFO("Channel[" << ch_idx << "] no beacon received");
    }

    _scan_step++;
    _scanNextChannel();
}

void RoamingManager::_finalizeScan()
{
    _scanning        = false;
    _cur_channel_idx = (size_t)_best_channel_idx;

    NS_LOG_INFO("Scan done → channel[" << _best_channel_idx << "] = "
                << _channel_list[_best_channel_idx]
                << "  SNR=" << _best_rssi);

    _staDevice->GetPhy()->SetAttribute("ChannelSettings",
                                       StringValue(_channel_list[_cur_channel_idx]));

    Ptr<StaWifiMac> staMac = DynamicCast<StaWifiMac>(_staDevice->GetMac());
    if (staMac != nullptr) {
        staMac->SetSsid(staMac->GetSsid());   // kick MAC into scanning/association
    }
}

// --------------------------------------------------------------------------
// Private: position helper
// --------------------------------------------------------------------------

std::tuple<double, double, double> RoamingManager::_currentPosition()
{
    Vector position = _mobility->GetPosition();
    return std::make_tuple(position.x, position.y, position.z);
}