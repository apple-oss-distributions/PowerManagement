/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
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
/*
 * Copyright (c) 2002 Apple Computer, Inc.  All rights reserved. 
 *
 * HISTORY
 *
 * 29-Aug-02 ebold created
 *
 */
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/IOMessage.h>
#include <syslog.h>

#include "BatteryTimeRemaining.h"
#include "PrivateLib.h"

/**** PMBattery configd plugin
  We clean up, massage, and re-package the data from the batteries and publish
  it in the more palatable form described in IOKit/Headers/IOPowerSource.h

  All kernel batteries conform to the IOPMPowerSource base class.
    
  We provide the following information in a CFDictionary and publish it for
  all user processes to see:
    Name
    CurrentCapacity
    MaxCapacity
    Remaining Time To Empty
    Remaining Time To Full Charge
    IsCharging
    IsPresent
    Type    
****/


// Return values from calculateTRWithCurrent
enum {
    kNothingToSeeHere = 0,
    kNoTimeEstimate,
};
    
    
// static global variables for tracking battery state
static int              _batCount;
static int              _impendingSleep = 0;
static CFAbsoluteTime   _lastWake = 0.0;
static CFStringRef      *_batName;
static CFStringRef      *batteryDynamicStore;


// forward declarations
static void     _initializeBatteryCalculations(void);
static int      _calculateTRWithCurrent(void);
static void     _packageBatteryInfo(int, CFDictionaryRef *);
static void     _timeRemainingMaybeValid(CFRunLoopTimerRef timer, void *info);


__private_extern__ void
BatteryTimeRemaining_prime(void)
{
    // setup battery calculation global variables
    _initializeBatteryCalculations();
    return;
}

__private_extern__ void
BatteryTimeRemainingSleepWakeNotification(natural_t messageType)
{
    CFRunLoopTimerRef               timer_rls = NULL;
    CFAbsoluteTime                  fire_date;
    IOPMBattery                   **b;
    
    switch ( messageType ) {

    case kIOMessageSystemWillSleep:
        // System is going to sleep - reset time remaining calculations.
        // Battery drain during sleep will produce an unrealistic time remaining
        // expectation on wake from sleep unless we reset the average sample.
        _impendingSleep = 1;

        break;

    case kIOMessageSystemWillPowerOn:

        b = _batteries();
        if(b && b[0]) {

            // [4422606] Delay for 1 second before grabbing wakeup time; on an
            // MP system our code may be running before the clock resync code
            // has had a chance to finish on the other processor. We  wait
            // and get a correct read before we call CFAbsoluteTimeGetCurrent()
            sleep(1);
    
            // Start invalid data timer; when it fires we assume battery data
            // has had time to re-adjust after wake from sleep, and is now valid
    
            // In the meantime, any attempt to publish time remaining in 
            // BatteriesHaveChanged will check the current time against
            // _lastWake + b[i]->invalidWakeSecs
    
            _lastWake = CFAbsoluteTimeGetCurrent();
        
            fire_date = _lastWake + (double)b[0]->invalidWakeSecs;
        
            timer_rls = CFRunLoopTimerCreate(
                            NULL, fire_date, 0.0, 
                            0, 0, _timeRemainingMaybeValid, NULL);
            CFRunLoopAddTimer( CFRunLoopGetCurrent(), timer_rls, 
                            kCFRunLoopDefaultMode);
            CFRelease(timer_rls);
        }
        break;
        
    case kIOMessageSystemHasPoweredOn:
        _impendingSleep = 0;
        break;
    }
}

static void _timeRemainingMaybeValid(CFRunLoopTimerRef timer, void *info)
{
    // Trigger battery time remaining re-calculation now that current reading
    // is valid.
    BatteryTimeRemainingBatteriesHaveChanged(NULL);
}

static void     _initializeBatteryCalculations(void)
{
    int                 i;  
        
    // Batteries detected, get their initial state
    _batCount = _batteryCount();
    if (_batCount == 0) {
        return;
    }

    _batName = (CFStringRef *)calloc(1, _batCount * sizeof(CFStringRef));
    batteryDynamicStore = (CFStringRef *)calloc(1, _batCount * sizeof(CFStringRef));

    
    for(i = 0;i < _batCount;i++){
        _batName[i] = CFStringCreateWithFormat(
                            kCFAllocatorDefault, 
                            NULL, 
                            CFSTR("InternalBattery-%d"), 
                            i);

        // Initialize SCDynamicStore battery key name
        batteryDynamicStore[i] = SCDynamicStoreKeyCreate(
                            kCFAllocatorDefault, 
                            CFSTR("%@%@/InternalBattery-%d"),
                            kSCDynamicStoreDomainState, 
                            CFSTR(kIOPSDynamicStorePath), 
                            i);
    }

    // make initial call to populate array and publish state
    BatteryTimeRemainingBatteriesHaveChanged(_batteries());

    return;
}


