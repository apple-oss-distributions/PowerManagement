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

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSObject.h>
#include "AppleSmartBatteryManager.h"
#include "AppleSmartBattery.h"

// The interval at which, when charging or discharging the battery, we poll
// for updates in the absence of any alarms or alerts.
#define kPOLLING_INTERVAL        30000

// Keys we use to publish battery state in our IOPMPowerSource::properties array
static const OSSymbol *_MaxErrSym = OSSymbol::withCString("MaxErr");
static const OSSymbol *_DeviceNameSym = OSSymbol::withCString("DeviceName");
static const OSSymbol *_FullyChargedSym = OSSymbol::withCString("FullyCharged");
static const OSSymbol *_AvgTimeToEmptySym = OSSymbol::withCString("AvgTimeToEmpty");
static const OSSymbol *_AvgTimeToFullSym = OSSymbol::withCString("AvgTimeToFull");
static const OSSymbol *_ManfDate = OSSymbol::withCString("ManufactureDate");

#define super IOPMPowerSource
OSDefineMetaClassAndStructors(AppleSmartBattery,IOPMPowerSource)

/******************************************************************************
 * AppleSmartBattery::smartBattery
 *     
 ******************************************************************************/

AppleSmartBattery * 
AppleSmartBattery::smartBattery(void)
{
    AppleSmartBattery  *me;
    me = new AppleSmartBattery;
    
    if(me && !me->init()) {
        me->release();
        return NULL;
    }

    return me;
}


/******************************************************************************
 * AppleSmartBattery::init
 *
 ******************************************************************************/

bool AppleSmartBattery::init(void) 
{
    if(!super::init()) {
        return false;
    }

    fProvider = NULL;
    fWorkLoop = NULL;
    fPollTimer = NULL;

    return true;
}


/******************************************************************************
 * AppleSmartBattery::start
 *
 ******************************************************************************/

bool AppleSmartBattery::start(IOService *provider)
{
    IOReturn        err;

    BattLog("AppleSmartBattery loading...\n");
    
    fProvider = OSDynamicCast(AppleSmartBatteryManager, provider);

    if(!fProvider || !super::start(provider)) {
        return false;
    }

    fPollingNow = false;
    fCancelPolling = false;
    
    fWorkLoop = getWorkLoop();
    
    fPollTimer = IOTimerEventSource::timerEventSource( this, 
                    OSMemberFunctionCast( IOTimerEventSource::Action, 
                    this, &AppleSmartBattery::timedOut) );

    if( !fWorkLoop || !fPollTimer
      || (kIOReturnSuccess != fWorkLoop->addEventSource(fPollTimer)) )
    {
        BattLog("IOWorkLoop::addEventSource 0x%08x-> ERROR = 0x%08x\n", fWorkLoop, err);
        return false;
    }
BattLog("AppleSmartBattery running one time setup.\n");

    // perform one-time SMBus battery transactions - read manufacturer,
    // serial number, and write the mAh capacity mode bit
    oneTimeBatterySetup();
    
BattLog("AppleSmartBattery polling battery data.\n");
    // Kick off the 30 second timer and do an initial poll
    pollBatteryState();

    return true;
}

/******************************************************************************
 * AppleSmartBattery::oneTimeBatterySetup
 *
 * Run once at startup time, does boring battery setup.
 ******************************************************************************/

