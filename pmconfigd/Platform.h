/*
 * Copyright (c) 2013 Apple Inc. All rights reserved.
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

#ifndef PowerManagement_Platform_h
#define PowerManagement_Platform_h
#include "XCTest_FunctionDefinitions.h"
#include "PrivateLib.h"


#define kTCPKeepAliveExpireSecs (12*60*60) // 12 hours

typedef enum {
    kNotSupported = 0,
    kActive,
    kInactive,
} tcpKeepAliveStates_et;


typedef struct {
    long                overrideSec;
    tcpKeepAliveStates_et   state;
    XCT_UNSAFE_UNRETAINED dispatch_source_t   expiration;
    CFAbsoluteTime      ts_turnoff; // Time at which Keep Aive will be turned off
} TCPKeepAliveStruct;

/*! getTCPKeepAliveState
 *  
 *  @param buf      Upon return, this buffer will contain a string either "active",
 *                  if TCPKeepAlive is active;
 *                  or "inactive: <reasons>" with the reason that it's inactive.
 *                      inactive: expired, quota
 *                  or "unsupported"
 *  @result         Returns a state value from enum tcpKeepAliveStates_et
 */
__private_extern__ tcpKeepAliveStates_et  getTCPKeepAliveState(char *buf, int buflen);
__private_extern__ long getTCPKeepAliveOverrideSec(void);
__private_extern__ void setTCPKeepAliveOverrideSec(long value);

__private_extern__ void startTCPKeepAliveExpTimer(void);
__private_extern__ void cancelTCPKeepAliveExpTimer(void);
__private_extern__ CFTimeInterval getTcpkaTurnOffTime(void);

__private_extern__ void enableTCPKeepAlive(void);
__private_extern__ void disableTCPKeepAlive(void);
__private_extern__ void evalTcpkaForPSChange(int pwrSrc);


__private_extern__ void setPushConnectionState(bool active);
__private_extern__ bool getPushConnectionState(void);
__private_extern__ bool getWakeOnLanState(void);
#endif
