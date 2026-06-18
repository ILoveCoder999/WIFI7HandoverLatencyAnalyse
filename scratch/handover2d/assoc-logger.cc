#include "assoc-logger.h"
#include "ns3/simulator.h"
#include "assoc-info.h"


NS_LOG_COMPONENT_DEFINE("AssocLogger");

AssocLogger::AssocLogger(std::string out_file_path, std::string header, Ptr<MobilityModel> mobility):
     _header(header), _mobility(mobility)
{
    _output_file.open(out_file_path.c_str(), std::ios::out | std::ios::trunc);
}

void AssocLogger::logHeader()
{
    _output_file << "[" << std::endl << _header;
}

void AssocLogger::logFooter()
{
    _output_file << std::endl << "]";
}

void AssocLogger::assocCallback(Mac48Address value)
{
    AssocInfo ai = {
        AssocInfoType::assoc,
        Simulator::Now(),
        _currentPosition(),
        value        
    };
    _output_file << "," << std::endl << json(ai);
}

void AssocLogger::deAssocCallback(Mac48Address value)
{
    AssocInfo ai = {
        AssocInfoType::deassoc,
        Simulator::Now(),
        _currentPosition(),
        value        
    };
    _output_file << "," << std::endl << json(ai);
}

void AssocLogger::beaconArrivalCallback(Time t)
{   
    BeaconInfo bi {
        BeaconInfoType::arrival,
        t,
        _currentPosition()
    };
    _output_file << "," << std::endl << json(bi);
}

void AssocLogger::receivedBeaconInfoCallback(StaWifiMac::ApInfo apInfo)
{
    BeaconInfo bi {
        BeaconInfoType::info,
        Simulator::Now(),
        _currentPosition(),
        apInfo
    };
    _output_file << "," << std::endl << json(bi);
};

inline std::tuple<double, double, double> AssocLogger::_currentPosition(){
    Vector position = _mobility->GetPosition();
    return std::make_tuple(position.x, position.y, position.z);
}
