#!/bin/sh
# notarize_mac.sh - send a Developer-ID-signed Syndicate64.app to Apple's notary service and staple the ticket.
#
#   tools/notarize_mac.sh [APP] [PROFILE]     (defaults: build/Syndicate64.app, the keychain profile civ64-notary)
#
# The app must be signed with a Developer ID Application identity, the hardened runtime and a secure timestamp:
#   ./build.sh -DSYN_CODESIGN_IDENTITY="Developer ID Application: <name> (<team>)"
# The profile is made once with: xcrun notarytool store-credentials <PROFILE> --apple-id ... --team-id ...
set -e
APP="${1:-build/Syndicate64.app}"
PROFILE="${2:-civ64-notary}"
ZIP="$(mktemp -d)/Syndicate64.zip"
codesign --verify --strict --deep "$APP"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$APP"
spctl --assess --type execute -vv "$APP"
rm -f "$ZIP"
