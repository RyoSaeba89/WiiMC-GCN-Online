/****************************************************************************
 * WiiMC
 * Tantric 2009-2012
 *
 * networkop.cpp
 * Network and SMB support routines
 ****************************************************************************/

#include <network.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>
#include <smb.h>
#include <mxml.h>

#include "wiimc.h"
#include "fileop.h"
#include "filebrowser.h"
#include "menu.h"
#include "networkop.h"
#include "settings.h"
//#include "utils/ftp_devoptab.h"
#include "utils/http.h"
#include "utils/gettext.h"
#include "libwiigui/gui.h"
#include "utils/3ds.h"
#include "utils/debuglog.h"
#include "utils/dns.h"

extern bool want3DS;

void ShowAction (const char *msg, UpdateCallback c);

static int netHalt = 0;
static bool networkInit = false;
static bool networkShareInit[MAX_SHARES] = { false, false, false, false, false };
//static bool ftpInit[MAX_SHARES] = { false, false, false, false, false };
char wiiIP[16] = { 0 };

/****************************************************************************
 * InitializeNetwork
 * Initializes the Wii/GameCube network interface
 ***************************************************************************/

static lwp_t networkthread = LWP_THREAD_NULL;
static u8 netstack[32768] ATTRIBUTE_ALIGN (32);

/* Which adapter answered, in words.
 *
 * This fork asks for none in particular: libogc2's if_config() tries the
 * DOL-015 first, then the three SPI chips ETH2GC is built around (W6100,
 * W5500, ENC28J60), and each of those probes Serial Port 1, then Serial
 * Port 2, then the two memory card slots. So an ETH2GC needs no code of its
 * own -- but nothing else then knows which one was found, and from the couch
 * "no network" and "found the wrong adapter" look identical (PORTING.md 5.3).
 *
 * Every driver stamps the netif with two letters: the first identifies the
 * chip, the second the port it answered on. Ported from gcradio,
 * source/main.c, adapter_label(). */
static char netAdapter[48] = "";

static void AdapterLabel(char *out, int max)
{
	char ifn[8] = "";
	const char *chip, *port;

	if(!if_indextoname(1, ifn) || !ifn[0] || !ifn[1])
	{
		snprintf(out, max, "unknown adapter");
		return;
	}

	switch(ifn[0])
	{
		case 'e': chip = "Broadband Adapter"; break; // "en", the DOL-015
		case 'E': chip = "ENC28J60"; break;          // ETH2GC Sidecar / Lite
		case 'W': chip = "W5500"; break;
		case 'w': chip = "W6100"; break;
		default:  chip = "adapter"; break;
	}

	switch(ifn[1])
	{
		case 'n':                                    // gcif is always SP1
		case '1': port = "Serial Port 1"; break;
		case '2': port = "Serial Port 2"; break;
		case 'A': port = "card slot A"; break;
		case 'B': port = "card slot B"; break;
		default:  port = "?"; break;
	}

	snprintf(out, max, "%s, %s", chip, port);
}

/* if_config() can return success and hand back an address that is not one.
 * Seen on hardware (PORTING.md 11.15): the first call failed with -1, the
 * second returned >= 0 in 14 ms with 255.255.255.255 for the address, the mask
 * and the gateway. Believing it would have pointed the resolver at the
 * broadcast address. */
static bool UsableAddress(const char *s)
{
	struct in_addr a;

	if(s == NULL || s[0] == 0 || !inet_aton(s, &a))
		return false;

	return a.s_addr != 0 && a.s_addr != 0xffffffff;
}

const char *NetworkAdapterName()
{
	if(netAdapter[0])
		return netAdapter;
	return "none";
}

