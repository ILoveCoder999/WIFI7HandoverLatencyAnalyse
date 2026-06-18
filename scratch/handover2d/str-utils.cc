#include "str-utils.h"
#include <algorithm> 

/*
FROM https://stackoverflow.com/a/14266139
*/ 
std::vector<std::string> str_utils::split_str(const std::string& s, const std::string& delimiter) {
    std::vector<std::string> tokens;
    size_t last = 0;
    size_t next = 0;
    std::string token;
    while ((next = s.find(delimiter, last)) != std::string::npos) {
        token = s.substr(last, next-last);
        tokens.push_back(token);
        last = next + 1;
    }
    tokens.push_back(s.substr(last));
    return tokens;
}

/*
FROM https://stackoverflow.com/a/217605
*/ 

// Trim from the start (in place)
void str_utils::ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
}

// Trim from the end (in place)
void str_utils::rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// Trim from both ends (in place)
void str_utils::trim(std::string &s) {
    rtrim(s);
    ltrim(s);
}
