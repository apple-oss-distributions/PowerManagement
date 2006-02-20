/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef __AppleSmartBattery__
#define __AppleSmartBattery__

#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/smbus/IOSMBusController.h>

#include "AppleSmartBatteryCommands.h"
#include "AppleSmartBatteryManager.h"

class AppleSmartBatteryManager;

class AppleSmartBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(AppleSmartBattery)
    
protected:
    AppleSmartBatteryManager    *fProvider;
    IOWorkLoop                  *fWorkLoop;
    IOTimerEventSource          *fPollTimer;
    bool                        fCancelPolling;
    bool                        fPollingNow;
    IOSMBusTransaction          fTransaction;

    // Accessor for MaxError reading
    // Percent error in MaxCapacity reading
    void    setMaxErr(int error);
    int     maxErr(void);

    // SmartBattery reports a device name
    void    setDeviceName(OSSymbol *sym);
    OSSymbol *deviceName(void);

    // Set when battery is fully charged;
    // Clear when battery starts discharging/AC is removed
    void    setFullyCharged(bool);
    bool    fullyCharged(void);

    void    setAverageTimeToEmpty(int seconds);
    int     averageTimeToEmpty(void);

    void    setAverageTimeToFull(int seconds);
    int     averageTimeToFull(void);
    
    void    setManufactureDate(int date);
    int     manufactureDate(void);

    void    oneTimeBatterySetup(void);
    
public:
    static AppleSmartBattery *smartBattery(void);

    virtual bool init(void);

    virtual bool start(IOService *provider);

    bool    pollBatteryState(void);

private:

    void    timedOut(void);

    void    rebuildLegacyIOBatteryInfo(void);

    bool    transactionCompletion(void *ref, IOSMBusTransaction *transaction);

    IOReturn AppleSmartBattery::readWordAsync(
                        uint8_t address,
                        uint8_t cmd);
};

#endif
