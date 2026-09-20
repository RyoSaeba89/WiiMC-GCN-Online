/* Exercise the actual auto-advance decision, the one that left the second
 * song of a folder unplayed: FindNextFile() plus the folder queue, compiled
 * from source/wiimc.cpp, and the real GetExt/IsAudioExt/GetFullPath from
 * source/fileop.cpp. Only the browser, the playlist and the loader are
 * simulated here. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MAXPATHLEN 1024

enum { TYPE_FILE, TYPE_FOLDER, TYPE_PLAYLIST, TYPE_SEARCH };

enum {
	ICON_NONE, ICON_FOLDER, ICON_FOLDER_CHECKED, ICON_FILE,
	ICON_FILE_CHECKED, ICON_CHECK, ICON_PLAY
};

enum { PLAY_SINGLE, PLAY_CONTINUOUS, PLAY_SHUFFLE, PLAY_LOOP, PLAY_THROUGH };

enum {
	MENU_BROWSE_VIDEOS, MENU_BROWSE_MUSIC, MENU_DVD, MENU_SETTINGS,
	MENU_BROWSE_ONLINEMEDIA
};

typedef struct _bentry
{
	unsigned long long length;
	int type;
	char *file;
	char *url;
	char *display;
	char *xml;
	char *image;
	char *tunein;
	char *year;
	char *desc;
	int icon;
	int pos;
	_bentry *next;
	_bentry *prior;
} BROWSERENTRY;

typedef struct
{
	char dir[MAXPATHLEN];
	int menu;
	char lastdir[MAXPATHLEN];
	int numEntries;
	BROWSERENTRY *selIndex;
	int pageIndex;
	BROWSERENTRY *first;
	BROWSERENTRY *last;
} BROWSER;

static BROWSER browser, browserMusic;
static int menuCurrent = MENU_BROWSE_MUSIC;
static bool menuMode = 0;
static bool nowPlayingSet, selectLoadedFile;
static int findLoadedFile;
static int controlledbygui = 1; /* audio: the GUI keeps the screen */
static char loadedFile[MAXPATHLEN];

static struct { int audioNorm; int playOrder; } WiiSettings;

static int loads;
static char lastLoaded[MAXPATHLEN];

static void wiiSetVolNorm(void) {}
static void FindFile(void) {}
static char *GetPartitionLabel(char *path) { (void)path; return NULL; }

static void wiiLoadFile(char *filename, char *partitionlabel)
{
	(void)partitionlabel;
	++loads;
	snprintf(lastLoaded, sizeof(lastLoaded), "%s", filename);
}

static void DebugMark(const char *fmt, ...) { (void)fmt; }

/* A browser entry belongs to the playlist when the playlist holds its path. */
static bool MusicPlaylistFind(BROWSERENTRY *index);

/* The real ones, lifted from source/fileop.cpp. FindDevice() only has to tell
 * a bare file name from an already complete path here. */
static bool FindDevice(char *filepath, int *device, int *devnum)
{
	(void)device;
	(void)devnum;
	return filepath && strstr(filepath, ":/") != NULL;
}

/* settings.h owns this table; the test uses the production one. */
#include "out/folder_queue_extensions.inc"

#include "out/folder_queue_fileop.inc"

static BROWSERENTRY *shuffleReturn;
static BROWSERENTRY *MusicPlaylistGetNextShuffle(void) { return shuffleReturn; }

/* The code under test. */
#include "out/folder_queue_wiimc.inc"

static bool MusicPlaylistFind(BROWSERENTRY *index)
{
	if(!index)
		return false;

	char path[MAXPATHLEN];
	GetFullPath(index, path);

	for(BROWSERENTRY *i = browserMusic.first; i != NULL; i = i->next)
		if(strcmp(i->file, path) == 0)
			return true;

	return false;
}

/****************************************************************************
 * Browser scaffolding
 ***************************************************************************/
static void ResetBrowser(BROWSER *b)
{
	BROWSERENTRY *i = b->first;

	while(i)
	{
		BROWSERENTRY *n = i->next;
		free(i->file);
		free(i);
		i = n;
	}

	memset(b, 0, sizeof(*b));
}

static BROWSERENTRY *AddEntry(BROWSER *b, const char *name, int type)
{
	BROWSERENTRY *e = (BROWSERENTRY *)calloc(1, sizeof(BROWSERENTRY));
	assert(e);
	e->file = strdup(name);
	assert(e->file);
	e->type = type;
	e->icon = (type == TYPE_FOLDER) ? ICON_FOLDER : ICON_FILE;
	e->prior = b->last;

	if(b->last)
		b->last->next = e;
	else
		b->first = e;

	b->last = e;
	b->numEntries++;
	return e;
}

static const char *kDir = "dav1:/Angry Video Game Nerd Adventures- OST/";

