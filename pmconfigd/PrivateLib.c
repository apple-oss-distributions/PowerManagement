/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCValidation.h>
#include <IOKit/IOReturn.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <syslog.h>
#include "PrivateLib.h"

enum
{
    PowerMangerScheduledShutdown = 1,
    PowerMangerScheduledSleep
};

#define kPowerManagerActionNotificationName "com.apple.powermanager.action"
#define kPowerManagerActionKey "action"
#define kPowerManagerValueKey "value"

// Tracks system battery state
static int batCount = 0;
static IOPMBattery **batteries = NULL;

/******
 * Do not remove DUMMY macros
 *
 * The following DUMMY_* macros aren't used in the source code, but they're
 * here as a dummy code for our localization pre-processor scripts to see the strings
 * we're using in CFCopyLocalizedStringWithDefaultValue. Several localization
 * tools analyze the arguments to these calls to auto-generate loc files.
 ******/

#define DUMMY_UPS_HEADER(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("WARNING!"), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("Warning!"), \
            NULL);
            
#define DUMMY_UPS_BODY(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("YOUR COMPUTER IS NOW RUNNING ON UPS BACKUP BATTERY. SAVE YOUR DOCUMENTS AND SHUTDOWN SOON."), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("Your computer is now running on UPS backup battery. Save your documents and shutdown soon."), \
            NULL);

#define DUMMY_BATT_HEADER(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("YOU ARE NOW RUNNING ON RESERVE BATTERY POWER."), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("You are now running on reserve battery power."), \
            NULL);

#define DUMMY_BATT_BODY(myBundle) CFCopyLocalizedStringWithDefaultValue( \
            CFSTR("PLEASE CONNECT YOUR COMPUTER TO AC POWER. IF YOU DO NOT, YOUR COMPUTER WILL GO TO SLEEP IN A FEW MINUTES TO PRESERVE THE CONTENTS OF MEMORY."), \
            CFSTR("Localizable"), \
            myBundle, \
            CFSTR("Please connect your computer to AC power. If you do not, your computer will go to sleep in a few minutes to preserve the contents of memory."), \
            NULL);


__private_extern__ IOReturn 
_setRootDomainProperty(
    CFStringRef                 key, 
    CFTypeRef                   val) 
{
    io_iterator_t               it;
    io_registry_entry_t         root_domain;
    IOReturn                    ret;

    IOServiceGetMatchingServices(
                    MACH_PORT_NULL, 
                    IOServiceNameMatching("IOPMrootDomain"), 
                    &it);
    if(!it) return kIOReturnError;
    root_domain = (io_registry_entry_t)IOIteratorNext(it);
    if(!root_domain) return kIOReturnError;
 
    ret = IORegistryEntrySetCFProperty(root_domain, key, val);

    IOObjectRelease(root_domain);
    IOObjectRelease(it);
    return ret;
}


static void sendNotification(int command)
{
    CFMutableDictionaryRef   dict = NULL;
    int numberOfSeconds = 600;
    
    CFNumberRef secondsValue = CFNumberCreate( NULL, kCFNumberIntType, &numberOfSeconds );
    CFNumberRef commandValue = CFNumberCreate( NULL, kCFNumberIntType, &command );

    dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, 
                                    &kCFTypeDictionaryKeyCallBacks, 
                                    &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(dict, CFSTR(kPowerManagerActionKey), commandValue);
    CFDictionarySetValue(dict, CFSTR(kPowerManagerValueKey), secondsValue);

    CFNotificationCenterPostNotificationWithOptions ( CFNotificationCenterGetDistributedCenter(),
                                            CFSTR(kPowerManagerActionNotificationName), 
                                            NULL, dict, 
                                            (kCFNotificationPostToAllSessions | kCFNotificationDeliverImmediately));
    CFRelease(dict);
    CFRelease(secondsValue);
    CFRelease(commandValue);
}


