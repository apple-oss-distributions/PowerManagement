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

#include <IOKit/pwr_mgt/RootDomain.h>

#include "AppleSmartBatteryManager.h"
#include "AppleSmartBattery.h"


// Power states!
enum {
    kMyOnPowerState = 1
};

// Commands!
enum {
    kInhibitChargingCmd = 0,
    kDisableInflowCmd
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
    
    const OSSymbol *ucClassName = 
            OSSymbol::withCStringNoCopy("AppleSmartBatteryManagerUserClient");
    setProperty(gIOUserClientClassKey, (OSObject *) ucClassName);
    ucClassName->release();
    

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

    // Command gate for SmartBatteryManager
    fManagerGate = IOCommandGate::commandGate(this);
    if (!fManagerGate) {
        return false;
    }
    wl->addEventSource(fManagerGate);
    
    // Command gate for SmartBattery
    gate = IOCommandGate::commandGate(fBattery);
    if (!gate) {
        return false;
    }
    wl->addEventSource(gate);
    fBatteryGate = gate;      // enable messages

    fBattery->registerService(0);

    this->registerService(0);

    return true;
}

// Default polling interval is 30 seconds
IOReturn AppleSmartBatteryManager::setPollingInterval(
    int milliSeconds)
{
    // Discard any negatize or zero arguments
    if(milliSeconds <= 0) return kIOReturnBadArgument;
    
    if(fBattery)
        fBattery->setPollingInterval(milliSeconds);

    setProperty("PollingInterval_msec", milliSeconds, 32);

    return kIOReturnSuccess;
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
        && fBatteryGate )
    {
        // We are waking from sleep - kick off a battery read to make sure
        // our battery concept is in line with reality.
        fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
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
        && fBatteryGate)
    {
        data = (uint16_t)(alarm->data[0] | (alarm->data[1] << 8));
        changed_bits = data ^ last_data;
        last_data = data;

        if(changed_bits & kMPresentBatt_A_Bit)
        {
            if(data & kMPresentBatt_A_Bit) {
                // Battery inserted
                fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                               fBattery, &AppleSmartBattery::handleBatteryInserted),
                               NULL, NULL, NULL, NULL);
            } else {
                // Battery removed
                fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                               fBattery, &AppleSmartBattery::handleBatteryRemoved),
                               NULL, NULL, NULL, NULL);
            }
        } else {
            // Just an alarm; re-read battery state.
            fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                               fBattery, &AppleSmartBattery::pollBatteryState),
                               NULL, NULL, NULL, NULL);
        }
    }

    return kIOReturnSuccess;
}

/* 
 * inhibitCharging
 * 
 * Called by AppleSmartBatteryManagerUserClient
 */
IOReturn AppleSmartBatteryManager::inhibitCharging(int level)
{
    IOReturn        ret = kIOReturnSuccess;
    
    if(!fManagerGate) return kIOReturnInternalError;
    
    this->setProperty("Charging Inhibited", level ? true : false);
    
    fManagerGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                       this, &AppleSmartBatteryManager::gatedSendCommand),
                       (void *)kInhibitChargingCmd, (void *)level, 
                       (void *)&ret, NULL);

    return ret;
}

/* 
 * disableInflow
 * 
 * Called by AppleSmartBatteryManagerUserClient
 */
IOReturn AppleSmartBatteryManager::disableInflow(int level)
{
    IOReturn        ret = kIOReturnSuccess;

    if(!fManagerGate) return kIOReturnInternalError;

    this->setProperty("Inflow Disabled", level ? true : false);
    
    fManagerGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                       this, &AppleSmartBatteryManager::gatedSendCommand),
                       (void *)kDisableInflowCmd, (void *)level, 
                       (void *)&ret, NULL);
    
    return ret;
}

/* 
 * handleFullDischarge
 * 
 * Called by AppleSmartBattery
 * If inflow is disabled, by the user client, we re-enable it and send a message
 * via IOPMrootDomain indicating that inflow has been re-enabled.
 */
