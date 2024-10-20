/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

MARIONETTE_TIMEOUT = 60000;
MARIONETTE_HEAD_JS = "head.js";

// Start tests
startTestCommon(
  function() {
    // The emulator's hard coded operatoer's mcc and mnc codes.
    is(mobileConnection.lastKnownNetwork, "310-410");
    // The emulator's hard coded icc's mcc, mnc codes and spn.
    is(mobileConnection.lastKnownHomeNetwork, "310-410-Android");
  },
  ["mobilenetwork"]
);