void AppleSmartBattery::oneTimeBatterySetup()
{
    IOSMBusTransaction          transaction;    
    IOReturn                    ret;
    char                        recv_str[kIOSMBusMaxDataCount+1];

    //******************************************************
    // Set Capacity Mode bit 
    // so battery reports in units of mAh rather than mWh 
    //******************************************************
    
    bzero(&transaction, sizeof(IOSMBusTransaction));

    transaction.protocol      = kIOSMBusProtocolReadWord;
    transaction.address       = kSMBusBatteryAddr;
    transaction.command       = kBBatteryModeCmd;
    transaction.options       = 0;

    ret = fProvider->performTransaction(&transaction);
    
    if( (kIOReturnSuccess == ret) && (kIOSMBusStatusOK == transaction.status) ) 
    {

        //******************************************************
        // Put battery into mAh reporting mode
        //******************************************************
        
        uint16_t    mode_flags =  transaction.receiveData[0]
                              | (transaction.receiveData[1] << 8);

        mode_flags |= kBCapacityModeBit;

        bzero(&transaction, sizeof(IOSMBusTransaction));
        
        transaction.protocol      = kIOSMBusProtocolWriteWord;
        transaction.address       = kSMBusBatteryAddr;
        transaction.command       = kBBatteryModeCmd;
        transaction.options       = 0;
        transaction.sendDataCount = 2;
        transaction.sendData[0]  = (uint8_t)(mode_flags & 0xFF);
        transaction.sendData[1]  = (uint8_t)((mode_flags >> 8) & 0xFF);

        ret = fProvider->performTransaction(&transaction);
    }


    //******************************************************
    // Get Manufacturer
    //******************************************************
    bzero(&transaction, sizeof(IOSMBusTransaction));
    transaction.protocol      = kIOSMBusProtocolReadBlock;
    transaction.address       = kSMBusBatteryAddr;
    transaction.command       = kBBatteryModeCmd;
    transaction.options       = 0;

    ret = fProvider->performTransaction(&transaction);
    
    if( (kIOReturnSuccess == ret) && (kIOSMBusStatusOK == transaction.status) ) 
    {        
        if(0 != transaction.receiveDataCount) 
        {    
            const OSSymbol *manf_sym;
            
            bzero(recv_str, sizeof(recv_str));
            bcopy(transaction.receiveData, recv_str, transaction.receiveDataCount);
            
            manf_sym = OSSymbol::withCString(recv_str);
            if(manf_sym) {
                setManufacturer((OSSymbol *)manf_sym);
                manf_sym->release();
            }        
        }
    }


    //******************************************************
    // Get DeviceName
    //******************************************************
    bzero(&transaction, sizeof(IOSMBusTransaction));
    transaction.protocol      = kIOSMBusProtocolReadBlock;
    transaction.address       = kSMBusBatteryAddr;
    transaction.command       = kBDeviceNameCmd;
    transaction.options       = 0;

    ret = fProvider->performTransaction(&transaction);
    
    if( (kIOReturnSuccess == ret) && (kIOSMBusStatusOK == transaction.status) ) 
    {        
        if(0 != transaction.receiveDataCount) 
        {    
            const OSSymbol *device_sym;
            
            bzero(recv_str, sizeof(recv_str));
            bcopy(transaction.receiveData, recv_str, transaction.receiveDataCount);
            
            device_sym = OSSymbol::withCString(recv_str);
            if(device_sym) {
                setDeviceName((OSSymbol *)device_sym);
                device_sym->release();
            }        
        }
    }
        
        
    //******************************************************
    // Get Manufacture Date
    //******************************************************
    bzero(&transaction, sizeof(IOSMBusTransaction));
    transaction.protocol      = kIOSMBusProtocolReadWord;
    transaction.address       = kSMBusBatteryAddr;
    transaction.command       = kBManufactureDateCmd;
    transaction.options       = 0;

    ret = fProvider->performTransaction(&transaction);
    
    if( (kIOReturnSuccess == ret) && (kIOSMBusStatusOK == transaction.status) ) 
    {        
        setManufactureDate((uint32_t)(transaction.receiveData[0]
                               | (transaction.receiveData[1] << 8)));
    }


    //******************************************************
    // Get Serial No
    //******************************************************
    bzero(&transaction, sizeof(IOSMBusTransaction));
    transaction.protocol      = kIOSMBusProtocolReadWord;
    transaction.address       = kSMBusBatteryAddr;
    transaction.command       = kBSerialNumberCmd;
    transaction.options       = 0;

    ret = fProvider->performTransaction(&transaction);
    
    if( (kIOReturnSuccess == ret) && (kIOSMBusStatusOK == transaction.status) ) 
    {
        const OSSymbol *serialSym;

        // IOPMPowerSource expects an OSSymbol for serial number, so we
        // sprint this 16-bit number into an OSSymbol
        
        bzero(recv_str, sizeof(recv_str));
        snprintf(recv_str, sizeof(recv_str), "%d", 
            ( transaction.receiveData[0] | (transaction.receiveData[1] << 8) ));
        serialSym = OSSymbol::withCString(recv_str);
        if(serialSym) {
            setSerial( (OSSymbol *) serialSym);
            serialSym->release();        
        }
        
    }


}

