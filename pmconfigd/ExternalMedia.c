/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>

#include "ExternalMedia.h"
#include "PMAssertions.h"
#include "PrivateLib.h"


#define _kExternalMediaAssertionName 	"com.apple.powermanagement.externalmediamounted"

static dispatch_queue_t  extMediaQ;

/*****************************************************************************/


static bool weLikeTheDisk(DADiskRef disk);
static void adjustExternalDiskAssertion(void);
static void _DiskDisappeared(DADiskRef disk, void *context);
static void _DiskAppeared(DADiskRef disk, void * context);

/*****************************************************************************/

static CFMutableSetRef      gExternalMediaSet = NULL;
static IOPMAssertionID      gDiskAssertionID = kIOPMNullAssertionID;
static DASessionRef         gDASession = NULL;

/*****************************************************************************/


__private_extern__ void ExternalMedia_prime(void)
{    
    extMediaQ = dispatch_queue_create("com.apple.powermanagent.extMediaQ", DISPATCH_QUEUE_SERIAL);

    dispatch_async(extMediaQ, ^{
        gExternalMediaSet = CFSetCreateMutable(0, 0, &kCFTypeSetCallBacks);
        
        if (!gExternalMediaSet)
            return;
        
        gDASession = DASessionCreate(0);
        
        DARegisterDiskAppearedCallback(gDASession, kDADiskDescriptionMatchVolumeMountable, _DiskAppeared, NULL);
        
        DARegisterDiskDisappearedCallback(gDASession, kDADiskDescriptionMatchVolumeMountable, _DiskDisappeared, NULL);
        
        DASessionSetDispatchQueue(gDASession, extMediaQ);
    });
    
}

/*****************************************************************************/

static void _DiskDisappeared(DADiskRef disk, void *context)
{
    if (weLikeTheDisk(disk))
    {		
        CFSetRemoveValue(gExternalMediaSet, disk);
        
        adjustExternalDiskAssertion();
    }
}

/*****************************************************************************/

static void _DiskAppeared(DADiskRef disk, void * context)
{
    if (weLikeTheDisk(disk))
    {		
        CFSetSetValue(gExternalMediaSet, disk);
        
        adjustExternalDiskAssertion();
    }
}


/*****************************************************************************/

static bool weLikeTheDisk(DADiskRef disk)
{
    CFDictionaryRef     description = NULL;
    CFStringRef         protocol = NULL;
    bool                ret = false;

    /*
    We will create an ExternalMedia assertion if any of these disks are present.
    That will prevent deep sleep.     
      USB hard drive    : Protocol = USB
      USB thumb drive   : Protocol = USB 
      SD Card           : Protocol = USB, Protocol = Secure Digital 
      External drive    : Interconnect Location = External

    These disks do not cause us to create an ExternalMedia assertion;
      CD/DVD            : Protocol = ATAPI
      Disk Image        : Protocol = Disk Image
    */
    
    description = DADiskCopyDescription(disk);
    if (description) {
        
        if (CFDictionaryGetValue(description, kDADiskDescriptionDeviceInternalKey) == kCFBooleanFalse) {
            ret = true;
        } else {
            protocol = CFDictionaryGetValue(description, kDADiskDescriptionDeviceProtocolKey);

            if (protocol &&
                (CFEqual(protocol, CFSTR(kIOPropertyPhysicalInterconnectTypeUSB)) ||
                 CFEqual(protocol, CFSTR(kIOPropertyPhysicalInterconnectTypeSecureDigital))))
            {
                ret = true;
            }
        }

        CFRelease(description);
    }
    return ret;
}

/*****************************************************************************/


static void adjustExternalDiskAssertion()
{
    CFIndex	deviceCount = CFSetGetCount(gExternalMediaSet);
    
    if (0 == deviceCount)
    {	
        /*
         * Release assertion
         */
        
        // This call dispatches assertion release to main queue
        InternalReleaseAssertion(&gDiskAssertionID);
        

        return;
    }
    

    if (0 < deviceCount)
    {
        /*
         *  Create new assertion
         */
         
        CFMutableDictionaryRef assertionDescription = NULL;
         
        assertionDescription = _IOPMAssertionDescriptionCreate(
                                        _kIOPMAssertionTypeExternalMedia, 
                                        CFSTR(_kExternalMediaAssertionName), 
                                        NULL, CFSTR("An external media device is attached."), NULL, 0, NULL);
        
        // This call dispatches assertion create to main queue
        InternalCreateAssertion(assertionDescription, 
                                &gDiskAssertionID);

        CFRelease(assertionDescription);

        return;
    }

    return;
}


