#pragma once

#include <string>

// Reads only the console's local account service. It never requests a Nintendo
// Account, network-service account, online token, or account linking.
struct CmzLocalProfile {
    bool available = false;
    std::string id;
    std::string pref_directory = "/save/guest/";
};

CmzLocalProfile cmz_initialize_local_profile();