__private_extern__ void
BatteryTimeRemainingBatteriesHaveChanged(IOPMBattery **battery_info)
{
    CFDictionaryRef             *result = NULL;
    int                         i;
    int                         invalid_time_remaining = 0;
    int                         calculation_return = kNothingToSeeHere;
    static SCDynamicStoreRef    store = NULL;
    static CFDictionaryRef      *old_battery;

    _batCount = _batteryCount();
    
    if(!battery_info) battery_info = _batteries();

    if ( NULL == old_battery ) {
        old_battery = (CFDictionaryRef *) calloc(1, _batCount * sizeof(CFDictionaryRef));
        if ( NULL == old_battery ) {
            // Uh-oh.
            syslog(LOG_INFO, "Power management: Failed to allocate old_battery in %s\n", __func__);
            return;
        }
    }

    result = (CFDictionaryRef *) calloc(1, _batCount * sizeof(CFDictionaryRef));
    if ( NULL == result ) {
        // This isn't good.
        syslog(LOG_INFO, "Power management: Failed to allocate result in %s\n", __func__);
        return;
    }
    
    // Calculate time remaining using current
    calculation_return = _calculateTRWithCurrent();
    
    if( (kNoTimeEstimate == calculation_return)    
       || ( battery_info && (_batCount > 0) &&
          ( (CFAbsoluteTimeGetCurrent() - _lastWake)    // too soon since last
           < (double)battery_info[0]->invalidWakeSecs) ) )   // wakeup?
    {
        invalid_time_remaining = 1;
    }
    
    // At this point our algorithm above has populated the time remaining estimate
    // We'll package that info into user-consumable dictionaries below.

    _packageBatteryInfo(invalid_time_remaining, result);

    // Publish the results of calculation in the SCDynamicStore
    if(!store) store = SCDynamicStoreCreate(
                                kCFAllocatorDefault, 
                                CFSTR("PM configd plugin"), 
                                NULL, 
                                NULL);
    for(i=0; i<_batCount; i++) {
        if(result[i]) {   
            // Determine if CFDictionary is new or has changed...
            // Only do SCDynamicStoreSetValue if the dictionary is different
            if(!old_battery[i]) {
                SCDynamicStoreSetValue(store, batteryDynamicStore[i], result[i]);
            } else {
                if(!CFEqual(old_battery[i], result[i])) {
                    SCDynamicStoreSetValue(store, batteryDynamicStore[i], result[i]);
                }
                CFRelease(old_battery[i]);
            }
            old_battery[i] = result[i];
        }
    }
    
    if(result) free(result);
}


/* _calculateTRWithCurrent
 * Implicit inputs: global battery state variables
 * Implicit output: values placed in global hoursRemaining[] variable
 *   returns true if we reached a valid estimate
 *   returns false if we're still calculating
 */
int _calculateTRWithCurrent(void)
{
    int             ret_val = kNothingToSeeHere;
    int             i;
    IOPMBattery     *b;
    IOPMBattery     **batts = _batteries();
    
    for(i=0; i<_batCount; i++)
    {
        b = batts[i];
        
        // If current is zero, finding a time remaining estimate is irrelevant
        // (in the case of being fully charged) or impossible (in the case
        // of having just plugged into AC and the PMU is thinking.
        // Allowing for some slop in either direction of zero.
        // While operating, current should be roughly in the 1,000mA-2,000mA
        // range. We allow for 5mA slop here.
        if( (b->amperage < 5) &&
            (b->amperage > -5) )
        {
            b->swCalculatedTR = 0;
            ret_val = kNoTimeEstimate;
            continue;
        }
        
        if(b->isCharging)
        {
            // h = -mAh/mA
            b->swCalculatedTR = 60*((double)(b->maxCap - b->currentCap)
                                / (double)b->amperage);
        } else { // discharging
            // h = mAh/mA
            b->swCalculatedTR = -60*((double)b->currentCap
                                / (double)b->amperage);
        }
    }
    return ret_val;
}


/* 
 * Implicit argument: All the global variables that track battery state
 */
