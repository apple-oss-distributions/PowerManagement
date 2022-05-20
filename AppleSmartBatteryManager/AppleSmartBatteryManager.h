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

#ifndef __AppleSmartBatteryManager__
#define __AppleSmartBatteryManager__

#include <IOKit/IOService.h>

#define TARGET_OS_OSX_X86   (TARGET_OS_OSX && !TARGET_CPU_ARM64)    // Non-Apple Silicon Mac platforms
#define TARGET_OS_OSX_AS    (TARGET_OS_OSX && TARGET_CPU_ARM64)     // Apple Silicon Mac platforms

#if TARGET_OS_OSX_X86
#include <IOKit/smbus/IOSMBusController.h>
#include "SmbusHandler.h"
#else
#include <IOKit/smc/AppleSMCFamily.h>
#endif

#include <os/log.h>

#include "AppleSmartBattery.h"
#include "AppleSmartBatteryManagerUserClient.h"

class AppleSmartBattery;
class AppleSmartBatteryManagerUserClient;
#if TARGET_OS_OSX_X86
class SmbusHandler;
#endif
class AppleSMC;

extern uint32_t gBMDebugFlags;
extern bool gDebugAllowed;
enum {
    BM_LOG_LEVEL0 = 0x00000001,     // basic logging for errors
    BM_LOG_LEVEL1 = 0x00000002,     // basic logging
    BM_LOG_LEVEL2 = 0x00000004,     // verbose logging
};

#define BM_LOG1(fmt, args...)           \
{                                       \
    if (gDebugAllowed && (gBMDebugFlags & BM_LOG_LEVEL1))  \
        os_log(OS_LOG_DEFAULT, fmt, ## args); \
}

#define BM_LOG2(fmt, args...)           \
{                                       \
    if (gDebugAllowed && (gBMDebugFlags & BM_LOG_LEVEL2))  \
        os_log(OS_LOG_DEFAULT, fmt, ## args); \
}

#define BM_ERRLOG(fmt, args...)         \
{                                       \
    if (gBMDebugFlags & BM_LOG_LEVEL0)  \
        os_log(OS_LOG_DEFAULT, fmt, ## args); \
}

enum {
    kRetryAttempts = 5,
    kInitialPollCountdown = 5,
    kIncompleteReadRetryMax = 10
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class AppleSmartBatteryManager : public IOService {
    friend class AppleSmartBatteryManagerUserClient;
#if TARGET_OS_OSX_X86
    friend class SmbusHandler;
#endif

    OSDeclareDefaultStructors(AppleSmartBatteryManager)

public:
    bool init(void) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

    IOReturn setPowerState(unsigned long which, IOService *whom) APPLE_KEXT_OVERRIDE;

    IOReturn message(UInt32 type, IOService *provider, void * argument) APPLE_KEXT_OVERRIDE;
    virtual IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;

    // Called by AppleSmartBattery
    // Re-enables AC inflow if appropriate
    void handleFullDischarge(void);

    // bool argument true means "set", false means "clear" exclusive acces
    // return: false means "exclusive access already granted", "true" means success
    //
    // TODO: do we have clients on iOS that need exclusive access? Need a wrapper for this?
    bool requestExclusiveSMBusAccess(bool request);

    // Returns true if an exclusive AppleSmartBatteryUserClient is attached. False otherwise.
    bool hasExclusiveClient(void);

    bool requestPoll(int type);
    bool isSystemSleeping();
    bool exclusiveClientExists();
    bool isBatteryInaccessible();

    // transactionCompletion is the guts of the state machine
#if TARGET_OS_OSX_X86
    bool    transactionCompletion(void *ref, IOSMBusTransaction *transaction);
    IOReturn performTransaction(ASBMgrRequest *req, OSObject * target, void * reference);
#endif
    IOReturn inhibitChargingGated(uint64_t level);
    IOReturn disableInflowGated(uint64_t level);
    bool smbusSupported();
    
private:
    // Called by AppleSmartBatteryManagerUserClient
    IOReturn inhibitCharging(int level);

    // Called by AppleSmartBatteryManagerUserClient
    IOReturn disableInflow(int level);

    // Called by AppleSmartBatteryManagerUserClient
    // Called by Battery Updater application
    IOReturn performExternalTransaction(
                        void            *in,    // struct EXSMBUSInputStruct
                        void            *out,   // struct EXSMBUSOutputStruct
                        IOByteCount     inSize,
                        IOByteCount     *outSize);

    void    gatedSendCommand(int cmd, int level, IOReturn *ret_code);

    IOReturn setOverrideCapacity(uint16_t level);
    IOReturn switchToTrueCapacity(void);
    void    handleBatteryInserted(void);
    void    handleBatteryRemoved(void);

    IOReturn smbusCompletionHandler(void *ref, IOReturn status, size_t byteCount, uint8_t *data);
    IOReturn requestExclusiveSMBusAccessGated(bool request);

    bool                        _started;
#if TARGET_OS_OSX_X86
    IOSMBusController           * fProvider;
    SmbusHandler                * fSmbus;
    ASBMgrTransactionCompletion fAsbmCompletion;
    OSObject                    *fAsbmTarget;
    void                        *fAsbmReference;
    IOReturn                    performSmbusTransactionGated(ASBMgrRequest *req, OSObject *target, void *ref);
#endif
#if TARGET_OS_IPHONE || TARGET_OS_OSX_AS
    IOTimerEventSource          * fBatteryPollSMC;
    AppleSMCFamily              * fProvider;
#endif
    IOCommandGate               * fManagerGate;
    AppleSmartBattery           * fBattery;
    bool                        fExclusiveUserClient;
    bool                        fSystemSleeping;
    bool                        fInacessible;
    IOWorkLoop                  *fWorkLoop;
    bool                        fSmbusSupport;

#if TARGET_OS_BRIDGE || TARGET_OS_OSX_AS
public:
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
#endif
};

#endif
