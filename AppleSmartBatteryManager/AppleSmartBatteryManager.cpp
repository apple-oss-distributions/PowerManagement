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

#include "AppleSmartBatteryManager.h"
#include "AppleSmartBattery.h"


// Power states!
enum {
    kMyOnPowerState = 1
};

static IOPMPowerState myTwoStates[2] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};


#define super IOService

OSDefineMetaClassAndStructors(AppleSmartBatteryManager, IOService)

bool AppleSmartBatteryManager::start(IOService *provider)
{
    bool        ret_bool;
    IOCommandGate * gate;
    IOWorkLoop *    wl;

    if(!super::start(provider)) {
        return false;
    }

    fProvider = OSDynamicCast(IOSMBusController, provider);
    if(!fProvider) {
        return false;
    }

    wl = getWorkLoop();
    if (!wl) {
        return false;
    }

    // Join power management so that we can get a notification early during
    // wakeup to re-sample our battery data. We don't actually power manage
    // any devices.
    PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);
        
    fBattery = AppleSmartBattery::smartBattery();

    if(!fBattery) return false;
    
    ret_bool = fBattery->attach(this);

    ret_bool = fBattery->start(this);
    
    gate = IOCommandGate::commandGate(fBattery);
    if (!gate) {
        return false;
    }
    wl->addEventSource(gate);
    fGate = gate;      // enable messages

    fBattery->registerService(0);

    return true;
}

IOReturn AppleSmartBatteryManager::performTransaction(
    IOSMBusTransaction * transaction,
    IOSMBusTransactionCompletion completion,
    OSObject * target,
    void * reference)
{
    /* directly pass bus transactions back up to SMBusController */
    return fProvider->performTransaction(transaction,
                completion,
                target,
                reference);
}

IOReturn AppleSmartBatteryManager::setPowerState(
    unsigned long which, 
    IOService *whom)
{
    if( (kMyOnPowerState == which) 
        && fGate )
    {
        // We are waking from sleep - kick off a battery read to make sure
        // our battery concept is in line with reality.
        fGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                           fBattery, &AppleSmartBattery::pollBatteryState),
                           (void *)1, NULL, NULL, NULL); // kNewBatteryPath = 1
    }
    return IOPMAckImplied;
}


IOReturn AppleSmartBatteryManager::message( 
    UInt32 type, 
    IOService *provider,
    void *argument )
{
    IOSMBusAlarmMessage     *alarm = (IOSMBusAlarmMessage *)argument;
    static uint16_t         last_data = 0;
    uint16_t                changed_bits = 0;
    uint16_t                data = 0;

    /* On SMBus alarms from the System Battery Manager, trigger a new
       poll of battery state.   */

    if(!alarm) return kIOReturnSuccess;
    
    if( (kIOMessageSMBusAlarm == type) 
        && (kSMBusManagerAddr == alarm->fromAddress)
        && fGate)
    {
        data = (uint16_t)(alarm->data[0] | (alarm->data[1] << 8));
        changed_bits = data ^ last_data;
        last_data = data;

        if(changed_bits & kMPresentBatt_A_Bit)
        {
            if(data & kMPresentBatt_A_Bit) {
                // Battery inserted
                fGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                               fBattery, &AppleSmartBattery::handleBatteryInserted),
                               NULL, NULL, NULL, NULL);
            } else {
                // Battery removed
                fGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                               fBattery, &AppleSmartBattery::handleBatteryRemoved),
                               NULL, NULL, NULL, NULL);
            }
        } else {
            // Just an alarm; re-read battery state.
            fGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                               fBattery, &AppleSmartBattery::pollBatteryState),
                               NULL, NULL, NULL, NULL);
        }
    }

    return kIOReturnSuccess;
}

void BattLog(char *fmt, ...)
{
#if 0
    va_list     listp;
    char        buf[128];

    va_start(listp, fmt);
    vsnprintf(buf, sizeof(buf), fmt, listp);
    va_end(listp);

    kprintf("BattLog: %s", buf);
    
    return;
#endif
}