/******************************************************************************
 * AppleSmartBattery::pollBatteryState
 *
 * Asynchronously kicks off the register poll.
 ******************************************************************************/

bool AppleSmartBattery::pollBatteryState(void)
{
    /* Start the battery polling state machine in the 0 start state */    
    return transactionCompletion((void *)0, NULL);
}

/******************************************************************************
 * AppleSmartBattery::transactionCompletion
 * -> Runs in workloop context
 *
 ******************************************************************************/
 
bool AppleSmartBattery::transactionCompletion(
    void *ref, 
    IOSMBusTransaction *transaction)
{
    int         next_state = (int)ref;
    int16_t     my_signed_16;
    uint16_t    my_unsigned_16;
    uint8_t     time_command = 0;

    static bool fully_charged = false;
    static bool batt_present = true;
    static bool ac_connected = false;
    static int  avg_current = 0;


    if(transaction) {
        BattLog("SB::transactionComplete next_state = %d status = 0x%02x\n", 
                    next_state, transaction->status);
        BattLog("   data[0] = %x data[1] = %x\n", 
                    transaction->receiveData[0], transaction->receiveData[1]);
    }

    // Zero is start state
    if( NULL == transaction ) next_state = 0;

    switch(next_state)
    {
    
        case 0:
            
            /* Cancel polling timer in case this round of reads was initiated
               by an alarm. We re-set the 30 second poll later. */
            fPollTimer->cancelTimeout();

            readWordAsync(kSMBusManagerAddr, kMStateCmd);

            break;
            
        case kMStateCmd:

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                                
                batt_present = (my_unsigned_16 & kMPresentBatt_A_Bit) ? true:false;

                setBatteryInstalled(batt_present);

                setIsCharging((my_unsigned_16 & kMChargingBatt_A_Bit) ? true:false);    
            } else {
                batt_present = false;
                setBatteryInstalled(false);
                setIsCharging(false);
            }
            
            readWordAsync(kSMBusManagerAddr, kMStateContCmd);

            break;

        case kMStateContCmd:
            
            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                ac_connected = (my_unsigned_16 & kMACPresentBit) ? true:false;
                setExternalConnected(ac_connected);
                BattLog("kMStateContCmd = 0x%02x, ac_connected = %d\n", 
                                my_unsigned_16, ac_connected);

                setExternalChargeCapable(
                    (my_unsigned_16 & kMPowerNotGoodBit) ? false:true);
            } else {
                ac_connected = false;
                setExternalConnected(true);
                setExternalChargeCapable(false);
            }
            
            readWordAsync(kSMBusBatteryAddr, kBRemainingCapacityCmd);
        
            break;

        case kBRemainingCapacityCmd:
        
            if( kIOSMBusStatusOK == transaction->status )
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                setCurrentCapacity( (unsigned int)my_unsigned_16 );
            } else {
                setCurrentCapacity(0);
            }

            readWordAsync(kSMBusBatteryAddr, kBFullChargeCapacityCmd);
        
            break;

        case kBFullChargeCapacityCmd:

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                setMaxCapacity( my_unsigned_16 );
            } else {
                setMaxCapacity(0);
            }

            readWordAsync(kSMBusBatteryAddr, kBAverageCurrentCmd);

            break;
            
        case kBAverageCurrentCmd:
        
            time_command = 0;
            
            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_signed_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                setAmperage( my_signed_16 );
                avg_current = my_signed_16;
                
            } else {
                // Battery not present, or general error
                avg_current = 0;
                setAmperage(0);
                setTimeRemaining(0);
            }
        
            readWordAsync(kSMBusBatteryAddr, kBVoltageCmd);

            break;
            
        case kBVoltageCmd:

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                setVoltage( my_unsigned_16 );
            } else {
                setVoltage(0);
            }

            readWordAsync(kSMBusBatteryAddr, kBBatteryStatusCmd);

            break;
            
        case kBBatteryStatusCmd:

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                if ( my_unsigned_16 & kBFullyChargedStatusBit) {
                    fully_charged = true;
                } else {
                    fully_charged = false;
                }
            } else {
                fully_charged = false;
            }
            
            setFullyCharged(fully_charged);

            readWordAsync(kSMBusBatteryAddr, kBMaxErrorCmd);

            break;
            
        case kBMaxErrorCmd:

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                setMaxErr( my_unsigned_16 );
            } else {
                setMaxErr(0);
            }

            readWordAsync(kSMBusBatteryAddr, kBCycleCountCmd);

            break;
            
        case kBCycleCountCmd:
        
            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                setCycleCount( my_unsigned_16 );
            } else {
                setCycleCount(0);
            }
        
            readWordAsync(kSMBusBatteryAddr, kBAverageTimeToEmptyCmd);

            break;

        case kBAverageTimeToEmptyCmd:

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                setAverageTimeToEmpty( my_unsigned_16 );
                
                if(avg_current < 0) {
                    setTimeRemaining( my_unsigned_16 );
                }                               
            } else {
                setTimeRemaining(0);
                setAverageTimeToEmpty(0);
            }
        
            readWordAsync(kSMBusBatteryAddr, kBAverageTimeToFullCmd);

            break;

            
        case kBAverageTimeToFullCmd:

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                setAverageTimeToFull( my_unsigned_16 );

                if(avg_current > 0) {
                    setTimeRemaining( my_unsigned_16 );
                }                               
            } else {
                setTimeRemaining(0);
                setAverageTimeToFull(0);
            }

            readWordAsync(kSMBusBatteryAddr, kBCurrentCmd);

            break;

        case kBCurrentCmd:
        
            int16_t     signed_16;

            if( kIOSMBusStatusOK == transaction->status ) 
            {
                signed_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                setProperty((const char *)"RealCurrent", 
                                (long long unsigned int)signed_16, 
                                (unsigned int)16);
            } else {
                setProperty((const char *)"RealCurrent", 
                                (long long unsigned int)0, 
                                (unsigned int)16);
            }

            rebuildLegacyIOBatteryInfo();
            
            updateStatus();
            
            fPollingNow = false;

            // Re-arm 30 second timer only if the batteries are 
            // not fully charged. No need to poll when fully charged.
            if(  ( !ac_connected )
              || ( !fully_charged && batt_present ) ) {
                fPollTimer->setTimeoutMS(kPOLLING_INTERVAL);
                BattLog("SmartBattery: new timeout scheduled in 30 seconds\n");
            } else {
                // We'll let the polling timer expire.
                // Right now we're plugged into AC, we'll start the timer again
                // when we get an alarm on AC unplug.
                BattLog("SmartBattery: battery is fully charged; letting timeout expire.\n");
            }

            break;

        default:
            BattLog("SmartBattery: Error state %d not expected\n", next_state);
    }

    return true;
}

