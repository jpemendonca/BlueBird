#ifndef VERSION_H
#define VERSION_H

#define BB_NAME        "BlueBird"
/* Test builds pass their own label and number (make -f ps5.mk VERSION=... BUILD_NUM=...) */
#ifndef BB_VERSION
#define BB_VERSION     "0.1.0"
#endif
#ifndef BB_BUILD
#define BB_BUILD       1
#endif
#define BB_DESCRIPTION "PS5 audio on Bluetooth speakers, through a USB Bluetooth adapter"

/* Not 8095 (Dump Forge) nor 8105 (OmniPad) nor 8090 (HearBridge) */
#define BB_PORT        8120

#endif /* VERSION_H */
