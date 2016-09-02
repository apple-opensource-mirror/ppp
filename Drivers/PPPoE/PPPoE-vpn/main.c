/*
 * Copyright (c) 2000-2003 Apple Computer, Inc. All rights reserved.
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

/* -----------------------------------------------------------------------------
 *
 *  Theory of operation :
 *
 *  PPPoE plugin for vpnd
 *
----------------------------------------------------------------------------- */

/* -----------------------------------------------------------------------------
  Includes
----------------------------------------------------------------------------- */

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <netdb.h>
#include <utmp.h>
#include <pwd.h>
#include <setjmp.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <net/dlil.h>
#include <net/if.h>
#include <net/route.h>
#include <pthread.h>
#include <sys/kern_event.h>
#include <netinet/in_var.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFBundle.h>
#include <SystemConfiguration/SystemConfiguration.h>

#define APPLE 1

#include "../../../Family/ppp_defs.h"
#include "../../../Family/if_ppp.h"
#include "../../../Family/ppp_domain.h"
#include "../../../Helpers/vpnd/vpnplugins.h"
#include "../PPPoE-extension/PPPoE.h"



// ----------------------------------------------------------------------------
//	� Private Globals
// ----------------------------------------------------------------------------

static CFBundleRef 	bundle = 0;
static int 			listen_sockfd = -1;


#define PPPOE_NKE	"PPPoE.kext"

int pppoevpn_get_pppd_args(struct vpn_params *params);
int pppoevpn_listen(void);
int pppoevpn_accept(void);
int pppoevpn_refuse(void);
void pppoevpn_close(void);

static u_long load_kext(char *kext);


/* -----------------------------------------------------------------------------
plugin entry point, called by vpnd
ref is the vpn bundle reference
pppref is the ppp bundle reference
bundles can be layout in two different ways
- As simple vpn bundles (bundle.vpn). the bundle contains the vpn bundle binary.
- As full ppp bundles (bundle.ppp). The bundle contains the ppp bundle binary, 
and also the vpn kext and the vpn bundle binary in its Plugins directory.
if a simple vpn bundle was used, pppref will be NULL.
if a ppp bundle was used, the vpn plugin will be able to get access to the 
Plugins directory and load the vpn kext.
----------------------------------------------------------------------------- */
int start(struct vpn_channel* the_vpn_channel, CFBundleRef ref, CFBundleRef pppref, int debug)
{
    int 	s;
    char 	name[MAXPATHLEN]; 
    CFURLRef	url;

    /* first load the kext if we are loaded as part of a ppp bundle */
    if (pppref) {
        s = socket(PF_PPP, SOCK_DGRAM, PPPPROTO_PPPOE);
        if (s < 0) {
            if (url = CFBundleCopyBundleURL(pppref)) {
                name[0] = 0;
                CFURLGetFileSystemRepresentation(url, 0, name, MAXPATHLEN - 1);
                CFRelease(url);
                strcat(name, "/");
                if (url = CFBundleCopyBuiltInPlugInsURL(pppref)) {
                    CFURLGetFileSystemRepresentation(url, 0, name + strlen(name), 
                                MAXPATHLEN - strlen(name) - strlen(PPPOE_NKE) - 1);
                    CFRelease(url);
                    strcat(name, "/");
                    strcat(name, PPPOE_NKE);
                    if (!load_kext(name))
                        s = socket(PF_PPP, SOCK_DGRAM, PPPPROTO_PPPOE);
                }	
            }
            if (s < 0) {
                syslog(LOG_ERR, "VPND PPPoE plugin: Unable to load PPPoE kernel extension\n");
                return -1;
            }
        }
        close (s);
    }
    
    /* retain reference */
    bundle = ref;
    CFRetain(bundle);
         
    // hookup our socket handlers
    bzero(the_vpn_channel, sizeof(struct vpn_channel));
    the_vpn_channel->get_pppd_args = pppoevpn_get_pppd_args;
    the_vpn_channel->listen = pppoevpn_listen;
    the_vpn_channel->accept = pppoevpn_accept;
    the_vpn_channel->refuse = pppoevpn_refuse;
    the_vpn_channel->close = pppoevpn_close;

    return 0;
}

/* ----------------------------------------------------------------------------- 
    pppoevpn_get_pppd_args
----------------------------------------------------------------------------- */
int pppoevpn_get_pppd_args(struct vpn_params *params)
{
    if (params->serverRef)			
        /* arguments from the preferences file */
        addstrparam(params->exec_args, &params->next_arg_index, "pppoemode", "answer");

    return 0;

}


/* ----------------------------------------------------------------------------- 
    system call wrappers
----------------------------------------------------------------------------- */
int pppoe_sys_accept(int sockfd, struct sockaddr *cliaddr, int *addrlen)
{
    int fd;
    
    while ((fd = accept(sockfd, cliaddr, addrlen)) == -1)
        if (errno != EINTR) {
            syslog(LOG_ERR, "VPND PPPoE plugin: error calling accept = %s\n", strerror(errno));
            return -1;
        }
    return fd;
}

