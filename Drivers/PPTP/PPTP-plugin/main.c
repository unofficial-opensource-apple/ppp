/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/* -----------------------------------------------------------------------------
 *
 *  Theory of operation :
 *
 *  plugin to add PPTP client support to pppd.
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

#include "../../../Controller/ppp_msg.h"
#include "../../../Family/ppp_defs.h"
#include "../../../Family/if_ppp.h"
#include "../../../Family/ppp_domain.h"
#include "../PPTP-extension/PPTP.h"
#include "../../../Helpers/pppd/pppd.h"
#include "../../../Helpers/pppd/fsm.h"
#include "../../../Helpers/pppd/lcp.h"
#include "pptp.h"

/* -----------------------------------------------------------------------------
 Definitions
----------------------------------------------------------------------------- */

#define MODE_CONNECT	"connect"
#define MODE_LISTEN	"listen"
#define MODE_ANSWER	"answer"

#define PPTP_NKE	"PPTP.kext"


/* -----------------------------------------------------------------------------
 Forward declarations
----------------------------------------------------------------------------- */

void pptp_device_check();
void pptp_check_options();
int pptp_connect();
void pptp_disconnect();
void pptp_close_fds();
void pptp_cleanup();
int pptp_establish_ppp(int);
void pptp_wait_input();
void pptp_disestablish_ppp(int);

static void pptp_echo_check();
static void pptp_echo_timeout(void *arg);
static void pptp_send_echo_request();
static void pptp_link_failure();
static void closeall();
static u_long load_kext(char *kext);
static void pptp_ip_up(void *arg, int p);
static void pptp_ip_down(void *arg, int p);
static boolean_t host_gateway(int cmd, struct in_addr host, struct in_addr gateway);


/* -----------------------------------------------------------------------------
 PPP globals
----------------------------------------------------------------------------- */


static int 	ctrlsockfd = -1;		/* control socket (TCP) file descriptor */
static int 	datasockfd = -1;		/* data socket (GRE) file descriptor */
static int 	eventsockfd = -1;		/* event socket to detect interface change */
static CFBundleRef 	bundle = 0;		/* our bundle ref */

/* option variables */
static char 	*mode = MODE_CONNECT;		/* connect mode by default */
static bool 	noload = 0;			/* don't load the kernel extension */

static u_int16_t our_call_id = 0;		/* pptp call id */
static u_int16_t peer_call_id = 0;		/* pptp peer's calld id */
static u_int16_t our_window = PPTP_RECEIVE_WINDOW;/* our receive window size */
static u_int16_t peer_ppd = 0;			/* peer packet processing delay */
static u_int16_t our_ppd = 0;			/* our packet processing delay */
static u_int16_t peer_window = 0;		/* peer's receive window size */
static u_int16_t maxtimeout = 64;		/* Maximum adaptative timeout (in seconds) */
static struct in_addr peeraddress;		/* the other side IP address */
static struct in_addr ip_zeros = { 0 };
static u_int8_t routeraddress[16];
static u_int8_t interface[17];
static pthread_t resolverthread = 0;
static int 	resolverfds[2];

/* 
Fast echo request procedure is run when a networking change is detected 
Echos are sent every 5 seconds for 30 seconds, or until a reply is received
If the network is detected dead, the the tunnel is disconnected.
Echos are not sent during normal operation.
*/
static int	echo_interval = 5; 		/* Interval between echo-requests */
static int	echo_fails = 6;		/* Tolerance to unanswered echo-requests */
static int	echo_timer_running = 0;
static int 	echos_pending = 0;	
static int	echo_identifier = 0; 	
static int	echo_active = 0;

extern int 		kill_link;
extern CFStringRef	serviceidRef;		/* from pppd/sys_MacOSX.c */
extern SCDynamicStoreRef cfgCache;		/* from pppd/sys_MacOSX.c */

