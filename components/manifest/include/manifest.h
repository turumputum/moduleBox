// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#ifndef __MANIFEST_H__
#define __MANIFEST_H__

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------


const char *        get_manifest_audioPlayer    ();
const char *        get_manifest_audioLAN       ();
const char *        get_manifest_adc1           ();
const char *        get_manifest_analog         ();
const char *        get_manifest_button_led      ();
const char *        get_manifest_button_ledBar   ();
const char *        get_manifest_button_ledRing  ();
const char *        get_manifest_button_smartLed ();
const char *        get_manifest_button_swiperLed();
const char *        get_manifest_3n_mosfet      ();
const char *        get_manifest_encoders       ();
// const char *        get_manifest_smartLed       ();
const char *        get_manifest_virtual_slots  ();
// const char *        get_manifest_st7789_display ();


int                 saveManifesto               ();


#endif // #define __MANIFEST_H__