__private_extern__ void _askNicelyThenShutdownSystem(void)
{
    sendNotification(PowerMangerScheduledShutdown);
}

__private_extern__ void _askNicelyThenSleepSystem(void)
{
    sendNotification(PowerMangerScheduledSleep);
}

// Accessor for internal battery structs
__private_extern__ IOPMBattery **_batteries(void)
{
    return batteries;
}

__private_extern__ bool _batterySupports(
    io_registry_entry_t which, 
    CFStringRef what)
{
    int         i = 0;
    bool        found = false;
    
    for(i=0; i<batCount; i++) 
    {
        if(which != batteries[i]->me) continue;
        if(CFDictionaryGetValue(batteries[i]->properties, what)) 
        {
            found = true;
            break;
        }
    }
    
    return found;
}

static void _unpackBatteryState(IOPMBattery *b, CFDictionaryRef prop)    
{
    CFBooleanRef    boo;
    CFNumberRef     n;
    
    if(!isA_CFDictionary(prop)) return;
    
    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSExternalConnectedKey));
    b->externalConnected = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSExternalChargeCapableKey));
    b->externalChargeCapable = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSBatteryInstalledKey));
    b->isPresent = (kCFBooleanTrue == boo);

    boo = CFDictionaryGetValue(prop, CFSTR(kIOPMPSIsChargingKey));
    b->isCharging = (kCFBooleanTrue == boo);

    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCurrentCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->currentCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSMaxCapacityKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->maxCap);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSTimeRemainingKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->hwReportedTR);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSAmperageKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->amperage);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSCycleCountKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->cycleCount);
    }
    n = CFDictionaryGetValue(prop, CFSTR(kIOPMPSLocationKey));
    if(n) {
        CFNumberGetValue(n, kCFNumberIntType, &b->location);
    }

    return;
}

__private_extern__ int  _batteryCount(void)
{
    return batCount;
}

__private_extern__ IOPMBattery *_newBatteryFound(io_registry_entry_t where)
{
    int             new_battery_index = batCount++;
    IOPMBattery     *new_battery = NULL;

    batteries = (IOPMBattery **)realloc(batteries, 
                                    batCount*sizeof(IOPMBattery *));
    new_battery = calloc(1, sizeof(IOPMBattery));
    batteries[new_battery_index] = new_battery;
    
    new_battery->me = where;

    _batteryChanged(new_battery);

    return new_battery;
}


__private_extern__ void _batteryChanged(IOPMBattery *changed_battery)
{
    kern_return_t       kr;
    
    if(0 == batCount) return;
    
    if(!changed_battery) { 
        // This is unexpected; we're not tracking this battery
        return;
    }
    
    // Free the last set of properties
    if(changed_battery->properties) { 
        CFRelease(changed_battery->properties);
        changed_battery->properties = NULL;
    }
    
    kr = IORegistryEntryCreateCFProperties(
                            changed_battery->me, 
                            &(changed_battery->properties),
                            kCFAllocatorDefault, 0);
    if(KERN_SUCCESS != kr) {
        changed_battery->properties = NULL;
        goto exit;
    }

    _unpackBatteryState(changed_battery, changed_battery->properties);    
exit:
    return;
}

// Returns 10.0 - 10.4 style IOPMCopyBatteryInfo dictionary, when possible.
__private_extern__ CFArrayRef _copyLegacyBatteryInfo(void) 
{
    CFArrayRef          battery_info = NULL;
    IOReturn            ret;
    
    // PMCopyBatteryInfo
    ret = IOPMCopyBatteryInfo(MACH_PORT_NULL, &battery_info);
    if(ret != kIOReturnSuccess || !battery_info)
    {
        return NULL;
    }
    
    return battery_info;
}

