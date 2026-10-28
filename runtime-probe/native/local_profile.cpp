#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "local_profile.h"

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace {

bool directory(const std::string& path) {
    struct stat info {};
    if (stat(path.c_str(), &info) == 0) {
        return S_ISDIR(info.st_mode);
    }
    return mkdir(path.c_str(), 0777) == 0;
}

std::string profile_id(AccountUid uid) {
    char text[33];
    std::snprintf(text, sizeof(text), "%016llx%016llx",
                  static_cast<unsigned long long>(uid.uid[0]),
                  static_cast<unsigned long long>(uid.uid[1]));
    return text;
}

void cache_picture(AccountProfile* profile, const std::string& id) {
    constexpr u32 maximum_size = 512 * 1024;
    u32 size = 0;
    if (R_FAILED(accountProfileGetImageSize(profile, &size)) ||
        size == 0 || size > maximum_size) {
        return;
    }
    auto* data = static_cast<unsigned char*>(std::malloc(size));
    if (data == nullptr) {
        return;
    }
    u32 actual = 0;
    const Result result = accountProfileLoadImage(profile, data, size, &actual);
    const std::string root = "sdmc:/switch/castleminerz/data/profiles";
    const std::string user = root + "/" + id;
    if (R_SUCCEEDED(result) && actual > 0 && actual <= size &&
        directory(root) && directory(user)) {
        const std::string path = user + "/picture.jpg";
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file != nullptr) {
            const bool written = std::fwrite(data, 1, actual, file) == actual;
            const bool closed = std::fclose(file) == 0;
            if (written && closed) {
                const std::string managed_path = "/data/profiles/" + id + "/picture.jpg";
                setenv("CMZ_LOCAL_PROFILE_PICTURE", managed_path.c_str(), 1);
            }
        }
    }
    std::free(data);
}

} // namespace

CmzLocalProfile cmz_initialize_local_profile() {
    CmzLocalProfile selected;
    unsetenv("CMZ_LOCAL_PROFILE_ID");
    unsetenv("CMZ_LOCAL_PROFILE_NAME");
    unsetenv("CMZ_LOCAL_PROFILE_PICTURE");
    directory("sdmc:/switch/castleminerz/save/guest");

    if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
        // Homebrew NROs may have no application launch property. acc:u1 can
        // still read local users; it does not require a linked online account.
        if (R_FAILED(accountInitialize(AccountServiceType_System))) {
            return selected;
        }
    }

    AccountUid uid {};
    accountGetPreselectedUser(&uid);
    if (!accountUidIsValid(&uid)) {
        // false explicitly permits local profiles that have no linked account.
        accountTrySelectUserWithoutInteraction(&uid, false);
    }
    if (!accountUidIsValid(&uid)) {
        PselUserSelectionSettings settings {};
        settings.is_network_service_account_required = false;
        settings.is_unqualified_user_selectable = true;
        const Result result = pselShowUserSelector(&uid, &settings);
        if (R_FAILED(result)) {
            // Some homebrew launchers/emulators do not implement the selector.
            // Prefer the existing local user over inventing an online identity.
            accountGetLastOpenedUser(&uid);
        }
    }

    AccountProfile profile {};
    if (accountUidIsValid(&uid) && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountProfileBase base {};
        if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base))) {
            const std::string id = profile_id(uid);
            const std::string save_root = "sdmc:/switch/castleminerz/save/profiles";
            // A storage error must not redirect this user into another user's
            // directory. Keep the UID-scoped path even if mkdir fails.
            directory(save_root);
            directory(save_root + "/" + id);
            char nickname[sizeof(base.nickname) + 1] {};
            std::memcpy(nickname, base.nickname, sizeof(base.nickname));
            setenv("CMZ_LOCAL_PROFILE_ID", id.c_str(), 1);
            setenv("CMZ_LOCAL_PROFILE_NAME", nickname[0] ? nickname : "Switch User", 1);
            cache_picture(&profile, id);
            selected.available = true;
            selected.id = id;
            selected.pref_directory = "/save/profiles/" + id + "/";
        }
        accountProfileClose(&profile);
    }
    accountExit();
    return selected;
}