/******************************************************************************
 *  timer expiration handler
 ******************************************************************************/

void AppleSmartBattery::timedOut(void)
{
    // Timer will be re-enabled from the battery polling routine.
    // Timer will not be kicked off again if battery is plugged in and
    // fully charged.
    if( !fPollingNow )
        pollBatteryState();    
}


/******************************************************************************
 *  Package battery data in "legacy battery info" format, readable by
 *  any applications using the not-so-friendly IOPMCopyBatteryInfo()
 ******************************************************************************/
 
 void AppleSmartBattery::rebuildLegacyIOBatteryInfo(void)
 {
    OSDictionary        *legacyDict = OSDictionary::withCapacity(5);
    uint32_t            flags = 0;
    OSNumber            *flags_num = NULL;
    
    if(externalConnected()) flags |= kIOPMACInstalled;
    if(batteryInstalled()) flags |= kIOPMBatteryInstalled;
    if(isCharging()) flags |= kIOPMBatteryCharging;
    
    flags_num = OSNumber::withNumber((unsigned long long)flags, 32);
    legacyDict->setObject(kIOBatteryFlagsKey, flags_num);
    flags_num->release();

    legacyDict->setObject(kIOBatteryCurrentChargeKey, properties->getObject(kIOPMPSCurrentCapacityKey));
    legacyDict->setObject(kIOBatteryCapacityKey, properties->getObject(kIOPMPSMaxCapacityKey));
    legacyDict->setObject(kIOBatteryVoltageKey, properties->getObject(kIOPMPSVoltageKey));
    legacyDict->setObject(kIOBatteryAmperageKey, properties->getObject(kIOPMPSAmperageKey));
    
    setLegacyIOBatteryInfo(legacyDict);
    
    legacyDict->release();
}