/* The listing the console actually had: a parent entry, three songs, and two
 * files that are not music. */
static void GivenTheFolder(void)
{
	ResetBrowser(&browser);
	ResetBrowser(&browserMusic);
	snprintf(browser.dir, sizeof(browser.dir), "%s", kDir);
	browser.menu = MENU_BROWSE_MUSIC;
	AddEntry(&browser, "..", TYPE_FOLDER);
	AddEntry(&browser, "01 AVGN Adventures Main Theme.mp3", TYPE_FILE);
	AddEntry(&browser, "02 The Board.mp3", TYPE_FILE);
	AddEntry(&browser, "03 Assholevania.mp3", TYPE_FILE);
	AddEntry(&browser, "Bonus", TYPE_FOLDER);
	AddEntry(&browser, "cover.jpg", TYPE_FILE);
	AddEntry(&browser, "notes.txt", TYPE_FILE);

	menuCurrent = MENU_BROWSE_MUSIC;
	menuMode = 0;
	controlledbygui = 1;
	loads = 0;
	lastLoaded[0] = 0;
	snprintf(loadedFile, sizeof(loadedFile), "%s01 AVGN Adventures Main Theme.mp3", kDir);
	BuildFolderQueue();
}

static void ExpectTrack(const char *name)
{
	char want[MAXPATHLEN];
	snprintf(want, sizeof(want), "%s%s", kDir, name);

	if(strcmp(loadedFile, want) != 0)
	{
		printf("FAIL: expected '%s', got '%s'\n", want, loadedFile);
		abort();
	}
}

static int trackNumber(void)
{
	return atoi(loadedFile + strlen(kDir));
}

/****************************************************************************
 * Cases
 ***************************************************************************/
static void the_folder_becomes_the_queue(void)
{
	GivenTheFolder();

	/* three songs, the jpg, the txt, the folders and '..' left out */
	assert(FolderQueueCount() == 3);

	WiiSettings.playOrder = PLAY_CONTINUOUS;

	assert(FindNextFile(true));
	ExpectTrack("02 The Board.mp3");
	assert(loads == 1);
	assert(strcmp(lastLoaded, loadedFile) == 0);

	assert(FindNextFile(true));
	ExpectTrack("03 Assholevania.mp3");

	/* continuous wraps */
	assert(FindNextFile(true));
	ExpectTrack("01 AVGN Adventures Main Theme.mp3");
	assert(loads == 3);
}

static void a_song_started_in_the_middle_carries_on_from_there(void)
{
	GivenTheFolder();
	snprintf(loadedFile, sizeof(loadedFile), "%s02 The Board.mp3", kDir);
	BuildFolderQueue();

	WiiSettings.playOrder = PLAY_CONTINUOUS;
	assert(FindNextFile(true));
	ExpectTrack("03 Assholevania.mp3");
}

static void single_stops_unless_asked(void)
{
	GivenTheFolder();
	WiiSettings.playOrder = PLAY_SINGLE;

	assert(!FindNextFile(true));
	assert(loads == 0);
	ExpectTrack("01 AVGN Adventures Main Theme.mp3");

	/* the transport button is a command, not auto-advance */
	RequestNextFile();
	assert(FindNextFile(true));
	ExpectTrack("02 The Board.mp3");

	/* and it is consumed */
	assert(!FindNextFile(true));
}

static void through_stops_at_the_end(void)
{
	GivenTheFolder();
	WiiSettings.playOrder = PLAY_THROUGH;

	assert(FindNextFile(true));
	ExpectTrack("02 The Board.mp3");
	assert(FindNextFile(true));
	ExpectTrack("03 Assholevania.mp3");
	assert(!FindNextFile(true));
	ExpectTrack("03 Assholevania.mp3");
	assert(loads == 2);
}

static void loop_repeats_the_same_song(void)
{
	GivenTheFolder();
	WiiSettings.playOrder = PLAY_LOOP;

	assert(FindNextFile(true));
	ExpectTrack("01 AVGN Adventures Main Theme.mp3");
	assert(FindNextFile(true));
	ExpectTrack("01 AVGN Adventures Main Theme.mp3");
	assert(loads == 2);

	RequestNextFile();
	assert(FindNextFile(true));
	ExpectTrack("02 The Board.mp3");
}

static void shuffle_stays_inside_the_folder(void)
{
	GivenTheFolder();
	WiiSettings.playOrder = PLAY_SHUFFLE;

	int seen[4] = { 0 };
	int previous = 1;

	for(int i = 0; i < 300; i++)
	{
		assert(FindNextFile(true));
		int n = trackNumber();
		assert(n >= 1 && n <= 3);
		assert(n != previous); /* a shuffle that repeats is a stuck song */
		seen[n]++;
		previous = n;
	}

	assert(seen[1] > 0 && seen[2] > 0 && seen[3] > 0);
}

