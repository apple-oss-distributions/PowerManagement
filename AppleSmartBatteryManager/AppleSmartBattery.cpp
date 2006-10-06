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
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <libkern/c++/OSObject.h>
#include "AppleSmartBatteryManager.h"
#include "AppleSmartBattery.h"

// Defines the order of reading properties in the power source state machine
enum {
    kExistingBatteryPath = 0,
    kNewBatteryPath = 1
};

// Three retry attempts on SMBus command failure
enum { 
    kRetryAttempts = 5
};

enum {
    kSecondsUntilValidOnWake    = 30,
    kPostChargeWaitSeconds      = 120,
    kPostDischargeWaitSeconds   = 120
};


enum {
    kDefaultPollInterval = 0,
    kQuickPollInterval = 1
};

// Polling intervals
// The battery kext switches between polling frequencies depending on
// battery load
static uint32_t milliSecPollingTable[2] =
    { 
      30000,    // 0 == Regular 30 second polling
      1000      // 1 == Quick 1 second polling
    };


// Delays to use on subsequent SMBus re-read failures.
// In microseconds.
static const uint32_t microSecDelayTable[kRetryAttempts] = 
    { 10, 100, 1000, 10000, 250000 };
                
                
#define STATUS_ERROR_NEEDS_RETRY(err)                           \
    ( (kIOSMBusStatusDeviceAddressNotAcknowledged == err)       \
   || (kIOSMBusStatusUnknownHostError == err)                   \
   || (kIOSMBusStatusUnknownFailure == err)                     \
   || (kIOSMBusStatusDeviceError == err)                        \
   || (kIOSMBusStatusTimeout == err)                            \
   || (kIOSMBusStatusBusy == err) )


// Keys we use to publish battery state in our IOPMPowerSource::properties array
static const OSSymbol *_MaxErrSym = 
                        OSSymbol::withCString(kIOPMPSMaxErrKey);
static const OSSymbol *_DeviceNameSym = 
                        OSSymbol::withCString(kIOPMDeviceNameKey);
static const OSSymbol *_FullyChargedSym = 
                        OSSymbol::withCString(kIOPMFullyChargedKey);
static const OSSymbol *_AvgTimeToEmptySym = 
                        OSSymbol::withCString("AvgTimeToEmpty");
static const OSSymbol *_AvgTimeToFullSym = 
                        OSSymbol::withCString("AvgTimeToFull");
static const OSSymbol *_ManfDateSym = 
                        OSSymbol::withCString(kIOPMPSManufactureDateKey);
static const OSSymbol *_DesignCapacitySym = 
                        OSSymbol::withCString(kIOPMPSDesignCapacityKey);

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

    fPollingInterval = kDefaultPollInterval;
    fPollingNow = false;
    fCancelPolling = false;

    fInflowDisabled = false;
    
    fWorkLoop = getWorkLoop();
    
    fPollTimer = IOTimerEventSource::timerEventSource( this, 
                    OSMemberFunctionCast( IOTimerEventSource::Action, 
                    this, &AppleSmartBattery::timedOut) );

    if( !fWorkLoop || !fPollTimer
      || (kIOReturnSuccess != fWorkLoop->addEventSource(fPollTimer)) )
    {
        return false;
    }
    
    // Publish the intended period in seconds that our "time remaining"
    // estimate is wildly inaccurate after wake from sleep.
    setProperty( kIOPMPSInvalidWakeSecondsKey, 
                 kSecondsUntilValidOnWake, 32);

    // Publish the necessary time period (in seconds) that a battery
    // calibrating tool must wait to allow the battery to settle after
    // charge and after discharge.
    setProperty( kIOPMPSPostChargeWaitSecondsKey, 
                 kPostChargeWaitSeconds, 32);
    setProperty( kIOPMPSPostDishargeWaitSecondsKey, 
                 kPostDischargeWaitSeconds, 32);
    

    // **** Should occur on workloop
    // zero out battery state with argument (do_update == true)
    clearBatteryState(false);
        
    // **** Should occur on workloop
    BattLog("AppleSmartBattery polling battery data.\n");
    // Kick off the 30 second timer and do an initial poll
    pollBatteryState( kNewBatteryPath );

    return true;
}


/******************************************************************************
 * AppleSmartBattery::setPollingInterval
 *
 ******************************************************************************/