__private_extern__ CFUserNotificationRef _showUPSWarning(void)
{
#ifndef STANDALONE
    CFMutableDictionaryRef      alert_dict;
    SInt32                      error;
    CFUserNotificationRef       note_ref;
    CFBundleRef                 myBundle;
    CFStringRef                 header_unlocalized;
    CFStringRef                 message_unlocalized;
    CFURLRef                    bundle_url;

    myBundle = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.SystemConfiguration.PowerManagement"));

    // Create alert dictionary
    alert_dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, 
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if(!alert_dict) return NULL;

    bundle_url = CFBundleCopyBundleURL(myBundle);
    CFDictionarySetValue(alert_dict, kCFUserNotificationLocalizationURLKey, bundle_url);
    CFRelease(bundle_url);

    header_unlocalized = CFSTR("WARNING!");
    message_unlocalized = CFSTR("YOUR COMPUTER IS NOW RUNNING ON UPS BACKUP BATTERY. SAVE YOUR DOCUMENTS AND SHUTDOWN SOON.");

    CFDictionaryAddValue(alert_dict, kCFUserNotificationAlertHeaderKey, header_unlocalized);
    CFDictionaryAddValue(alert_dict, kCFUserNotificationAlertMessageKey, message_unlocalized);
    
    note_ref = CFUserNotificationCreate(kCFAllocatorDefault, 0, 0, &error, alert_dict);
    CFRelease(alert_dict);
    if(0 != error)
    {
        syslog(LOG_INFO, "pmcfgd: UPS warning error = %d\n", error);
        return NULL;    
    }

    return note_ref;
#endif // STANDALONE    
}
/*
__private_extern__ CFUserNotificationRef _showLowBatteryWarning(void)
{

// _showLowBatteryWarning is a no-op until
// we resolve the TalkingAlerts issue. Need this to generate a speakable alert,
// but the plumbing involved in doing that from configd is complicated.
// For the time being, BatteryMonitor will continue to issue this alert.
    CFMutableDictionaryRef      alert_dict;
    SInt32                      error;
    CFUserNotificationRef       note_ref;
    CFBundleRef                 myBundle;
    CFURLRef                    low_batt_image_URL;    
    CFURLRef                    bundle_url;
    CFStringRef                 header_unlocalized;
    CFStringRef                 message_unlocalized;
    
    myBundle = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.SystemConfiguration.PowerManagement"));
    
    // Create alert dictionary
    alert_dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, 
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if(!alert_dict) return NULL;
    
    bundle_url = CFBundleCopyBundleURL(myBundle);
    CFDictionarySetValue(alert_dict, kCFUserNotificationLocalizationURLKey, bundle_url);
    CFRelease(bundle_url);

    
    low_batt_image_URL = CFBundleCopyResourceURL(myBundle, CFSTR("low-batt"), CFSTR("icns"), 0);
    if(low_batt_image_URL) 
    {
        CFShow(low_batt_image_URL);
        CFDictionaryAddValue(alert_dict, kCFUserNotificationIconURLKey, low_batt_image_URL);
        CFRelease(low_batt_image_URL);
    }
    
    header_unlocalized = CFSTR("YOU ARE NOW RUNNING ON RESERVE BATTERY POWER.");
    message_unlocalized = CFSTR("PLEASE CONNECT YOUR COMPUTER TO AC POWER. IF YOU DO NOT, YOUR COMPUTER WILL GO TO SLEEP IN A FEW MINUTES TO PRESERVE THE CONTENTS OF MEMORY.");
    
    CFDictionaryAddValue(alert_dict, kCFUserNotificationAlertHeaderKey, header_unlocalized);
    CFDictionaryAddValue(alert_dict, kCFUserNotificationAlertMessageKey, message_unlocalized);
    
    note_ref = CFUserNotificationCreate(kCFAllocatorDefault, 0, 0, &error, alert_dict);
    CFRelease(alert_dict);
    if(0 != error)
    {
        syslog(LOG_INFO, "PowerManagement: battery warning error = %d\n", error);
        return NULL;    
    }

    return note_ref;

}
*/