static void a_real_playlist_wins(void)
{
	GivenTheFolder();

	char path[MAXPATHLEN];
	snprintf(path, sizeof(path), "%ssomewhere else.mp3", kDir);
	AddEntry(&browserMusic, path, TYPE_FILE);
	snprintf(path, sizeof(path), "%sand another.mp3", kDir);
	AddEntry(&browserMusic, path, TYPE_FILE);

	BuildFolderQueue(); /* the user has a queue of their own */
	assert(FolderQueueCount() == 0);

	browserMusic.selIndex = browserMusic.first;
	WiiSettings.playOrder = PLAY_CONTINUOUS;

	assert(FindNextFile(true));
	ExpectTrack("and another.mp3");
}

static void browsing_away_does_not_stop_the_music(void)
{
	GivenTheFolder();
	WiiSettings.playOrder = PLAY_CONTINUOUS;

	/* the user walks off to another folder while the song plays: the browser
	 * list is freed and rebuilt under the MPlayer thread */
	ResetBrowser(&browser);
	snprintf(browser.dir, sizeof(browser.dir), "dav1:/Somewhere Else/");
	AddEntry(&browser, "..", TYPE_FOLDER);
	AddEntry(&browser, "unrelated.mp3", TYPE_FILE);

	assert(FindNextFile(true));
	ExpectTrack("02 The Board.mp3");
}

static void a_song_outside_the_listing_advances_to_nothing(void)
{
	GivenTheFolder();
	snprintf(loadedFile, sizeof(loadedFile), "http://example.net/stream.mp3");
	BuildFolderQueue();

	assert(FolderQueueCount() == 0);
	WiiSettings.playOrder = PLAY_CONTINUOUS;
	assert(!FindNextFile(true));
	assert(loads == 0);
}

static void a_stop_request_beats_every_mode(void)
{
	GivenTheFolder();
	WiiSettings.playOrder = PLAY_CONTINUOUS;
	controlledbygui = 2; /* the user pressed stop */

	assert(!FindNextFile(true));
	assert(loads == 0);
}

static void the_queue_is_bounded(void)
{
	ResetBrowser(&browser);
	ResetBrowser(&browserMusic);
	snprintf(browser.dir, sizeof(browser.dir), "dav1:/Everything/");
	menuCurrent = MENU_BROWSE_MUSIC;
	controlledbygui = 1;
	loads = 0;

	char name[64];
	for(int i = 0; i < FOLDER_QUEUE_MAX + 500; i++)
	{
		snprintf(name, sizeof(name), "%05d.mp3", i);
		AddEntry(&browser, name, TYPE_FILE);
	}

	snprintf(loadedFile, sizeof(loadedFile), "dav1:/Everything/00000.mp3");
	BuildFolderQueue();
	assert(FolderQueueCount() == FOLDER_QUEUE_MAX);

	WiiSettings.playOrder = PLAY_CONTINUOUS;
	assert(FindNextFile(true));
	assert(strcmp(loadedFile, "dav1:/Everything/00001.mp3") == 0);
	ClearFolderQueue();
}

static void without_loading_it_only_picks(void)
{
	GivenTheFolder();
	WiiSettings.playOrder = PLAY_CONTINUOUS;

	assert(FindNextFile(false));
	ExpectTrack("02 The Board.mp3");
	assert(loads == 0);
}

int main(void)
{
	srand(1);

	struct { const char *name; void (*run)(void); } cases[] = {
		{ "the folder becomes the queue", the_folder_becomes_the_queue },
		{ "a song started in the middle carries on", a_song_started_in_the_middle_carries_on_from_there },
		{ "single stops unless asked", single_stops_unless_asked },
		{ "through stops at the end", through_stops_at_the_end },
		{ "loop repeats the same song", loop_repeats_the_same_song },
		{ "shuffle stays inside the folder", shuffle_stays_inside_the_folder },
		{ "a real playlist wins", a_real_playlist_wins },
		{ "browsing away does not stop the music", browsing_away_does_not_stop_the_music },
		{ "a song outside the listing advances to nothing", a_song_outside_the_listing_advances_to_nothing },
		{ "a stop request beats every mode", a_stop_request_beats_every_mode },
		{ "the queue is bounded", the_queue_is_bounded },
		{ "without loading it only picks", without_loading_it_only_picks },
	};

	for(unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		cases[i].run();
		printf("PASS %s\n", cases[i].name);
	}

	ResetBrowser(&browser);
	ResetBrowser(&browserMusic);
	ClearFolderQueue();
	printf("PASS %u folder queue cases\n", (unsigned)(sizeof(cases) / sizeof(cases[0])));
	return 0;
}