void AppleSmartBattery::setPollingInterval(
    int milliSeconds)
{
    milliSecPollingTable[kDefaultPollInterval] = milliSeconds;
    fPollingInterval = kDefaultPollInterval;
}

/******************************************************************************
 * AppleSmartBattery::pollBatteryState
 *
 * Asynchronously kicks off the register poll.
 ******************************************************************************/

bool AppleSmartBattery::pollBatteryState(int path)
{
    // This must be called under workloop synchronization
    fMachinePath = path;

    /* Start the battery polling state machine in the 0 start state */    
    return transactionCompletion((void *)0, NULL);
}

void AppleSmartBattery::handleBatteryInserted(void)
{
    // This must be called under workloop synchronization
    pollBatteryState( kNewBatteryPath );

    return;
}

void AppleSmartBattery::handleBatteryRemoved(void)
{
    // This must be called under workloop synchronization
    clearBatteryState(true);

    return;
}

void AppleSmartBattery::handleInflowDisabled(bool inflow_state)
{
    fInflowDisabled = inflow_state;
    // And kick off a re-poll using this new information
    pollBatteryState(kExistingBatteryPath);

    return;
}

void AppleSmartBattery::handleChargeInhibited(bool charge_state)
{
    fChargeInhibited = charge_state;
    // And kick off a re-poll using this new information
    pollBatteryState(kExistingBatteryPath);
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
    uint32_t    delay_for = 0;

    char        recv_str[kIOSMBusMaxDataCount+1];

    IOSMBusStatus transaction_status;

    static int retry_attempts = 0;

    static bool fully_discharged = false;
    static bool fully_charged = false;
    static bool batt_present = true;
    static int  ac_connected = -1;
    static int  avg_current = 0;


    if( NULL == transaction ) 
    {
        // NULL argument for transaction means we should start
        // the state machine from scratch. Zero is the start state.
        next_state = 0;
    } else {
        transaction_status = transaction->status;

        BattLog("transaction state = 0x%02x; status = 0x%02x\n; word = %02x.%02x", 
                    next_state, transaction_status,
                    transaction->receiveData[1], transaction->receiveData[0]);

        if( (kIOSMBusStatusOK == transaction_status) 
           && (0 != retry_attempts) )
        {
            // Transaction succeeded after some number of retries            
            BattLog("SmartBattery: retry %d succeeded!\n", retry_attempts);

            retry_attempts = 0;            

        } else if( STATUS_ERROR_NEEDS_RETRY(transaction_status)
                   && (retry_attempts < kRetryAttempts) )
        {
            // The transaction failed. We'll delay by the specified time,
            // then retry the transaction.
            
            delay_for = microSecDelayTable[retry_attempts];
            
            if( 0 != delay_for ) {
                if( delay_for < 1000 ) {
                    // micro
                    IODelay(delay_for);
                } else {
                    // milli
                    IOSleep(delay_for / 1000);            
                }            
            }
            
            retry_attempts++;
            
            BattLog("SmartBattery: failed with 0x%02x; retry attempt %d of %d\n",
                            transaction_status, retry_attempts, kRetryAttempts);

            // Kick off the same transaction that just failed
            readWordAsync(transaction->address, transaction->command);

            return true;
        } else if( kRetryAttempts == retry_attempts ) 
        {
            // Too many consecutive failures to read this entry. Give up, and 
            // go on to attempt a read on the next element in the state machine.
            // ** These two setProperty lines are here purely for debugging. **
            setProperty("LastBattReadError", transaction_status, 16);
            setProperty("LastBattReadErrorCmd", transaction->command, 16);

            BattLog("SmartBattery: Giving up on (0x%02x, 0x%02x) after %d retries.\n",
                transaction->address, transaction->command, retry_attempts);

            retry_attempts = 0;
        }
    }



    switch(next_state)
    {
    
        case 0:
            
            /* Cancel polling timer in case this round of reads was initiated
               by an alarm. We re-set the 30 second poll later. */
            fPollTimer->cancelTimeout();

            readWordAsync(kSMBusManagerAddr, kMStateContCmd);

            break;

        case kMStateContCmd:
            
            // Determines if AC is plugged or unplugged
            // Determines if AC is "charge capable"
            if( kIOSMBusStatusOK == transaction_status ) 
            {
                int new_ac_connected;

                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                // If fInflowDisabled is currently set, then we acknowledge 
                // our lack of AC power.
                //
                // inflow disable means the system is not drawing power from AC.
                //
                // Even with inflow disabled, the AC bit is still true if AC
                // is attached. We zero the bit instead, so that it looks
                // more accurate in BatteryMonitor.
                
                new_ac_connected = ( !fInflowDisabled 
                                && (my_unsigned_16 & kMACPresentBit) ) ? 1:0;


                // Tell IOPMrootDomain on ac connect/disconnect

                IOPMrootDomain *rd = getPMRootDomain();
                if( rd && (new_ac_connected != ac_connected) ) {
                    if(new_ac_connected) {
                        rd->receivePowerNotification( kIOPMSetACAdaptorConnected 
                                                    | kIOPMSetValue );
                    } else {
                        rd->receivePowerNotification(kIOPMSetACAdaptorConnected);
                    }
                }

                ac_connected = new_ac_connected;

                setExternalConnected(ac_connected);
                setExternalChargeCapable(
                        (my_unsigned_16 & kMPowerNotGoodBit) ? false:true);
            } else {
                ac_connected = false;
                setExternalConnected(true);
                setExternalChargeCapable(false);
            }

            readWordAsync(kSMBusManagerAddr, kMStateCmd);
            
            break;
            
        case kMStateCmd:

            // Determines if battery is present
            // Determines if battery is charging
            if( kIOSMBusStatusOK == transaction_status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                                
                batt_present = (my_unsigned_16 & kMPresentBatt_A_Bit) 
                                ? true : false;

                setBatteryInstalled(batt_present);

                // If fChargeInhibit is currently set, then we acknowledge 
                // our lack of charging and force the "isCharging" bit to false.
                //
                // charge inhibit means the battery will not charge, even if
                // AC is attached.
                // Without marking this lack of charging here, it can take
                // up to 30 seconds for the charge disable to be reflected in
                // the UI.

                setIsCharging( !fChargeInhibited
                    &&  (my_unsigned_16 & kMChargingBatt_A_Bit) ? true:false);    
            } else {
                batt_present = false;
                setBatteryInstalled(false);
                setIsCharging(false);
            }
            
            
            /* If the battery is present, we continue with our state machine
               and read battery state below.
               Otherwise, if the battery is not present, we zero out all
               the settings that would have been set in a connected battery. 
            */
            if(!batt_present) {
                // Clean-up battery state for absent battery; do no further
                // battery work until messaged that another battery has
                // arrived.
                
                // zero out battery state with argument (do_update == true)
                clearBatteryState(true);
                return true;
            }


            // The battery read state machine may fork at this stage.
            if(kNewBatteryPath == fMachinePath) {
                /* Following this path reads:
                    manufacturer info; serial number; device name;
                    design capacity; etc.
                    
                   This path re-joins the main path at RemainingCapacity.
                */
                readBlockAsync(kSMBusBatteryAddr, kBManufactureNameCmd);            
            } else {
                /* This path continues reading the normal battery settings
                   that change during regular use.
                   
                   Implies (fMachinePath == kExistingBatteryPath)
                */
                readWordAsync(kSMBusBatteryAddr, kBRemainingCapacityCmd);
            }
            
            break;

/************ Only executed in ReadForNewBatteryPath ****************/
        case kBManufactureNameCmd:
            if( kIOSMBusStatusOK == transaction_status ) 
            {
                if(0 != transaction->receiveDataCount) 
                {    
                    const OSSymbol *manf_sym;
                    
                    bzero(recv_str, sizeof(recv_str));
                    bcopy(transaction->receiveData, recv_str, 
                            transaction->receiveDataCount);

                    manf_sym = OSSymbol::withCString(recv_str);
                    if(manf_sym) {
                        setManufacturer((OSSymbol *)manf_sym);
                        manf_sym->release();
                    }        
                }
            } else {
                properties->removeObject(manufacturerKey);            
            }
            
            readWordAsync(kSMBusBatteryAddr, kBManufactureDateCmd);
        break;


/************ Only executed in ReadForNewBatteryPath ****************/
        case kBManufactureDateCmd:
            if( kIOSMBusStatusOK == transaction_status ) 
            {
                setManufactureDate(
                        (uint32_t)(transaction->receiveData[0]
                                | (transaction->receiveData[1] << 8)));
            } else {
                setManufactureDate(0);
            }
            
            readBlockAsync(kSMBusBatteryAddr, kBDeviceNameCmd);
            break;
            
/************ Only executed in ReadForNewBatteryPath ****************/
        case kBDeviceNameCmd:
            if( kIOSMBusStatusOK == transaction_status ) 
            {
                if(0 != transaction->receiveDataCount) 
                {    
                    const OSSymbol *device_sym;
                    
                    bzero(recv_str, sizeof(recv_str));
                    bcopy(transaction->receiveData, recv_str, 
                            transaction->receiveDataCount);
                    
                    device_sym = OSSymbol::withCString(recv_str);
                    if(device_sym) {
                        setDeviceName((OSSymbol *)device_sym);
                        device_sym->release();
                    }        
                }
            } else {
                properties->removeObject(_DeviceNameSym);
            }


            readWordAsync(kSMBusBatteryAddr, kBSerialNumberCmd);
            
            break;
/************ Only executed in ReadForNewBatteryPath ****************/
        case kBSerialNumberCmd:
            if( kIOSMBusStatusOK == transaction_status ) 
            {
                const OSSymbol *serialSym;
        
                // IOPMPowerSource expects an OSSymbol for serial number, so we
                // sprint this 16-bit number into an OSSymbol
                
                bzero(recv_str, sizeof(recv_str));
                snprintf(recv_str, sizeof(recv_str), "%d", 
                    ( transaction->receiveData[0] 
                    | (transaction->receiveData[1] << 8) ));
                serialSym = OSSymbol::withCString(recv_str);
                if(serialSym) {
                    setSerial( (OSSymbol *) serialSym);
                    serialSym->release();        
                }
            } else {
                properties->removeObject(serialKey);
            }
            
            readWordAsync(kSMBusBatteryAddr, kBDesignCapacityCmd);
            break;

/************ Only executed in ReadForNewBatteryPath ****************/
        case kBDesignCapacityCmd:
            if( kIOSMBusStatusOK == transaction_status ) 
            {
                OSNumber    *design_cap;
                design_cap = OSNumber::withNumber(
                        (uint32_t)(transaction->receiveData[0] 
                                | (transaction->receiveData[1] << 8)),  32);
                if(design_cap) {
                    properties->setObject(_DesignCapacitySym, design_cap);
                    design_cap->release();
                }
            } else {
                OSNumber    *zero_num = OSNumber::withNumber((long long unsigned int)0, 32);
                if(zero_num) {
                    properties->setObject(_DesignCapacitySym, zero_num);
                    zero_num->release();
                }
            }
            
            readWordAsync(kSMBusBatteryAddr, kBRemainingCapacityCmd);
            break;
            
/* ========== Back to our regularly scheduled battery reads ==========
   The "new battery" reads re-join all battery regular battery reads here */
        case kBRemainingCapacityCmd:
        
            if( kIOSMBusStatusOK == transaction_status )
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];
                                
                fRemainingCapacity = my_unsigned_16;
                
                setCurrentCapacity( (unsigned int)my_unsigned_16 );
            } else {
                setCurrentCapacity(0);
            }

            readWordAsync(kSMBusBatteryAddr, kBFullChargeCapacityCmd);
        
            break;

        case kBFullChargeCapacityCmd:

            if( kIOSMBusStatusOK == transaction_status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                fFullChargeCapacity = my_unsigned_16;

                setMaxCapacity( my_unsigned_16 );

                if( fFullChargeCapacity )
                {
                    /*
                     * Conditionally set polling interval to 1 second if we're
                     *     discharging && below 5% && on AC power
                     * i.e. we're doing an Inflow Disabled discharge
                     */
                    if( (((100*fRemainingCapacity) / fFullChargeCapacity ) < 5) 
                        && ac_connected )
                    {
                        setProperty("Quick Poll", true);
                        fPollingInterval = kQuickPollInterval;
                    } else {
                        setProperty("Quick Poll", false);
                        fPollingInterval = kDefaultPollInterval;
                    }
                }
            } else {
                setMaxCapacity(0);
            }

            readWordAsync(kSMBusBatteryAddr, kBAverageCurrentCmd);

            break;
            
        case kBAverageCurrentCmd:
        
            time_command = 0;
            
            if( kIOSMBusStatusOK == transaction_status ) 
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

            if( kIOSMBusStatusOK == transaction_status ) 
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

            if( kIOSMBusStatusOK == transaction_status ) 
            {
                my_unsigned_16 = (transaction->receiveData[1] << 8)
                                | transaction->receiveData[0];

                if ( my_unsigned_16 & kBFullyChargedStatusBit) {
                    fully_charged = true;
                } else {
                    fully_charged = false;
                }
                
                if ( my_unsigned_16 & kBFullyDischargedStatusBit)
                {
                    if(!fully_discharged) {
                        fully_discharged = true;
    
                        // Immediately cancel AC Inflow disable
                        fProvider->handleFullDischarge();
                    }
                } else {
                    fully_discharged = false;
                }
                
            } else {
                fully_charged = false;
            }
            
            setFullyCharged(fully_charged);

            readWordAsync(kSMBusBatteryAddr, kBMaxErrorCmd);

            break;
            
        case kBMaxErrorCmd:

            if( kIOSMBusStatusOK == transaction_status ) 
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
        
            if( kIOSMBusStatusOK == transaction_status ) 
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

            if( kIOSMBusStatusOK == transaction_status ) 
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

            if( kIOSMBusStatusOK == transaction_status ) 
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

            if( kIOSMBusStatusOK == transaction_status ) 
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
                fPollTimer->setTimeoutMS( milliSecPollingTable[fPollingInterval] );
                BattLog("SmartBattery: new timeout scheduled in %d msec\n",
                                fPollingInterval);
            } else {
                // We'll let the polling timer expire.
                // Right now we're plugged into AC, we'll start the timer again
                // when we get an alarm on AC unplug.
                BattLog("SmartBattery: letting timeout expire.\n");
            }

            break;

        default:
            BattLog("SmartBattery: Error state %d not expected\n", next_state);
    }

    return true;
}