void AppleSmartBatteryManager::handleFullDischarge(void)
{
    IOPMrootDomain      *root_domain = getPMRootDomain();
    IOReturn            ret;
    void *              messageArgument = NULL;

    if( getProperty("Inflow Disabled") )
    {
        messageArgument = (void *)kInflowForciblyEnabledBit;

        /* 
         * Send disable inflow command to SB Manager
         */
        fManagerGate->runAction(OSMemberFunctionCast(IOCommandGate::Action,
                           this, &AppleSmartBatteryManager::gatedSendCommand),
                           (void *)kDisableInflowCmd, (void *)0, /* OFF */ 
                           (void *)&ret, NULL);        
    }


    /* 
     * Message user space clients that battery is fully discharged.
     * If appropriate, set kInflowForciblyEnabledBit
     */
    if(root_domain) {
        root_domain->messageClients( 
                kIOPMMessageInternalBatteryFullyDischarged, 
                messageArgument );
    }
}


void AppleSmartBatteryManager::gatedSendCommand(
    int cmd, 
    int level, 
    IOReturn *ret_code)
{
    *ret_code = kIOReturnError;
    bzero(&fTransaction, sizeof(IOSMBusTransaction));

    fTransaction.protocol      = kIOSMBusProtocolWriteWord;
    fTransaction.sendDataCount = 2;
    
    /*
     * kDisableInflowCmd
     */
    if( kDisableInflowCmd == cmd) 
    {
        fTransaction.address            = kSMBusManagerAddr;
        fTransaction.command            = kMStateContCmd;
        if( 0 != level ) {
            fTransaction.sendData[0]    = kMCalibrateBit;
        } else {
            fTransaction.sendData[0]    = 0x0;
        }
    }

    /*
     * kInhibitChargingCmd
     */
    if( kInhibitChargingCmd == cmd) 
    {
        fTransaction.address            = kSMBusChargerAddr;
        fTransaction.command            = kBChargingCurrentCmd;
        if( 0 != level ) {
            // non-zero level for chargeinhibit means turn off charging.
            // We set charge current to 0.
            fTransaction.sendData[0]    = 0x0;
        } else {
            // We re-enable charging by setting it to 6000, a signifcantly
            // large number (> 4500 per Chris C) to ensure charging will resume.
            fTransaction.sendData[0]    = 0x70;
            fTransaction.sendData[1]    = 0x17;
        }
    }

    *ret_code = fProvider->performTransaction(
                    &fTransaction,
                    OSMemberFunctionCast( IOSMBusTransactionCompletion,
                      this, &AppleSmartBatteryManager::transactionCompletion),
                    (OSObject *)this);
                    
exit:
    return;
}

bool AppleSmartBatteryManager::transactionCompletion(
    void *ref, 
    IOSMBusTransaction *transaction)
{
    if( kIOSMBusStatusOK == transaction->status )
    {
        // If we just completed sending the DisableInflow command, give a 
        // callout to our attached battery and let them know whether its
        // enabled or disabled.
        if( (kMStateContCmd == transaction->command)
            && (kSMBusManagerAddr == transaction->address) )
        {
            fBattery->handleInflowDisabled( 
                        transaction->sendData[0] ? true : false);        
        }

        // If we just completed sending the ChargeInhibit command, give a 
        // callout to our attached battery and let them know whether its
        // enabled or disabled.
        if( (kBChargingCurrentCmd == transaction->command)
            && (kSMBusChargerAddr == transaction->address) )
        {
            fBattery->handleChargeInhibited( 
                        (transaction->sendData[0] | transaction->sendData[1]) 
                        ? false : true);
        }       
        
    } else {
        BattLog("AppleSmartBatteryManager::transactionCompletion:"\
                                    "ERROR 0x%08x!\n", transaction->status);
    }
    return false;
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