static void * netcb (void *arg)
{
#ifdef WANT_NETWORK
	/* if_config() blocks, and it blocks for a long time: bringing the BBA's
	 * link up takes seconds, and when nothing answers libogc2 walks four
	 * drivers across four ports before giving up. That is what this thread is
	 * for -- the GUI keeps drawing "Initializing network..." and stays
	 * cancellable while the probe runs.
	 *
	 * DHCP only. There is no static-address setting anywhere in the menus to
	 * read one from, and adding one is not phase 1. */
	while(netHalt != 2)
	{
		int retry = 3;

		while(retry > 0 && netHalt != 2 && !networkInit)
		{
			char ip[16] = "", mask[16] = "", gw[16] = "";
			s32 res;

			DebugMark("net: if_config (dhcp), attempt %d", 4 - retry);
			res = if_config(ip, mask, gw, true);

			if(res >= 0 && UsableAddress(ip))
			{
				struct in_addr a;

				strncpy(wiiIP, ip, sizeof(wiiIP) - 1);
				wiiIP[sizeof(wiiIP) - 1] = 0;
				AdapterLabel(netAdapter, sizeof(netAdapter));

				/* The resolver queries the gateway: DHCP hands one back, every
				 * home router relays DNS, and there is no menu to configure a
				 * server in. See PORTING.md 3.1. */
				if(UsableAddress(gw) && inet_aton(gw, &a))
				{
					dns_set_server(a.s_addr);
				}
				else
				{
					dns_set_server(0);
					DebugMark("net: no usable gateway ('%s') -- names will not resolve", gw);
				}
				dns_cache_flush();

				DebugMark("net: up on %s -- ip %s mask %s gw %s",
					netAdapter, wiiIP, mask, gw);
				networkInit = true;
				break;
			}

			if(res >= 0)
			{
				/* Success with a nonsense address. libogc2 brings the stack up
				 * once; calling again cannot un-fail a first attempt that
				 * failed, so spending the remaining tries on it is pointless. */
				DebugMark("net: if_config returned %d but ip '%s' is not an address -- giving up", res, ip);
				break;
			}

			DebugMark("net: if_config failed (%d) -- cable, link LED, or no adapter", res);
			retry--;

			if(retry > 0 && netHalt != 2)
				sleep(1);
		}

		if(netHalt != 2)
		{
			LWP_SuspendThread(networkthread);
			usleep(100);
		}
	}
#endif
	return NULL;
}

/****************************************************************************
 * StartNetworkThread
 *
 * Signals the network thread to resume, or creates a new thread
 ***************************************************************************/
void StartNetworkThread()
{
	netHalt = 0;

#ifdef WANT_NETWORK
	if(networkthread == LWP_THREAD_NULL)
		LWP_CreateThread(&networkthread, netcb, NULL, netstack, sizeof(netstack), 40);
	else
		LWP_ResumeThread(networkthread);
#endif
}

/****************************************************************************
 * StopNetworkThread
 *
 * Signals the network thread to stop
 ***************************************************************************/
static void StopNetworkThread()
{
	if(networkthread == LWP_THREAD_NULL || !LWP_ThreadIsSuspended(networkthread))
		return;

	netHalt = 2;

	LWP_ResumeThread(networkthread);

	// wait for thread to finish
	LWP_JoinThread(networkthread, NULL);
	networkthread = LWP_THREAD_NULL;
}

extern "C"{
/* A breadcrumb MPlayer can drop that is flushed to the card before it returns.
 * It lives here rather than in the MPlayer tree because the sub-make does not
 * get -DWANT_DEBUGLOG, so DebugMark is not visible there -- and because this
 * way ENABLE_DEBUGLOG = 0 turns it into an empty function instead of an
 * undefined symbol. For diagnosing a freeze, where anything merely buffered
 * is lost (PORTING.md 11.18). */
void MPlayerNetMark(const char *what, int value)
{
	DebugMark("net: %s = %d", what, value);
}

void CheckMplayerNetwork() //to use in cache2.c in mplayer
{
#ifdef WANT_NETWORK
	/* Called from cache2.c when a network read keeps failing: forget the
	 * interface so the next InitializeNetwork() brings it up again. Do not
	 * start the thread from here -- this runs on MPlayer's cache thread. */
	if(net_gethostip() == 0)
		networkInit = false;
#endif
}
}

static bool cancelNetworkInit = false;

static void networkInitCallback(void *ptr)
{
	GuiButton *b = (GuiButton *)ptr;
	
	if(b->GetState() == STATE_CLICKED)
	{
		b->ResetState();
		cancelNetworkInit = true;
	}
}

bool InitializeNetwork(bool silent)
{
	if(networkInit)
		return true;

#ifndef WANT_NETWORK
	return false; // built without the transport, and the wait below would spin
#else
	ShowAction("Initializing network...", networkInitCallback);
	cancelNetworkInit = false;

	while(!networkInit)
	{
		StartNetworkThread();

		if(networkthread == LWP_THREAD_NULL)
			break; // the thread could not be created; do not spin on it

		while (!LWP_ThreadIsSuspended(networkthread) && !cancelNetworkInit)
			usleep(50 * 1000);

		StopNetworkThread();

		if(silent || cancelNetworkInit)
			break;
	}

	CancelAction();

	return networkInit;
#endif
}

