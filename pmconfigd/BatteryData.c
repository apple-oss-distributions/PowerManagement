/*
 * Copyright (c) 2017 Apple Computer, Inc. All rights reserved.
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
/*
 * Copyright (c) 2017 Apple Computer, Inc.  All rights reserved.
 *
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPreferences_Private.h>
#include <CoreFoundation/CFXPCBridge.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/IOReturn.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/powermanagement_mig.h>
#include <battery/battery.h>
#include "BatteryTimeRemaining.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