void _packageBatteryInfo(int stillCalc, CFDictionaryRef *ret)
{
    CFNumberRef         n, n0, nneg1;
    CFMutableDictionaryRef  mutDict = NULL;
    int             i;
    int             temp;
    int             minutes;
    int             set_capacity, set_charge;
    IOPMBattery     *b;
    IOPMBattery     **batts = _batteries();

    // Stuff battery info into CFDictionaries
    for(i=0; i<_batCount; i++) {
        b = batts[i];

        // Create the battery info dictionary
        mutDict = NULL;
        mutDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if(!mutDict) return;
        
        // Set transport type to "Internal"
        CFDictionarySetValue(mutDict, 
                        CFSTR(kIOPSTransportTypeKey), 
                        CFSTR(kIOPSInternalType));

        // Set Power Source State to AC/Battery
        CFDictionarySetValue(mutDict, 
                        CFSTR(kIOPSPowerSourceStateKey), 
                        (b->externalConnected ? CFSTR(kIOPSACPowerValue):
                                                CFSTR(kIOPSBatteryPowerValue)));
                                                
        // round charge and capacity down to a % scale
        if(0 != b->maxCap)
        {
            set_capacity = 100;
            set_charge = (int)lround((double)b->currentCap*100.0/(double)b->maxCap);

            if( (100 == set_charge) && b->isCharging)
            {
                // We will artificially cap the percentage to 99% while charging
                // Batteries may take 10-20 min beyond 100% of charging to
                // relearn their absolute maximum capacity. Leave cap at 99%
                // to indicate we're not done charging. (4482296, 3285870)
                set_charge = 99;
            }
        } else {
            // Bad battery or bad reading => 0 capacity
            set_capacity = set_charge = 0;
        }

        // Set maximum capacity
        n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &set_capacity);
        if(n) {
            CFDictionarySetValue(mutDict, CFSTR(kIOPSMaxCapacityKey), n);
            CFRelease(n);
        }
        
        // Set current charge
        n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &set_charge);
        if(n) {
            CFDictionarySetValue(mutDict, CFSTR(kIOPSCurrentCapacityKey), n);
            CFRelease(n);
        }
        
        // Set isPresent flag
        CFDictionarySetValue(mutDict, CFSTR(kIOPSIsPresentKey), 
                    b->isPresent ? kCFBooleanTrue:kCFBooleanFalse);
        
        // Set _isCharging and time remaining
        minutes = b->swCalculatedTR;
        temp = 0;
        n0 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &temp);
        temp = -1;
        nneg1 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &temp);
        if( !b->isPresent ) {
            // remaining time calculations only have meaning if the battery is present
            CFDictionarySetValue(mutDict, CFSTR(kIOPSIsChargingKey), kCFBooleanFalse);
            CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToFullChargeKey), n0);
            CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToEmptyKey), n0);
        } else {
            // A battery is installed
            if(stillCalc) {
                // If we are still calculating then our time remaining
                // numbers aren't valid yet. Stuff with -1.
                CFDictionarySetValue(mutDict, CFSTR(kIOPSIsChargingKey), 
                        b->isCharging ? kCFBooleanTrue : kCFBooleanFalse);
                CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToFullChargeKey), nneg1);
                CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToEmptyKey), nneg1);
            } else {   
                // else there IS a battery installed, and remaining time 
                // calculation makes sense.
                if(b->isCharging) {
                    // Set _isCharging to True
                    CFDictionarySetValue(mutDict, CFSTR(kIOPSIsChargingKey), kCFBooleanTrue);
                    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &minutes);
                    if(n) {
                        CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToFullChargeKey), n);
                        CFRelease(n);
                    }
                    CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToEmptyKey), n0);
                } else {
                    // Not Charging
                    // Set _isCharging to False
                    CFDictionarySetValue(mutDict, CFSTR(kIOPSIsChargingKey), kCFBooleanFalse);
                    // But are we plugged in?
                    if(b->externalConnected)
                    {
                        // plugged in but not charging == fully charged
                        CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToFullChargeKey), n0);
                        CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToEmptyKey), n0);
                    } else {
                        // not charging, not plugged in == d_isCharging
                        n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &minutes);
                        if(n) {
                            CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToEmptyKey), n);
                            CFRelease(n);
                        }
                        CFDictionarySetValue(mutDict, CFSTR(kIOPSTimeToFullChargeKey), n0);
                    }
                }
            }
        }
        CFRelease(n0);
        CFRelease(nneg1);

        // Set name
        CFDictionarySetValue(mutDict, CFSTR(kIOPSNameKey), _batName[i]);
        ret[i] = mutDict;
    }

    return;
}
