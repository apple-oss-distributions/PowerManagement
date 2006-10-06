/*
 *  suidLauncher.c
 *  PowerManagement
 *
 *  Created by local on 4/28/06.
 *  Copyright 2006 Apple Computer. All rights reserved.
 *
 */
 
/*
 *  This builds a small binary whose sole purpose is to run the command
 *  and arguments it is passed with root permissions.
 *  In the context of BatteryFaker, this will run a shell script that
 *  requires root privileges to load and unload kexts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[])
{
    int err_return;
    
    if(1 == argc) {
        // No arguments; nothing to execute!
        exit(1);
    }

    err_return = 
        execvp( /* prog name */ argv[1], /* arguments */ &argv[2]);

    if( -1 == err_return ) {
        printf("Errno %d from execvp; \"%s\"\n", errno, strerror(errno));
        exit(2);
    } else {
        printf("Error %d return from execvp.\n");
        exit(3);
    }
}