/*
 *  suidLauncher.c
 *  PowerManagement
 *
 *  Created by local on 4/28/06.
 *  Copyright 2006 Apple Computer. All rights reserved.
 *
 */
 
/*
 *  This builds a small suid binary whose sole purpose is to run 
 *  kextload and kextunload commands as root.
 *
 *
 *  Usage:
 *      suidLauncher load <path_to_kext>
 *      suidLauncher unload <path_to_kext>
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define ARG_LOAD            "load"
#define ARG_UNLOAD          "unload"
#define ARG_KICKER          "kickbattmon"


#define PATH_KEXTLOAD       "/sbin/kextload"
#define PATH_KEXTUNLOAD     "/sbin/kextunload"
#define PATH_KILLALL        "/usr/bin/killall"

int main(int argc, char *argv[])
{
    int         exec_err;
    char        *kextPath;
    
    if (argc < 2) 
    {
        // No arguments; nothing to execute!
        printf("error: no arguments\n");
        return 0;
    }
    
    kextPath = argv[2];
    
    if ( (0 == strcmp(argv[1], ARG_LOAD))
            && kextPath) 
    {
        exec_err = execl(PATH_KEXTLOAD, PATH_KEXTLOAD, kextPath, NULL);    
    } else if ( (0 == strcmp(argv[1], ARG_UNLOAD))
                && kextPath ) 
    {
        // exec kextunload
        exec_err = execl(PATH_KEXTUNLOAD, PATH_KEXTUNLOAD, kextPath, NULL);
    } else if (0 == strcmp(argv[1], ARG_KICKER))
    {
        exec_err = execl(PATH_KILLALL, PATH_KILLALL, "SystemUIServer", NULL);
    } else {
        printf("error: invalid input. did nothing.\n");
        return 0;
    }
    
    if (0 == exec_err) {
        printf("\tsuccess: %s\n", argv[1]);
        return 0;
    }

    if (-1 == exec_err) 
    {
        printf("Error %d from execvp \"%s\"; \"%s\"\n", exec_err, strerror(exec_err), argv[1]);
        return 2;
    } else {
        printf("Error %d return from execvp \"%s\".\n", argv[1]);
        return 3;
    }
}