/* option descriptors */
option_t pptp_options[] = {
    { "nopptpload", o_bool, &noload,
      "Don't try to load the PPTP kernel extension", 1 },
    { "pptpmode", o_string, &mode,
      "Configure configuration mode [connect, listen, answer]" },
    { "pptpmaxtimeout", o_int, &maxtimeout,
      "Maximum adaptative timeout" },
    { "pptp-fast-echo-interval", o_int, &echo_interval,
      "Fast echo interval to reassert link" },
    { "pptp-fast-echo-failure", o_int, &echo_fails,
      "Fast echo failure to reassert link" },
    { NULL }
};


/* -----------------------------------------------------------------------------
plugin entry point, called by pppd
----------------------------------------------------------------------------- */
int start(CFBundleRef ref)
{
 
    bundle = ref;
    CFRetain(bundle);
    
    // add the socket specific options
    add_options(pptp_options);
    
    // hookup our socket handlers
    dev_device_check_hook = pptp_device_check;
    dev_wait_input_hook = pptp_wait_input;
    dev_check_options_hook = pptp_check_options;
    dev_connect_hook = pptp_connect;
    dev_disconnect_hook = pptp_disconnect;
    dev_cleanup_hook = pptp_cleanup;
    dev_close_fds_hook = pptp_close_fds;
    dev_establish_ppp_hook = pptp_establish_ppp;
    dev_disestablish_ppp_hook = pptp_disestablish_ppp;

    add_notifier(&ip_up_notify, pptp_ip_up, 0);
    add_notifier(&ip_down_notify, pptp_ip_down, 0);

    return 0;
}

/* ----------------------------------------------------------------------------- 
----------------------------------------------------------------------------- */
void pptp_device_check()
{
    if (!strcmp(mode, MODE_ANSWER)) {
        // make sure we get a file descriptor > 2 so that pppd can detach and close 0,1,2
        ctrlsockfd = dup(0);
    }
}

/* ----------------------------------------------------------------------------- 
do consistency checks on the options we were given
----------------------------------------------------------------------------- */
void pptp_check_options()
{
    if (strcmp(mode, MODE_CONNECT) 
        && strcmp(mode, MODE_LISTEN) 
        && strcmp(mode, MODE_ANSWER)) {
        error("PPTP incorrect mode : '%s'", mode ? mode : "");
        mode = MODE_CONNECT;
    }
}