int pppoe_sys_close(int sockfd)
{
    while (close(sockfd) == -1)
        if (errno != EINTR) {
            syslog(LOG_ERR, "VPND PPPoE plugin: error calling close on socket = %s\n", strerror(errno));
            return -1;
        }
    return 0;
}


/* -----------------------------------------------------------------------------
    closeall
----------------------------------------------------------------------------- */
static void closeall()
{
    int i;

    for (i = getdtablesize() - 1; i >= 0; i--) close(i);
    open("/dev/null", O_RDWR, 0);
    dup(0);
    dup(0);
    return;
}


/* -----------------------------------------------------------------------------
    load_kext
----------------------------------------------------------------------------- */
static u_long load_kext(char *kext)
{
    int pid;

    if ((pid = fork()) < 0)
        return 1;

    if (pid == 0) {
        closeall();
        // PPP kernel extension not loaded, try load it...
        execl("/sbin/kextload", "kextload", kext, (char *)0);
        exit(1);
    }

    while (waitpid(pid, 0, 0) < 0) {
        if (errno == EINTR)
            continue;
       return 1;
    }
    return 0;
}


/* -----------------------------------------------------------------------------
    pppoevpn_listen()  called by vpnd to setup listening socket
----------------------------------------------------------------------------- */
int pppoevpn_listen(void)
{

    //struct sockaddr_in	addrListener;    
    
    // Create the requested socket
    while ((listen_sockfd = socket (PF_PPP, SOCK_DGRAM, PPPPROTO_PPPOE)) < 0) 
        if (errno != EINTR) {
            syslog(LOG_ERR, "VPND PPPoE: Could not create socket - err = %s\n", strerror(errno));
            return -1 ;
    }
    
/*
    struct sockaddr_pppoe 	addr;
    int				len, fd;

    info("PPPoE listening on service '%s' [access concentrator '%s']...\n", 
            service ? service : "", 
            access_concentrator ? access_concentrator : "");

    bzero(&addr, sizeof(addr));
    addr.ppp.ppp_len = sizeof(struct sockaddr_pppoe);
    addr.ppp.ppp_family = AF_PPP;
    addr.ppp.ppp_proto = PPPPROTO_PPPOE;
    if (access_concentrator)
        strncpy(addr.pppoe_ac_name, access_concentrator, sizeof(addr.pppoe_ac_name));
    if (service)
        strncpy(addr.pppoe_service, service, sizeof(addr.pppoe_service));
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_pppoe)) < 0) {
        error("PPPoE bind failed, %m");
        return -1;
*/

    while (listen(listen_sockfd, SOMAXCONN) < 0) 
        if (errno == EINTR) {
            syslog(LOG_ERR, "VPND PPPoE plugin: error calling listen = %s\n", strerror(errno));
            return -1;
        }

    return listen_sockfd; 
}


/* -----------------------------------------------------------------------------
    pppoevpn_accept() called by vpnd to listen for incomming connections.
----------------------------------------------------------------------------- */
int pppoevpn_accept(void) 
{

    int				fdConn;
    struct sockaddr_storage	ssSender;
    struct sockaddr		*sapSender = (struct sockaddr *)&ssSender;
    int				nSize = sizeof(ssSender);

    if ((fdConn = pppoe_sys_accept(listen_sockfd, sapSender, &nSize)) < 0)
            return -1;
    if (sapSender->sa_family != AF_PPP) {
        syslog(LOG_ERR, "VPND PPPoE plugin: Unexpected protocol family!\n");
        if (pppoe_sys_close(fdConn) < 0)
            return -1;
        return 0;
    }
   
    return fdConn;
}

/* -----------------------------------------------------------------------------
    pppoevpn_refuse() called by vpnd to refuse incomming connections
    
        return values:  -1		error
                        0		handled - do not launch pppd
----------------------------------------------------------------------------- */
int pppoevpn_refuse(void) 
{

    int				fdConn;
    struct sockaddr_pppoe	ssSender;
    struct sockaddr		*sapSender = (struct sockaddr *)&ssSender;
    int				nSize = sizeof (ssSender);

    if ((fdConn = pppoe_sys_accept(listen_sockfd, sapSender, &nSize)) < 0)
        return -1;
    
    if (pppoe_sys_close(fdConn) < 0)
        return -1;
        
    return 0;
}

/* -----------------------------------------------------------------------------
    pppoevpn_close()  called by vpnd to close listening socket and cleanup.
----------------------------------------------------------------------------- */
void pppoevpn_close(void)
{
    if (listen_sockfd != -1) {
        if (pppoe_sys_close(listen_sockfd) < 0)
            ;  // do nothing      
            
        listen_sockfd = -1;
    }
}

