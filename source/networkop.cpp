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

/* netcb() sets this on its way out. LWP_ThreadIsSuspended() cannot stand in
 * for it: the worker is also "not suspended" while it is wedged, which is
 * precisely the case the callers have to tell apart. */
static volatile bool netThreadExited = false;

/* Set when a worker had to be left running because it could not be reclaimed.
 * It still owns the EXI lock and lwIP's globals, so no second one may start. */
static bool netThreadAbandoned = false;

/* How long the first screen waits before handing the console back.
 *
 * libogc2's if_configex() spins on LWP_YieldThread() until either an address
 * arrives or its DHCP retry counter runs out. With an adapter present, a link
 * up and nothing answering DHCP, neither ever happens: measured on hardware
 * at 81 s and still spinning, with the thread in state 0 -- running, not
 * blocked on anything. Nothing below this layer can be interrupted, so the
 * wait has to be bounded from here (PORTING.md 3.1).
 *
 * The number comes from this console's own logs, not from taste. A healthy
 * DOL-015 on Serial Port 1 returns an address in 10.8 to 11.1 s (builds
 * 20260914-221604 through 20260918-082310). When the first attempt fails it
 * does so at about 5.6 s, and the second then succeeds -- 11.5 s measured,
 * and up to roughly 17 s if that second attempt takes the full time. So
 * anything under 20 s would abandon a working adapter, and 30 s leaves room
 * for a third attempt while staying far from the 81 s hang. */
#define NET_BOOT_TIMEOUT_SECS 30

/* How often the gate says what it can see while it waits. */
#define NET_WAIT_NOTE_SECS 2

/* How long a worker asked to stop gets before it is declared wedged. One
 * parked between retries leaves immediately; one inside if_config() never
 * will. */
