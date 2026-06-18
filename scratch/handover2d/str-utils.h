#include <vector>
#include <string>

namespace str_utils {
    std::vector<std::string> split_str(const std::string& s, const std::string& delimiter);
    void ltrim(std::string &s);
    void rtrim(std::string &s);
    void trim(std::string &s);
}