void AppleSmartBattery::clearBatteryState(bool do_update)
{
    // Only clear out battery state; don't clear manager state like AC Power.
    // We just zero out the int and bool values, but remove the OSType values.
    
    setBatteryInstalled(false);
    setIsCharging(false);
    setCurrentCapacity(0);
    setMaxCapacity(0);
    setTimeRemaining(0);
    setAmperage(0);
    setVoltage(0);
    setCycleCount(0);
    setAdapterInfo(0);
    setLocation(0);
    
    properties->removeObject(manufacturerKey);
    properties->removeObject(serialKey);
    properties->removeObject(batteryInfoKey);
    
    rebuildLegacyIOBatteryInfo();

    if(do_update) {
        updateStatus();
    }
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
        pollBatteryState( kExistingBatteryPath );
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
    legacyDict->setObject(kIOBatteryCycleCountKey, properties->getObject(kIOPMPSCycleCountKey));
    
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
        properties->setObject(_ManfDateSym, n);
        n->release();
    }
}

int     AppleSmartBattery::manufactureDate(void)
{
    OSNumber    *n = OSDynamicCast(OSNumber, properties->getObject(_ManfDateSym));
    if(n) {
        return n->unsigned32BitValue();
    } else {
        return 0;
    }
}


/******************************************************************************
 ******************************************************************************
 **
 **  Async SmartBattery read convenience functions
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

    ret = fProvider->performTransaction(
                    &fTransaction,
                    OSMemberFunctionCast( IOSMBusTransactionCompletion,
                      this, &AppleSmartBattery::transactionCompletion),
                    (OSObject *)this,
                    (void *)cmd);

    return ret;
}

IOReturn AppleSmartBattery::readBlockAsync(
    uint8_t address,
    uint8_t cmd
) {
    IOReturn                ret = kIOReturnError;
    bzero(&fTransaction, sizeof(IOSMBusTransaction));

    // All transactions are performed async
    fTransaction.protocol      = kIOSMBusProtocolReadBlock;
    fTransaction.address       = address;
    fTransaction.command       = cmd;

    ret = fProvider->performTransaction(
                    &fTransaction,
                    OSMemberFunctionCast( IOSMBusTransactionCompletion,
                      this, &AppleSmartBattery::transactionCompletion),
                    (OSObject *)this,
                    (void *)cmd);

    return ret;
}


