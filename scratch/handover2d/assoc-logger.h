#ifndef ASSOC_LOGGER_H
#define ASSOC_LOGGER_H

// standard includes
#include <fstream>
#include <iostream>

// ns3 includes
#include "ns3/pointer.h"
#include "ns3/mobility-model.h"
#include "ns3/wifi-mac.h"
#include "ns3/sta-wifi-mac.h"

using namespace ns3;

class AssocLogger
{
    public:
        AssocLogger(std::string out_file_path, std::string header, Ptr<MobilityModel> mobility);
        ~AssocLogger() {};

        void logHeader();
        void logFooter();

        /* CALLBACKS*/
        void assocCallback(Mac48Address value);
        void deAssocCallback(Mac48Address value);
        void beaconArrivalCallback(Time t);
        void receivedBeaconInfoCallback(StaWifiMac::ApInfo apInfo);        
    
    protected:
        std::tuple<double, double, double> _currentPosition();

        std::ofstream _output_file;
        std::string _header;       
        Ptr<MobilityModel> _mobility;
};

#endif