/******************************************************************************
 *  New value accessors
 ******************************************************************************/

void AppleSmartBattery::setMaxErr(int error)
{
    OSNumber    *n = OSNumber::withNumber(error, 32);
    if(n) {
        properties->setObject(_MaxErrSym, n);
        n->release();
    }
}

int AppleSmartBattery::maxErr(void)
{
    OSNumber    *n = OSDynamicCast(OSNumber, properties->getObject(_MaxErrSym));
    if(n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}


void AppleSmartBattery::setDeviceName(OSSymbol *sym)
{
    if(sym) {
        properties->setObject(_DeviceNameSym, (OSObject *)sym);
    }
}

OSSymbol * AppleSmartBattery::deviceName(void)
{
    return OSDynamicCast(OSSymbol, properties->getObject(_DeviceNameSym));
}


void    AppleSmartBattery::setFullyCharged(bool charged)
{
    properties->setObject(
                    _FullyChargedSym, 
                    (charged ? kOSBooleanTrue:kOSBooleanFalse) );
}

bool    AppleSmartBattery::fullyCharged(void) 
{
    return (kOSBooleanTrue == properties->getObject(_FullyChargedSym));
}


void    AppleSmartBattery::setAverageTimeToEmpty(int seconds)
{
    OSNumber    *n = OSNumber::withNumber(seconds, 32);
    if(n) {
        properties->setObject(_AvgTimeToEmptySym, n);
        n->release();
    }
}

int     AppleSmartBattery::averageTimeToEmpty(void)
{
    OSNumber    *n = OSDynamicCast(OSNumber, properties->getObject(_AvgTimeToEmptySym));
    if(n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}

void    AppleSmartBattery::setAverageTimeToFull(int seconds)
{
    OSNumber    *n = OSNumber::withNumber(seconds, 32);
    if(n) {
        properties->setObject(_AvgTimeToFullSym, n);
        n->release();
    }
}

int     AppleSmartBattery::averageTimeToFull(void)
{
    OSNumber    *n = OSDynamicCast(OSNumber, properties->getObject(_AvgTimeToFullSym));
    if(n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}


void    AppleSmartBattery::setManufactureDate(int date)
{
    OSNumber    *n = OSNumber::withNumber(date, 32);
    if(n) {
        properties->setObject(_ManfDate, n);
        n->release();
    }
}

int     AppleSmartBattery::manufactureDate(void)
{
    OSNumber    *n = OSDynamicCast(OSNumber, properties->getObject(_ManfDate));
    if(n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}


/******************************************************************************
 ******************************************************************************
 **
 **  Smart Battery read/write convenience function
 **
 ******************************************************************************
 ******************************************************************************/

IOReturn AppleSmartBattery::readWordAsync(
    uint8_t address,
    uint8_t cmd
) {
    IOReturn                ret = kIOReturnError;
    bzero(&fTransaction, sizeof(IOSMBusTransaction));

    // All transactions are performed async
    fTransaction.protocol      = kIOSMBusProtocolReadWord;
    fTransaction.address       = address;
    fTransaction.command       = cmd;
    fTransaction.options       = 0;
    fTransaction.sendDataCount = 0;

    ret = fProvider->performTransaction(
                    &fTransaction,
                    OSMemberFunctionCast( IOSMBusTransactionCompletion,
                                          this,
                                          &AppleSmartBattery::transactionCompletion),
                    (OSObject *)this,
                    (void *)cmd);

    return ret;
}