/* ----------------------------------------------------------------------------- 
called back everytime we go out of select, and data needs to be read
the hook is called and has a chance to get data out of its file descriptor
in the case of PPTP, we get control data on the socket
or get awaken when connection is closed
----------------------------------------------------------------------------- */
void pptp_wait_input()
{
    int err;
    
    if (eventsockfd != -1 && is_ready_fd(eventsockfd)) {
    
        char                 	buf[256], ev_if[32];
        struct kern_event_msg	*ev_msg;
        struct kev_in_data     	*inetdata;

        if (recv(eventsockfd, &buf, sizeof(buf), 0) != -1) {
            ev_msg = (struct kern_event_msg *) &buf;
            inetdata = (struct kev_in_data *) &ev_msg->event_data[0];
            switch (ev_msg->event_code) {
                case KEV_INET_NEW_ADDR:
                case KEV_INET_CHANGED_ADDR:
                case KEV_INET_ADDR_DELETED:
                    sprintf(ev_if, "%s%ld", inetdata->link_data.if_name, inetdata->link_data.if_unit);
                    // check if changes occured on the interface we are using
                    if (!strncmp(ev_if, interface, sizeof(interface))) {
                        if (inetdata->link_data.if_family == APPLE_IF_FAM_PPP
                            || echo_interval == 0
                            || echo_fails == 0) {
                            // disconnect immediatly
                            pptp_link_failure();
                        }
                        else {
                            // give time to check for connectivity
                            /* Clear the parameters for generating echo frames */
                            echos_pending      = 0;
                            echo_active = 1;
                            
                            /* If a timeout interval is specified then start the timer */
                            if (echo_interval != 0)
                                pptp_echo_check();
                        }
                    }
                    break;
            }
        }
    }

    if (ctrlsockfd != -1 && is_ready_fd(ctrlsockfd)) {
    
       err = pptp_data_in(ctrlsockfd);
       if (err < 0) {
            // looks like we have been disconnected...
            // it's OK to get a hangup during terminate phase
            if (phase != PHASE_TERMINATE) {
                notice("PPTP hangup");
                status = EXIT_HANGUP;
            }
            remove_fd(ctrlsockfd);
            remove_fd(eventsockfd);
            hungup = 1;
            lcp_lowerdown(0);	/* PPTP link is no longer available */
            link_terminated(0);
        }
    }
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
void *pptp_resolver_thread(void *arg)
{
    struct hostent 	*host;
    char		result = -1;

    if (pthread_detach(pthread_self()) == 0) {
        
        // try to resolve the name
        if (host = gethostbyname(remoteaddress)) {
            peeraddress = *(struct in_addr *)host->h_addr;
            result = 0;
        }
    }

    write(resolverfds[1], &result, 1);
    return 0;
}

/* ----------------------------------------------------------------------------- 
get the socket ready to start doing PPP.
That is, open the socket and start the PPTP dialog
----------------------------------------------------------------------------- */
int pptp_connect()
{
    char 		dev[32], name[MAXPATHLEN], c; 
    int 		err = 0, len, fd;  
    CFURLRef		url;
    CFDictionaryRef	dict;
    CFStringRef		string, key;
    struct sockaddr_in 	addr;	
    struct kev_request	kev_req;
    
    if (cfgCache == NULL || serviceidRef == NULL) {
        goto fail;
    }

    sprintf(dev, "socket[%d:%d]", PF_PPP, PPPPROTO_PPTP);
    strlcpy(ppp_devnam, dev, sizeof(ppp_devnam));

    hungup = 0;
    kill_link = 0;
    our_call_id = getpid();
    routeraddress[0] = 0;
    interface[0] = 0;
    
    key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL, kSCDynamicStoreDomainState, kSCEntNetIPv4);
    if (key) {
        dict = SCDynamicStoreCopyValue(cfgCache, key);
	CFRelease(key);
        if (dict) {
            if (string  = CFDictionaryGetValue(dict, kSCPropNetIPv4Router));
                CFStringGetCString(string, routeraddress, sizeof(routeraddress), kCFStringEncodingUTF8);
            if (string  = CFDictionaryGetValue(dict, kSCDynamicStorePropNetPrimaryInterface));
                CFStringGetCString(string, interface, sizeof(interface), kCFStringEncodingUTF8);
            CFRelease(dict);
        }
    }

    eventsockfd = socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT);
    if (eventsockfd != -1) {
        // PPTP can survive without event socket anyway
        kev_req.vendor_code = KEV_VENDOR_APPLE;
        kev_req.kev_class = KEV_NETWORK_CLASS;
        kev_req.kev_subclass = KEV_INET_SUBCLASS;
        ioctl(eventsockfd, SIOCSKEVFILT, &kev_req);
    }

    /* -------------------------------------------------------------*/
    /* connect mode : we need a valid remote address or name */
    if (!strcmp(mode, MODE_CONNECT)) {
        if (remoteaddress == 0) {
            error("PPTP: No remote address supplied...\n");
            devstatus = EXIT_PPTP_NOSERVER;
            goto fail;
        }

        if (inet_aton(remoteaddress, &peeraddress) == 0) {
        
            if (pipe(resolverfds) < 0) {
                error("PPTP: failed to create pipe for gethostbyname...\n");
                goto fail;
            }
    
            if (pthread_create(&resolverthread, NULL, pptp_resolver_thread, NULL)) {
                error("PPTP: failed to create thread for gethostbyname...\n");
                close(resolverfds[0]);
                close(resolverfds[1]);
                goto fail;
            }
            
            while (read(resolverfds[0], &c, 1) != 1) {
                if (kill_link) {
                    pthread_cancel(resolverthread);
                    break;
                }
            }
            
            close(resolverfds[0]);
            close(resolverfds[1]);
            
            if (kill_link)
                goto fail1;
            
            if (c) {
                error("PPTP: Host '%s' not found...\n", remoteaddress);
                devstatus = EXIT_PPTP_NOSERVER;
                goto fail;
            }
        }

        info("PPTP connecting to server '%s' (%s)...\n", remoteaddress, inet_ntoa(peeraddress));

        if ((ctrlsockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
            error("PPTP can't create control socket...\n");
            goto fail;
        }    

        /* connect to the pptp server */
        bzero(&addr, sizeof(addr));
        addr.sin_len = sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_port = PPTP_TCP_PORT;
        addr.sin_addr = peeraddress; 
        while (connect(ctrlsockfd, (struct sockaddr *)&addr, sizeof(addr))) {
            if (errno != EINTR) {
                error("PPTP connect errno = %d %m\n", errno);
                devstatus = EXIT_PPTP_NOANSWER;
                goto fail;
            }
            if (kill_link)
                goto fail1;
        }

        err = pptp_outgoing_call(ctrlsockfd, 
            our_call_id, our_window, our_ppd, &peer_call_id, &peer_window, &peer_ppd);
        
    }
    /* -------------------------------------------------------------*/
    /* answer mode : we need a valid remote address or name */
    else if (!strcmp(mode, MODE_ANSWER)) {
        len = sizeof(addr);
        if (getpeername(ctrlsockfd, (struct sockaddr *)&addr, &len) < 0) {
            error("PPTP: cannot get client address... %m\n");
            //devstatus = EXIT_PPTP_NOSERVER;
            goto fail;
        }
        peeraddress = addr.sin_addr;

        info("PPTP incoming call in progress from '%s'...", inet_ntoa(peeraddress));
        
        err = pptp_incoming_call(ctrlsockfd, 
            our_call_id, our_window, our_ppd, &peer_call_id, &peer_window, &peer_ppd);
    }
    /* -------------------------------------------------------------*/
    else if (!strcmp(mode, MODE_LISTEN)) {

        info("PPTP listening...\n");
        
        if ((fd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
            error("PPTP can't create listening socket...\n");
            goto fail;
        }    

        bzero(&addr, sizeof(addr));
        addr.sin_len = sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_port = PPTP_TCP_PORT;
        addr.sin_addr.s_addr = INADDR_ANY; 
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            error("PPTP bind failed, %m");
            goto fail;
        }

        if (listen(fd, 10) < 0) {
            error("PPTP listen failed, %m");
            return errno;
        }

        len = sizeof(addr);
        ctrlsockfd = accept(fd, (struct sockaddr *)&addr, &len);
        close(fd);	// close the socket used for listening
        if (ctrlsockfd < 0) {
            error("PPTP accept failed, %m");
            goto fail;
        }
    
        peeraddress = addr.sin_addr;

        info("PPTP incoming call in progress from '%s'...", inet_ntoa(peeraddress));

        err = pptp_incoming_call(ctrlsockfd, 
                our_call_id, our_window, our_ppd, &peer_call_id, &peer_window, &peer_ppd);
    }

    if (err) {
        if (err != -2) {
            if (err != -1)
                devstatus = err;
            goto fail;
        }
        goto fail1;
    }
    
    info("PPTP connection established.");

    /* open the data socket */
    datasockfd = socket(PF_PPP, SOCK_DGRAM, PPPPROTO_PPTP);
    if (datasockfd < 0) {
        if (!noload) {
            if (url = CFBundleCopyBundleURL(bundle)) {
                name[0] = 0;
                CFURLGetFileSystemRepresentation(url, 0, name, MAXPATHLEN - 1);
                CFRelease(url);
                strcat(name, "/");
                if (url = CFBundleCopyBuiltInPlugInsURL(bundle)) {
                    CFURLGetFileSystemRepresentation(url, 0, name + strlen(name), 
                        MAXPATHLEN - strlen(name) - strlen(PPTP_NKE) - 1);
                    CFRelease(url);
                    strcat(name, "/");
                    strcat(name, PPTP_NKE);
                    if (!load_kext(name))
                        datasockfd = socket(PF_PPP, SOCK_DGRAM, PPPPROTO_PPTP);
                }	
            }
        }
        if (datasockfd < 0) {
            error("Failed to open PPTP socket: %m");
            goto fail;
       }
    }

    if (debug) {
        u_int32_t 	flags;
        flags = debug ? PPTP_FLAG_DEBUG : 0;
        if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_FLAGS, &flags, 4)) {
            error("PPTP can't set PPTP flags...\n");
            goto fail;
        }
    }

    if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_PEERADDRESS, &peeraddress.s_addr, 4)) {
        error("PPTP can't set PPTP server address...\n");
        goto fail;
    }

    if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_CALL_ID, &our_call_id, 2)) {
        error("PPTP can't set our call id...\n");
        goto fail;
    }

    if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_PEER_CALL_ID, &peer_call_id, 2)) {
        error("PPTP can't set peer call id...\n");
        goto fail;
    }
    if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_WINDOW, &our_window, 2)) {
        error("PPTP can't set our receive window size...\n");
        goto fail;
    }
    if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_PEER_WINDOW, &peer_window, 2)) {
        error("PPTP can't set peer receive window size...\n");
        goto fail;
    }
    if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_PEER_PPD, &peer_ppd, 2)) {
        error("PPTP can't set peer packet processing delay ...\n");
        goto fail;
    }
    if (setsockopt(datasockfd, PPPPROTO_PPTP, PPTP_OPT_MAXTIMEOUT, &maxtimeout, 2)) {
        error("PPTP can't set adaptative maximum timeout ...\n");
        goto fail;
    }

    return datasockfd;
 
