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

#define super IOService


OSDefineMetaClassAndStructors(AppleSmartBatteryManager, IOService)

bool AppleSmartBatteryManager::start(IOService *provider)
{
    bool        ret_bool;

    if(!super::start(provider)) {
        return false;
    }

    fProvider = OSDynamicCast(IOSMBusController, provider);
    if(!fProvider) {
        return false;
    }
        
    fBattery = AppleSmartBattery::smartBattery();

    if(!fBattery) return false;
    
    ret_bool = fBattery->attach(this);

    ret_bool = fBattery->start(this);
    
    fBattery->registerService(0);

    return true;
}

IOReturn AppleSmartBatteryManager::performTransaction(IOSMBusTransaction * transaction,
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


IOReturn AppleSmartBatteryManager::message( 
    UInt32 type, 
    IOService *provider,
    void *argument )
{
    IOSMBusAlarmMessage     *alarm = (IOSMBusAlarmMessage *)argument;

    /* On SMBus alarms from the System Battery Manager, trigger a new
       poll of battery state.   */

    if(!alarm) return kIOReturnSuccess;
    
    if( (kIOMessageSMBusAlarm == type) 
        && (kSMBusManagerAddr == alarm->fromAddress))
    {
        fBattery->pollBatteryState();
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

    IOLog("%s", buf);
    
    return;
#endif
}

