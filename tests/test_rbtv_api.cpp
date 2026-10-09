#include "rbtv_api.h"

#include <cassert>
#include <iostream>

int main() {
    assert(rbtv::md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    assert(rbtv::md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    assert(rbtv::md5_hex("language=0&sportType=1&stream=true") ==
           "6ae3f9cc0b66c659c13c8727e624fef1");
    assert(rbtv::signature_prefix("language=0&sportType=1&stream=true") == "6ae3f9");
    assert(rbtv::signature_prefix("matchId=123&language=0&sportType=1&stream=true") == "21f511");
    std::cout << "RBTV API signature tests passed\n";
    return 0;
}