fail:   
    status = EXIT_CONNECT_FAILED;
fail1:
    if (eventsockfd != -1) {
        close(eventsockfd);
        eventsockfd = -1;
    }
    if (ctrlsockfd != -1) {
        close(ctrlsockfd);
        ctrlsockfd = -1;
    }
    return -1;
}

/* ----------------------------------------------------------------------------- 
run the disconnector
----------------------------------------------------------------------------- */
void pptp_disconnect()
{
    info("PPTP disconnecting...\n");
    
    if (eventsockfd != -1) {
        close(eventsockfd);
        eventsockfd = -1;
    }
    if (ctrlsockfd >= 0) {
        close(ctrlsockfd);
        ctrlsockfd = -1;
    }

    info("PPTP disconnected\n");
}

/* ----------------------------------------------------------------------------- 
close the socket descriptors
----------------------------------------------------------------------------- */
void pptp_close_fds()
{
    echo_active = 0; 
    if (echo_timer_running) {
        UNTIMEOUT (pptp_echo_timeout, 0);
        echo_timer_running = 0;
    }
    if (eventsockfd != -1) {
        close(eventsockfd);
        eventsockfd = -1;
    }
    if (datasockfd >= 0) {
        close(datasockfd);
        datasockfd = -1;
    }
    if (ctrlsockfd >= 0) {
        close(ctrlsockfd);
        ctrlsockfd = -1;
    }
}

