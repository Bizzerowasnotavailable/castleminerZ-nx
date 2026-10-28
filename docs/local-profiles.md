# Local Switch profiles

The native host reads the console-local user selected for the application using
libnx's `acc:u0` service, with read access through `acc:u1` as a fallback for
homebrew launches without application metadata. A linked Nintendo Account is
**not** required. The port
never requests a network-service account ID, online token, or account-linking UI.

If the launcher provides no selected user, the host tries local selection without
interaction, then the regular user selector with
`is_network_service_account_required = false`. If that applet is unavailable, it
tries the last opened local user. If no local profile is available, it uses an
offline guest instead.

## XNA mapping

- Player one's `SignedInGamer.Gamertag` is the selected user's UTF-8 nickname.
- `GamerProfile.GetGamerPicture()` reads a private SD copy of that user's profile
  JPEG, or a project-owned PNG fallback if the image cannot be read.
- Local network-session gamers inherit the same nickname and profile picture.
- The avatar API returns a stable project-owned placeholder, not an Xbox avatar
  or a converted Mii. The avatar renderer still has no mesh.
- `IsSignedInToLive` stays false; online sessions and purchases stay unavailable.
  Xbox friends, invitations, marketplace and account services are not redirected
  to Nintendo's online services.
- Additional controller slots are offline placeholders, not other console users
  silently signed in without selection.

## Data separation

FNA's preference/storage root is selected before managed startup:

```text
sdmc:/switch/castleminerz/save/profiles/<local-uid>/
sdmc:/switch/castleminerz/save/guest/
```

The local UID is a 32-character hexadecimal directory key, not a Nintendo Account
ID. Nickname changes do not change that directory. A storage error never redirects
a selected user's writes into another user's directory. Existing unscoped saves
are not moved, deleted, or automatically imported.

The original game also derives save encryption keys and world ownership from its
gamertag. Nickname-change compatibility still needs testing; a stable directory
alone does not guarantee that the original game will accept an old encrypted save
after a rename. Back up saves before changing a profile nickname.

Profile pictures stay under `sdmc:/switch/castleminerz/data/profiles/<local-uid>/`.
They are never staged into RomFS or source control. Logs can contain the local
nickname or directory key; review them before sharing.

## Tests

```powershell
./scripts/test-local-profiles.ps1
```

These synthetic tests cover nickname mapping, invalid UID rejection, pictures and
fallbacks, offline privileges, and local-session identity propagation. They do not
replace physical Switch account-selector, multi-user, save/load, or rename tests.

Eden selected its configured local profile, used that UID's storage root, and
logged the offline session's `Player Joined` event with the profile nickname.
Offline world entry is now confirmed visually. Player statistics, world metadata
and inventory files are created under that profile's storage root. File creation
does not yet establish successful reload, multi-user isolation or rename safety.
This verifies the local account-service path in the emulator, not hardware.