#define NET_STOP_TIMEOUT_SECS 2

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
	 * DHCP unless settings.xml carries a static address. That escape hatch
	 * exists because the DHCP wait cannot be interrupted: libogc2's
	 * if_configex() spins on LWP_YieldThread() until an address arrives or
	 * its retry counter runs out, and on a link whose server never completes
	 * the exchange, neither happens -- 81 s and counting on hardware, thread
	 * state 0, running. Nothing above this layer can cut that short, so the
	 * only real cure is not to enter it. The static path returns as soon as
	 * the adapter probe is done. */
	const bool useStatic = UsableAddress(WiiSettings.netStaticIP);

	if(WiiSettings.netStaticIP[0] && !useStatic)
		DebugMark("net: netStaticIP '%s' is not an address -- falling back to DHCP",
			WiiSettings.netStaticIP);

	while(netHalt != 2)
	{
		int retry = 3;

		while(retry > 0 && netHalt != 2 && !networkInit)
		{
			char ip[16] = "", mask[16] = "", gw[16] = "";
			s32 res;

			if(useStatic)
			{
				/* if_config() reads these three when use_dhcp is false. */
				snprintf(ip, sizeof(ip), "%s", WiiSettings.netStaticIP);
				snprintf(mask, sizeof(mask), "%s",
					UsableAddress(WiiSettings.netStaticMask) ? WiiSettings.netStaticMask : "255.255.255.0");
				/* if_config() runs inet_addr() over all three strings and
				 * only checks the pointers for NULL, never for empty. An
				 * empty gateway would come back as 255.255.255.255, so give
				 * it an address that parses to "none". */
				snprintf(gw, sizeof(gw), "%s",
					UsableAddress(WiiSettings.netStaticGW) ? WiiSettings.netStaticGW : "0.0.0.0");

				DebugMark("net: if_config (static %s/%s gw %s), attempt %d",
					ip, mask, gw[0] ? gw : "none", 4 - retry);
				res = if_config(ip, mask, gw, false);
			}
			else
			{
				DebugMark("net: if_config (dhcp), attempt %d", 4 - retry);
				res = if_config(ip, mask, gw, true);
			}

			if(res >= 0 && UsableAddress(ip))
			{
				struct in_addr a;

				strncpy(wiiIP, ip, sizeof(wiiIP) - 1);
				wiiIP[sizeof(wiiIP) - 1] = 0;
				AdapterLabel(netAdapter, sizeof(netAdapter));

				/* The resolver queries the gateway: DHCP hands one back, every
				 * home router relays DNS, and there is no menu to configure a
				 * server in. On the static path the gateway is whatever
				 * settings.xml gave, so an empty one costs name resolution --
				 * which the log says out loud. See PORTING.md 3.1. */
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

				DebugMark("net: up on %s (%s) -- ip %s mask %s gw %s",
					netAdapter, useStatic ? "static" : "dhcp", wiiIP, mask, gw);
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

	netThreadExited = true;
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
#ifdef WANT_NETWORK
	if(netThreadAbandoned)
	{
		/* An abandoned worker may still finish -- if_config() can return
		 * minutes later with an address in hand. Reclaim it when it does;
		 * until then, refuse to run a second one beside it. */
		if(!netThreadExited)
			return;

		LWP_JoinThread(networkthread, NULL);
		networkthread = LWP_THREAD_NULL;
		netThreadAbandoned = false;
		DebugMark("net: the abandoned worker finished after all -- reclaimed");
	}
#endif

	netHalt = 0;

#ifdef WANT_NETWORK
	if(networkthread == LWP_THREAD_NULL)
	{
		netThreadExited = false;

		if(LWP_CreateThread(&networkthread, netcb, NULL, netstack, sizeof(netstack), 40) != 0)
		{
			networkthread = LWP_THREAD_NULL;
			netThreadExited = true;
			DebugMark("net: could not create the network thread");
		}
	}
	else if(LWP_ThreadIsSuspended(networkthread))
		LWP_ResumeThread(networkthread);
#endif
}

/****************************************************************************
 * StopNetworkThread
 *
 * Signals the network thread to stop
 ***************************************************************************/
/* Reclaims the worker, or reports that it could not be reclaimed.
 *
 * The previous version returned early whenever the thread was *not*
 * suspended -- that is, in the one case that matters, a worker still inside
 * if_config(). Cancellation then did nothing at all and the caller was never
 * told, so it went on believing the worker was gone while that worker was
 * still free to write wiiIP and networkInit behind the GUI's back.
 *
 * A wedged worker is abandoned, never killed: it can hold the EXI lock and
 * leave lwIP half-initialised, and LWP_JoinThread() on it would hang this
 * caller for exactly as long as if_config() hangs that one. */
static bool StopNetworkThread()
{
	if(networkthread == LWP_THREAD_NULL)
		return true;

	netHalt = 2;

	if(LWP_ThreadIsSuspended(networkthread))
		LWP_ResumeThread(networkthread);

	for(int i = 0; i < NET_STOP_TIMEOUT_SECS * 50 && !netThreadExited; i++)
		usleep(20 * 1000);

	if(!netThreadExited)
	{
		if(!netThreadAbandoned)
			DebugMark("net: the worker is still inside if_config() -- abandoned, not killed");

		netThreadAbandoned = true;
		return false;
	}

	LWP_JoinThread(networkthread, NULL);
	networkthread = LWP_THREAD_NULL;
	netThreadAbandoned = false;
	return true;
}

extern "C"{
static char playbackNetworkError[160];
void MPlayerNetworkError(const char *message)
{
	snprintf(playbackNetworkError, sizeof(playbackNetworkError), "%s", message ? message : "");
	if(playbackNetworkError[0]) DebugMark("network playback: %s", playbackNetworkError);
}
const char *MPlayerGetNetworkError(void) { return playbackNetworkError; }
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

/* The first screen waits for an address, but no longer waits for ever.
 *
 * ShowAction() disables mainWindow while the progress and GUI threads keep
 * drawing the throbber.  After NET_BOOT_TIMEOUT_SECS the user is offered the
 * choice the old gate never had: try again, or go on without a network.
 *
 * That choice is what makes the console usable at all.  Everything
 * downstream of this call -- LoadSettings(), InitMPlayer(), the card browser,
 * local playback -- used to sit behind a wait that a silent DHCP server could
 * hold open indefinitely, so one unanswered broadcast took the whole
 * application with it.
 *
 * Returns true only with a usable address. */
bool WaitForNetworkAtBoot()
{
#ifndef WANT_NETWORK
	return false;
#else
	if(networkInit)
		return true;

	DebugMark("net: boot gate waiting up to %d s for a usable IP address",
		NET_BOOT_TIMEOUT_SECS);

	while(!networkInit && !ExitRequested)
	{
		u64 started = gettime();

		ShowAction("Initializing network, please wait...");
		StartNetworkThread();

		u64 noted = started;

		while(!networkInit && !ExitRequested &&
			networkthread != LWP_THREAD_NULL &&
			!LWP_ThreadIsSuspended(networkthread) &&
			diff_sec(started, gettime()) < NET_BOOT_TIMEOUT_SECS)
		{
			usleep(50 * 1000);

			if(diff_sec(noted, gettime()) >= NET_WAIT_NOTE_SECS)
			{
				/* What happens inside if_configex() cannot be reached from
				 * here, but the one thing its loop is waiting for can be
				 * read: net_gethostip() is a single load from the netif that
				 * DHCP fills in, safe from any thread at any time. An
				 * address appearing here while the probe keeps running would
				 * mean the loop is stuck on something other than the
				 * address; all zeroes to the end means nothing answered. */
				u32 ip = net_gethostip();

				noted = gettime();
				DebugMark("net: waiting %u s -- netif ip %u.%u.%u.%u",
					(unsigned)diff_sec(started, gettime()),
					(unsigned)((ip >> 24) & 0xff), (unsigned)((ip >> 16) & 0xff),
					(unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));
			}
		}

		CancelAction();

		if(networkInit || ExitRequested)
			break;

		if(StopNetworkThread())
			DebugMark("net: boot gate -- the probe gave up without an address");
		else
			DebugMark("net: boot gate gave up after %u s -- the probe is still running",
				(unsigned)diff_sec(started, gettime()));

		if(WindowPrompt("Network",
			"No address yet. The adapter may be missing, the cable unplugged, "
			"or the DHCP server silent. You can continue without a network and "
			"play from the card, or set netStaticIP in settings.xml to skip "
			"DHCP entirely.",
			"Retry", "Continue offline") == 0)
		{
			DebugMark("net: boot gate released offline at the user's request");
			break;
		}
	}

	if(networkInit)
	{
		StopNetworkThread();
		DebugMark("net: boot gate released on IP %s", wiiIP);
	}
	else if(ExitRequested)
	{
		DebugMark("net: boot gate left during shutdown");
	}

	return networkInit;
#endif
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

	if(!networkInit)
	{
		StartNetworkThread();

		while (networkthread != LWP_THREAD_NULL && !LWP_ThreadIsSuspended(networkthread) && !cancelNetworkInit)
			usleep(50 * 1000);

		StopNetworkThread();
	}

	CancelAction();
	if(!networkInit && !silent && !cancelNetworkInit)
		ErrorPrompt("Network unavailable. Check the adapter and cable, then restart WiiMC.");

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