/* ----------------------------------------------------------------------------- 
clean up before quitting
----------------------------------------------------------------------------- */
void pptp_cleanup()
{
    pptp_close_fds();
}

/* ----------------------------------------------------------------------------- 
establish the socket as a ppp link
----------------------------------------------------------------------------- */
int pptp_establish_ppp(int fd)
{
    int 	x;

    if (ioctl(fd, PPPIOCATTACH, &x) < 0) {
        error("Couldn't attach socket to the link layer: %m");
        return -1;
    }

    // add just the control socket
    // the data socket is just for moving data in the kernel
    add_fd(ctrlsockfd);		
    add_fd(eventsockfd);		
    return 0;
}

/* ----------------------------------------------------------------------------- 
establish the socket as a ppp link
----------------------------------------------------------------------------- */
void pptp_disestablish_ppp(int fd)
{
    int 	x;
    
    remove_fd(ctrlsockfd);
    remove_fd(eventsockfd);		

    if (ioctl(fd, PPPIOCDETACH, &x) < 0)
        error("Couldn't detach socket from link layer: %m");
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
void closeall()
{
    int i;

    for (i = getdtablesize() - 1; i >= 0; i--) close(i);
    open("/dev/null", O_RDWR, 0);
    dup(0);
    dup(0);
    return;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_long load_kext(char *kext)
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
----------------------------------------------------------------------------- */
void pptp_ip_up(void *arg, int p)
{
    struct in_addr	gateway;

    if (routeraddress[0] == 0
        || inet_aton(routeraddress, &gateway) != 1)
	return;
    
    if (peeraddress.s_addr) {
	host_gateway(RTM_DELETE, peeraddress, ip_zeros);
	host_gateway(RTM_ADD,    peeraddress, gateway);
    }
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
void pptp_ip_down(void *arg, int p)
{
    if (peeraddress.s_addr) {
	host_gateway(RTM_DELETE, peeraddress, ip_zeros);
    }
}

/* -----------------------------------------------------------------------------
add/remove a host route
----------------------------------------------------------------------------- */
static boolean_t
host_gateway(int cmd, struct in_addr host, struct in_addr gateway)
{
    int 			len;
    int 			rtm_seq = 0;
    struct {
	struct rt_msghdr	hdr;
	struct sockaddr_in	dst;
	struct sockaddr_in	gway;
    } 				rtmsg;
    int 			sockfd = -1;

    if ((sockfd = socket(PF_ROUTE, SOCK_RAW, AF_INET)) < 0) {
	syslog(LOG_INFO, "host_gateway: open routing socket failed, %s",
	       strerror(errno));
	return (FALSE);
    }

    memset(&rtmsg, 0, sizeof(rtmsg));
    rtmsg.hdr.rtm_type = cmd;
    rtmsg.hdr.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC | RTF_HOST;
    rtmsg.hdr.rtm_version = RTM_VERSION;
    rtmsg.hdr.rtm_seq = ++rtm_seq;
    rtmsg.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY;
    rtmsg.dst.sin_len = sizeof(rtmsg.dst);
    rtmsg.dst.sin_family = AF_INET;
    rtmsg.dst.sin_addr = host;
    rtmsg.gway.sin_len = sizeof(rtmsg.gway);
    rtmsg.gway.sin_family = AF_INET;
    rtmsg.gway.sin_addr = gateway;

    len = sizeof(rtmsg);
    rtmsg.hdr.rtm_msglen = len;
    if (write(sockfd, &rtmsg, len) < 0) {
	syslog(LOG_DEBUG, "host_gateway: write routing socket failed, %s",
	       strerror(errno));
	close(sockfd);
	return (FALSE);
    }

    close(sockfd);
    return (TRUE);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static void pptp_echo_check ()
{
    if (echo_active == 0 || echo_timer_running != 0)
        return;
    
    pptp_send_echo_request ();

    TIMEOUT (pptp_echo_timeout, 0, echo_interval);
    echo_timer_running = 1;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static void pptp_echo_timeout (void *arg)
{
    if (echo_timer_running != 0) {
        echo_timer_running = 0;
        pptp_echo_check ();
    }
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
void pptp_received_echo_reply(u_int32_t identifier, u_int8_t result, u_int8_t error)
{
    // not really interested in the content
    // we just know our link is still alive
    dbglog("PPTP received Echo Reply, id = %d", identifier);
    /* Reset the number of outstanding echo frames */
    echos_pending = 0;
    echo_active = 0;    
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static void pptp_link_failure ()
{
    // major change happen on the interface we are using.
    // disconnect PPTP 
    // Enhancement : should check if link is still usable
    notice("PPTP has detected change in the network and lost connection with the server.");
    // Fix me : Don't generate error message 
    // Additionnal exit code requires localization
    status = EXIT_USER_REQUEST;
    //devstatus = EXIT_PPTP_NETWORKCHANGED;
    //status = EXIT_HANGUP;
    remove_fd(ctrlsockfd);
    remove_fd(eventsockfd);
    hungup = 1;
    lcp_lowerdown(0);	/* PPTP link is no longer available */
    link_terminated(0);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static void pptp_send_echo_request ()
{
    /*
     * Detect the failure of the peer at this point.
     */
    if (echo_fails != 0) {
        if (echos_pending >= echo_fails) {
            pptp_link_failure();
	    echos_pending = 0;
	}
    }

    /*
     * Make and send the echo request frame.
     */
    dbglog("PPTP sent Echo Request, id = %d", echo_identifier);
    if (pptp_echo(ctrlsockfd, echo_identifier++) == -1)
        pptp_link_failure();
    echos_pending++;
}