void CloseShare(int num)
{
#if 0
	char devName[10];
	sprintf(devName, "smb%d", num);

	if(networkShareInit[num-1])
		smbClose(devName);
	networkShareInit[num-1] = false;
#endif
}

/****************************************************************************
 * Mount SMB Share
 ****************************************************************************/

bool
ConnectShare (int num, bool silent)
{
	if(!InitializeNetwork(silent))
		return false;
#if 0
	char mountpoint[10];
	sprintf(mountpoint, "smb%d", num);
	int retry = 1;
	int chkS = (strlen(WiiSettings.smbConf[num-1].share) > 0) ? 0:1;
	int chkI = (strlen(WiiSettings.smbConf[num-1].ip) > 0) ? 0:1;

	if(networkShareInit[num-1])
		return true;

	// check that all parameters have been set
	if(chkS + chkI > 0)
	{
		if(!silent)
		{
			char msg[50];
			wchar_t msg2[100];
			if(chkS + chkI > 1) // more than one thing is wrong
				sprintf(msg, "Check settings file.");
			else if(chkS)
				sprintf(msg, "Share name is blank.");
			else if(chkI)
				sprintf(msg, "Share IP is blank.");

			swprintf(msg2, 100, L"%s - %s", gettext("Invalid network share settings"), gettext(msg));
			ErrorPrompt(msg2);
		}
		return false;
	}

	while(retry)
	{
		if(!silent)
			ShowAction ("Connecting to network share...");
		
		if(smbInitDevice(mountpoint, WiiSettings.smbConf[num-1].user, WiiSettings.smbConf[num-1].pwd,
					WiiSettings.smbConf[num-1].share, WiiSettings.smbConf[num-1].ip))
			networkShareInit[num-1] = true;

		if(networkShareInit[num-1] || silent)
			break;

		retry = ErrorPromptRetry("Failed to connect to network share.");
		if(retry) InitializeNetwork(silent);
	}

	if(!silent)
		CancelAction();

	return networkShareInit[num-1];
#endif
}

void ReconnectShare(int num, bool silent)
{
	//CloseShare(num);
	//ConnectShare(num, silent);
}

void CloseFTP(int num)
{
#if 0
	char devName[10];
	sprintf(devName, "ftp%d", num);

	if(ftpInit[num-1])
		ftpClose(devName);
	ftpInit[num-1] = false;
#endif
}

/****************************************************************************
 * Mount FTP site
 ****************************************************************************/

bool
ConnectFTP (int num, bool silent)
{
	//if(!InitializeNetwork(silent))
		return false;
#if 0
	char mountpoint[10];
	sprintf(mountpoint, "ftp%d", num);

	int chkI = (strlen(WiiSettings.ftpConf[num-1].ip) > 0) ? 0:1;
	int chkU = (strlen(WiiSettings.ftpConf[num-1].user) > 0) ? 0:1;
	int chkP = (strlen(WiiSettings.ftpConf[num-1].pwd) > 0) ? 0:1;

	// check that all parameters have been set
	if(chkI + chkU + chkP > 0)
	{
		if(!silent)
		{
			char msg[50];
			wchar_t msg2[100];
			if(chkI + chkU + chkP > 1) // more than one thing is wrong
				sprintf(msg, "Check settings file.");
			else if(chkI)
				sprintf(msg, "IP is blank.");
			else if(chkU)
				sprintf(msg, "Username is blank.");
			else if(chkP)
				sprintf(msg, "Password is blank.");

			swprintf(msg2, 100, L"%s - %s", gettext("Invalid FTP site settings"), gettext(msg));
			ErrorPrompt(msg2);
		}
		return false;
	}

	if(!ftpInit[num-1])
	{
		if(!silent)
			ShowAction ("Connecting to FTP site...");

		if(ftpInitDevice(mountpoint, WiiSettings.ftpConf[num-1].user, 
			WiiSettings.ftpConf[num-1].pwd, WiiSettings.ftpConf[num-1].folder, 
			WiiSettings.ftpConf[num-1].ip, WiiSettings.ftpConf[num-1].port, WiiSettings.ftpConf[num-1].passive))
		{
			ftpInit[num-1] = true;
		}
		if(!silent)
			CancelAction();
	}

	if(!ftpInit[num-1] && !silent)
		ErrorPrompt("Failed to connect to FTP site.");

	return ftpInit[num-1];
#endif
}
