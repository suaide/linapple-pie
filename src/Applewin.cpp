/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2007, Tom Charlesworth, Michael Pohoreski

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: main
 *
 * Author: Various
 */

/* Adaptation for SDL and POSIX (l) by beom beotiger, Nov-Dec 2007, krez beotiger March 2012 AD */
/*

Linappple-pie was adapted in OCT 2015 for use with Retropie.
By Mark Ormond.
*/


#include "stdafx.h"
#include "MouseInterface.h"
// for time logging
#include <time.h>
#include <sys/time.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <getopt.h>
#include <strings.h>
#include <fstream>

TCHAR *g_pAppTitle = TITLE_APPLE_2E_ENHANCED;

eApple2Type	g_Apple2Type	= A2TYPE_APPLE2EEHANCED;

int          opt;
bool argdisks = false;
bool argdisks2 = false;
bool autoboot = false;
bool fullscreenboot = false;
bool disablecursor = false;
BOOL      behind            = 0;			// Redundant
DWORD     cumulativecycles  = 0;			// Wraps after ~1hr 9mins
DWORD     cyclenum          = 0;			// Used by SpkrToggle() for non-wave sound
DWORD     emulmsec          = 0;
static DWORD emulmsec_frac  = 0;
bool      g_bFullSpeed      = false;
bool hddenabled = false;
static bool g_uMouseInSlot4 = false;	// not any mouse in slot4??--bb
char *Disk1="blank.dsk";
char *Disk2="blank.dsk";

AppMode_e	g_nAppMode = MODE_LOGO;

// Default screen sizes
// SCREEN_WIDTH & SCREEN_HEIGHT defined in Frame.h
UINT g_ScreenWidth = SCREEN_WIDTH;
UINT g_ScreenHeight = SCREEN_HEIGHT;

//static int lastmode         = MODE_LOGO;		-- not used???
DWORD     needsprecision    = 0;			// Redundant
TCHAR     g_sCurrentDir[MAX_PATH] = TEXT(""); // Also Starting Dir for Slot6 disk images?? --bb
TCHAR     g_sHDDDir[MAX_PATH] = TEXT(""); // starting dir for HDV (Apple][ HDD) images?? --bb
TCHAR     g_sSaveStateDir[MAX_PATH] = TEXT(""); // starting dir for states --bb
TCHAR     g_sParallelPrinterFile[MAX_PATH] = TEXT("Printer.txt");	// default file name for Parallel printer

// FTP Variables
TCHAR     g_sFTPLocalDir[MAX_PATH] = TEXT(""); // FTP Local Dir, see linapple.conf for details
TCHAR     g_sFTPServer[MAX_PATH] = TEXT(""); // full path to default FTP server
TCHAR     g_sFTPServerHDD[MAX_PATH] = TEXT(""); // full path to default FTP server
TCHAR     g_sFTPUserPass[512] = TEXT("anonymous:mymail@hotmail.com"); // full login line

bool      g_bResetTiming    = false;			// Redundant
BOOL      restart           = 0;

// several parameters affecting the speed of emulated CPU
DWORD		g_dwSpeed		= SPEED_NORMAL;	// Affected by Config dialog's speed slider bar
double		g_fCurrentCLK6502 = CLK_6502;	// Affected by Config dialog's speed slider bar
static double g_fMHz		= 1.0;			// Affected by Config dialog's speed slider bar

int	g_nCpuCyclesFeedback = 0;
DWORD   g_dwCyclesThisFrame = 0;

FILE*		g_fh			= NULL; // file for logging, let's use stderr instead?
bool		g_bDisableDirectSound = false;  // direct sound, use SDL Sound, or SDL_mixer???

CSuperSerialCard	sg_SSC;
CMouseInterface		sg_Mouse;

UINT	g_Slot4 = CT_Mockingboard;	// CT_Mockingboard or CT_MouseInterface

CURL *g_curl = NULL;	// global easy curl resourse
//===========================================================================

// ???? what is DBG_CALC_FREQ???  O_O   --bb
#define DBG_CALC_FREQ 0
#if DBG_CALC_FREQ
const UINT MAX_CNT = 256;
double g_fDbg[MAX_CNT];
UINT g_nIdx = 0;
double g_fMeanPeriod,g_fMeanFreq;
ULONG g_nPerfFreq = 0;
#endif



//---------------------------------------------------------------------------

void ContinueExecution()
{
    static BOOL pageflipping    = 0; //?

    const double fUsecPerSec        = 1.e6;

    const UINT nExecutionPeriodUsec = 1000;		// 1.0ms
    const double fExecutionPeriodClks = g_fCurrentCLK6502 * ((double)nExecutionPeriodUsec / fUsecPerSec);

    bool bScrollLock_FullSpeed = g_bScrollLock_FullSpeed; //g_uScrollLockToggle;

    g_bFullSpeed = ( (g_dwSpeed == SPEED_MAX) ||
                     bScrollLock_FullSpeed ||
                     (DiskIsSpinning() && enhancedisk && !Spkr_IsActive() && !MB_IsActive()) );

    if(g_bFullSpeed)
    {
        // Don't call Spkr_Mute() - will get speaker clicks
        MB_Mute();
        SysClk_StopTimer();
        g_nCpuCyclesFeedback = 0;	// For the case when this is a big -ve number
        //		SetPriorityNormal();
    }
    else
    {
        // Don't call Spkr_Demute()
        MB_Demute();
        SysClk_StartTimerUsec(nExecutionPeriodUsec);
        // Switch to higher priority, eg. for audio (BUG #015394)
        //		SetPriorityAboveNormal();
    }

    //

    int nCyclesToExecute = (int) fExecutionPeriodClks + g_nCpuCyclesFeedback;
    if(nCyclesToExecute < 0)
        nCyclesToExecute = 0;

    DWORD dwExecutedCycles = CpuExecute(nCyclesToExecute);
    g_dwCyclesThisFrame += dwExecutedCycles;

    //

    cyclenum = dwExecutedCycles;

    DiskUpdatePosition(dwExecutedCycles);
    JoyUpdatePosition();
    // the next call does not present	in current Applewin as on March 2012??
    VideoUpdateVbl(g_dwCyclesThisFrame);

    SpkrUpdate(cyclenum);
    sg_SSC.CommUpdate(cyclenum);
    PrintUpdate(cyclenum);

    //

    const DWORD CLKS_PER_MS = (DWORD)g_fCurrentCLK6502 / 1000;

    emulmsec_frac += dwExecutedCycles;
    if(emulmsec_frac > CLKS_PER_MS)
    {
        emulmsec += emulmsec_frac / CLKS_PER_MS;
        emulmsec_frac %= CLKS_PER_MS;
    }

    //
    // DETERMINE WHETHER THE SCREEN WAS UPDATED, THE DISK WAS SPINNING,
    // OR THE KEYBOARD I/O PORTS WERE BEING EXCESSIVELY QUERIED THIS CLOCKTICK
    VideoCheckPage(0);
    BOOL screenupdated = VideoHasRefreshed();
    BOOL systemidle    = 0;	//(KeybGetNumQueries() > (clockgran << 2));	//  && (!ranfinegrain);	// TO DO

    if(screenupdated)
        pageflipping = 3;

    //

    if(g_dwCyclesThisFrame >= dwClksPerFrame)
    {
        g_dwCyclesThisFrame -= dwClksPerFrame;

        if(g_nAppMode != MODE_LOGO)
        {
            VideoUpdateFlash();

            static BOOL  anyupdates     = 0;
            static DWORD lastcycles     = 0;
            static BOOL  lastupdates[2] = {0,0};

            anyupdates |= screenupdated;

            //

            lastcycles = cumulativecycles;
            if ((!anyupdates) && (!lastupdates[0]) && (!lastupdates[1]) && VideoApparentlyDirty())
            {
                VideoCheckPage(1);
                static DWORD lasttime = 0;
                DWORD currtime = GetTickCount();
                if ((!g_bFullSpeed) ||
                        (currtime-lasttime >= (DWORD)((graphicsmode || !systemidle) ? 100 : 25)))
                {
                    VideoRefreshScreen();
                    lasttime = currtime;
                }
                screenupdated = 1;
            }

            lastupdates[1] = lastupdates[0];
            lastupdates[0] = anyupdates;
            anyupdates     = 0;

            if (pageflipping)
                pageflipping--;
        }

        MB_EndOfVideoFrame();
    }

    //

    if(!g_bFullSpeed)
    {
        SysClk_WaitTimer();

#if DBG_CALC_FREQ
        if(g_nPerfFreq)
        {
            //QueryPerformanceCounter((LARGE_INTEGER*)&nTime1); QueryPerformanceFrequency
            LONG nTime1 = GetTickCount();//no QueryPerformanceCounter and alike
            LONG nTimeDiff = nTime1 - nTime0;
            double fTime = (double)nTimeDiff / (double)(LONG)g_nPerfFreq;

            g_fDbg[g_nIdx] = fTime;
            g_nIdx = (g_nIdx+1) & (MAX_CNT-1);
            g_fMeanPeriod = 0.0;
            for(UINT n=0; n<MAX_CNT; n++)
                g_fMeanPeriod += g_fDbg[n];
            g_fMeanPeriod /= (double)MAX_CNT;
            g_fMeanFreq = 1.0 / g_fMeanPeriod;
        }
#endif
    }
}

//===========================================================================

void SetCurrentCLK6502()
{
    static DWORD dwPrevSpeed = (DWORD) -1;

    if(dwPrevSpeed == g_dwSpeed)
        return;

    dwPrevSpeed = g_dwSpeed;

    // SPEED_MIN    =  0 = 0.50 MHz
    // SPEED_NORMAL = 10 = 1.00 MHz
    //                20 = 2.00 MHz
    // SPEED_MAX-1  = 39 = 3.90 MHz
    // SPEED_MAX    = 40 = ???? MHz (run full-speed, /g_fCurrentCLK6502/ is ignored)


    if(g_dwSpeed < SPEED_NORMAL)
        g_fMHz = 0.5 + (double)g_dwSpeed * 0.05;
    else
        g_fMHz = (double)g_dwSpeed / 10.0;

    g_fCurrentCLK6502 = CLK_6502 * g_fMHz;

    //
    // Now re-init modules that are dependent on /g_fCurrentCLK6502/
    //

    SpkrReinitialize();
    MB_Reinitialize();
}

//===========================================================================
void EnterMessageLoop ()
{
    //	MSG message;
    SDL_Event event;

    //	PeekMessage(&message, NULL, 0, 0, PM_NOREMOVE);
    while(true)

        //	while (message.message!=WM_QUIT)
    {
        if(SDL_PollEvent(&event))
        {
            if(event.type == SDL_QUIT && event.key.keysym.sym != SDLK_F4) return;
            FrameDispatchMessage(&event);

            while ((g_nAppMode == MODE_RUNNING) || (g_nAppMode == MODE_STEPPING))
            {
                if(SDL_PollEvent(&event)) {
                    if(event.type == SDL_QUIT && event.key.keysym.sym != SDLK_F4) return;
                    FrameDispatchMessage(&event);
                }
                else if (g_nAppMode == MODE_STEPPING)
                {
                    DebugContinueStepping();
                }
                else
                {
                    ContinueExecution();
                    if (g_nAppMode != MODE_DEBUG)
                    {
                        if (joyexitenable)
                        {
                            CheckJoyExit();
                            if (joyquitevent) return;
                        }
                        if (g_bFullSpeed)
                            ContinueExecution();
                    }
                }
            }
        }
        else
        {
            if (g_nAppMode == MODE_DEBUG)
                DebuggerUpdate();
            else if (g_nAppMode == MODE_LOGO || g_nAppMode == MODE_PAUSED)
                SDL_Delay(100);		// Stop process hogging CPU
        }
    }
}

//---------------------------------------------------------------------------

int DoDiskInsert(int nDrive, LPSTR szFileName)
{
    return DiskInsert(nDrive, szFileName, 0, 0);
}

//===========================================================================
// Let us load main configuration from config file.  Y_Y  --bb
void LoadConfiguration ()
{
    DWORD dwComputerType;
    LOAD(TEXT("Computer Emulation"),&dwComputerType);
    switch (dwComputerType)
    {
    // NB. No A2TYPE_APPLE2E

    case 0:
        g_Apple2Type = A2TYPE_APPLE2;
        break;
    case 1:
        g_Apple2Type = A2TYPE_APPLE2PLUS;
        break;
    case 2:
        g_Apple2Type = A2TYPE_APPLE2EEHANCED;
        break;
    default:
        g_Apple2Type = A2TYPE_APPLE2EEHANCED;
        break;
    }
    //  }
    // determine Apple type and set appropriate caption -- should be in (F9)switching modes?
    switch (g_Apple2Type)
    {
    case A2TYPE_APPLE2:
        g_pAppTitle = TITLE_APPLE_2;
        break;
    case A2TYPE_APPLE2PLUS:
        g_pAppTitle = TITLE_APPLE_2_PLUS;
        break;
    case A2TYPE_APPLE2E:
        g_pAppTitle = TITLE_APPLE_2E;
        break;
    case A2TYPE_APPLE2EEHANCED:
        g_pAppTitle = TITLE_APPLE_2E_ENHANCED;
        break;
    }
    // Load Joystick values
    joytype[0] = 2;
    joytype[1] = 0;
    LOAD(TEXT("Joystick 0"),&joytype[0]);
    LOAD(TEXT("Joystick 1"),&joytype[1]);
    LOAD(TEXT("Joy0Index"),&joy1index);
    LOAD(TEXT("Joy1Index"),&joy2index);

    LOAD(TEXT("Joy0Button1"),&joy1button1);
    LOAD(TEXT("Joy0Button2"),&joy1button2);
    LOAD(TEXT("Joy1Button1"),&joy2button1);

    LOAD(TEXT("Joy0Axis0"),&joy1axis0);
    LOAD(TEXT("Joy0Axis1"),&joy1axis1);
    LOAD(TEXT("Joy1Axis0"),&joy2axis0);
    LOAD(TEXT("Joy1Axis1"),&joy2axis1);
    LOAD(TEXT("JoyExitEnable"),&joyexitenable);
    LOAD(TEXT("JoyExitButton0"),&joyexitbutton0);
    LOAD(TEXT("JoyExitButton1"),&joyexitbutton1);


    if (joytype[0]==1 ) printf ("Joystick 1 Index # = %i, Name = %s \nButton 1 = %i, Button 2 = %i \nAxis 0 = %i,Axis 1 = %i\n",joy1index,SDL_JoystickName(joy1index),joy1button1, joy1button2,joy1axis0,joy1axis1);
    if (joytype[1]==1 )printf ("Joystick 2 Index # = %i, Name = %s \nButton 1 = %i \nAxis 0 = %i,Axis 1 = %i\n",joy2index,SDL_JoystickName(joy2index),joy2button1,joy2axis0,joy2axis1);

    LOAD(TEXT("Sound Emulation"),&soundtype);

    DWORD dwSerialPort;
    LOAD(TEXT("Serial Port"),&dwSerialPort);
    sg_SSC.SetSerialPort(dwSerialPort); // ----------- why it is here????

    LOAD(TEXT("Emulation Speed"),&g_dwSpeed);

    LOAD(TEXT("Enhance Disk Speed"),(DWORD *)&enhancedisk);//
    LOAD(TEXT("Video Emulation"),&videotype);
    //  printf("Video Emulation = %d\n", videotype);

    DWORD dwTmp = 0;	// temp var

    LOAD(TEXT("Fullscreen"),&dwTmp);	// load fullscreen flag
    fullscreen = (BOOL) dwTmp;
    if (fullscreenboot) fullscreen = true;

    LOAD(TEXT("DisableCursor"),&dwTmp);	// load Disable Cursor Flag
    disablecursor = (BOOL) dwTmp;


    dwTmp = 1;
    LOAD(TEXT(REGVALUE_SHOW_LEDS),&dwTmp);	// load Show Leds flag
    g_ShowLeds = (BOOL) dwTmp;

    //printf("Fullscreen = %d\n", fullscreen);
    //  LOAD(TEXT("Uthernet Active")  ,(DWORD *)&tfe_enabled);

    SetCurrentCLK6502();	// set up real speed

    if(LOAD(TEXT(REGVALUE_MOUSE_IN_SLOT4), &dwTmp))
        g_uMouseInSlot4 = dwTmp;
    g_Slot4 = g_uMouseInSlot4 ? CT_MouseInterface : CT_Mockingboard;

    if(LOAD(TEXT(REGVALUE_SOUNDCARD_TYPE), &dwTmp))
        MB_SetSoundcardType((eSOUNDCARDTYPE)dwTmp);

    if(LOAD(TEXT(REGVALUE_SAVE_STATE_ON_EXIT), &dwTmp))
        g_bSaveStateOnExit = dwTmp ? true : false;

    if(LOAD(TEXT(REGVALUE_HDD_ENABLED), &dwTmp)) hddenabled = (bool) dwTmp;// after MemInitialize

    char *szHDFilename = NULL;

    if(RegLoadString(TEXT("Configuration"), TEXT("Monochrome Color"), 1, &szHDFilename, 10))
    {
        if (!sscanf(szHDFilename, "#%X", &monochrome)) monochrome = 0xC0C0C0;
        free(szHDFilename);
        szHDFilename = NULL;
    }

    dwTmp = 0;
    LOAD(TEXT("Boot at Startup"),&dwTmp);	//
    if ((dwTmp) || (autoboot))
    {
        // autostart
        SDL_Event user_ev;
        user_ev.type = SDL_USEREVENT;
        user_ev.user.code = 1;	//restart?
        SDL_PushEvent(&user_ev);
    }

    dwTmp = 0;
    LOAD(TEXT("Slot 6 Autoload"),&dwTmp);	// load autoinsert for Slot 6 flag
    if(dwTmp &&!autoboot) {
        // Load floppy disk images and insert it automatically in slot 6 drive 1 and 2
        if(RegLoadString(TEXT("Configuration"), TEXT(REGVALUE_DISK_IMAGE1), 1, &szHDFilename, MAX_PATH))
        {
            DoDiskInsert(0, szHDFilename);
            free(szHDFilename);
            szHDFilename = NULL;
        }
        if(RegLoadString(TEXT("Configuration"), TEXT(REGVALUE_DISK_IMAGE2), 1, &szHDFilename, MAX_PATH))
        {
            DoDiskInsert(1, szHDFilename);
            free(szHDFilename);
            szHDFilename = NULL;
        }
    }
    else {

        if (argdisks)  {
            DoDiskInsert(0, Disk1);
        }
        else
        {

            const char* home = getenv("HOME");
            std::string MASTER_DISKstr(home);
            MASTER_DISKstr += "/.linapple/Master.dsk";
            const char * MasterDiskLocation =  MASTER_DISKstr.c_str();
            ifstream ifile2 (MasterDiskLocation);

            if (ifile2) {
                char *MasterDisk= new char[MASTER_DISKstr.length() + 1];
                strcpy(MasterDisk, MASTER_DISKstr.c_str());
                DoDiskInsert(0, MasterDisk);
            }
            else  {
                char *MasterDisk = "Master.dsk";
                DoDiskInsert(0, MasterDisk);
            }
        }
        if (argdisks2)  DoDiskInsert(1, Disk2);
    }
    // Load hard disk images and insert it automatically in slot 7
    if(RegLoadString(TEXT("Configuration"), TEXT(REGVALUE_HDD_IMAGE1), 1, &szHDFilename, MAX_PATH))
    {
        //	  printf("LoadConfiguration: returned string is: %s\n", szHDFilename);
        HD_InsertDisk2(0, szHDFilename);
        free(szHDFilename);
        szHDFilename = NULL;
    }
    if(RegLoadString(TEXT("Configuration"), TEXT(REGVALUE_HDD_IMAGE2), 1, &szHDFilename, MAX_PATH))
    {
        //	  printf("LoadConfiguration: returned string is: %s\n", szHDFilename);
        HD_InsertDisk2(1, szHDFilename);
        free(szHDFilename);
        szHDFilename = NULL;
    }

    // file name for Parallel Printer
    if(RegLoadString(TEXT("Configuration"), TEXT(REGVALUE_PPRINTER_FILENAME), 1, &szHDFilename, MAX_PATH))
    {
        if(strlen(szHDFilename) > 1) strncpy(g_sParallelPrinterFile, szHDFilename, MAX_PATH);
        free(szHDFilename);
        szHDFilename = NULL;
    }

    char *szFilename = NULL;
    double scrFactor = 0.0;
    // Define screen sizes
    if (RegLoadString(TEXT("Configuration"),TEXT("Screen factor"),1, &szFilename,16)) {
        scrFactor =  atof(szFilename);
        if(scrFactor > 0.1) {
            g_ScreenWidth = UINT(g_ScreenWidth * scrFactor);
            g_ScreenHeight = UINT(g_ScreenHeight * scrFactor);
        }
        free(szFilename);
        szFilename = NULL;
    }
    else {	// Try to set Screen Width & Height directly
        dwTmp = 0;
        LOAD(TEXT("Screen Width"),&dwTmp);
        if(dwTmp > 0) g_ScreenWidth = dwTmp;
        dwTmp = 0;
        LOAD(TEXT("Screen Height"),&dwTmp);
        if(dwTmp > 0) g_ScreenHeight = dwTmp;
    }

    if (RegLoadString(TEXT("Configuration"),TEXT(REGVALUE_SAVESTATE_FILENAME),1, &szFilename,MAX_PATH)) {
        Snapshot_SetFilename(szFilename);	// If not in Registry than default will be used
        free(szFilename);
        szFilename = NULL;
    }

    // Current/Starting Dir is the "root" of where the user keeps his disk images
    RegLoadString(TEXT("Preferences"), REGVALUE_PREF_START_DIR, 1, &szFilename, MAX_PATH);
    if (szFilename) {
        strcpy(g_sCurrentDir, szFilename);
        free(szFilename);
        szFilename = NULL;
    }
    //  SetCurrentDirectory(g_sCurrentDir);
    if(strlen(g_sCurrentDir) == 0 || g_sCurrentDir[0] != '/') //something is wrong in dir name?
    {   //
        char *tmp = getenv("HOME"); /* we don't have HOME?  ^_^  0_0  $_$  */
        if(tmp == NULL) strcpy(g_sCurrentDir, "/");  //begin from the root, then
        else strcpy(g_sCurrentDir, tmp);
    }
    // Load starting directory for HDV (Apple][ HDD) images
    RegLoadString(TEXT("Preferences"), REGVALUE_PREF_HDD_START_DIR, 1, &szFilename, MAX_PATH);
    if (szFilename) {
        strcpy(g_sHDDDir, szFilename);
        free(szFilename);
        szFilename = NULL;
    }
    //  SetCurrentDirectory(g_sCurrentDir);
    if(strlen(g_sHDDDir) == 0 || g_sHDDDir[0] != '/') //something is wrong in dir name?
    {
        char *tmp = getenv("HOME"); /* we don't have HOME?  ^_^  0_0  $_$  */
        if(tmp == NULL) strcpy(g_sHDDDir, "/");  //begin from the root, then
        else strcpy(g_sHDDDir, tmp);
    }

    // Load starting directory for saving current states
    RegLoadString(TEXT("Preferences"), REGVALUE_PREF_SAVESTATE_DIR, 1, &szFilename, MAX_PATH);
    if (szFilename) {
        strcpy(g_sSaveStateDir, szFilename);
        free(szFilename);
        szFilename = NULL;
    }
    if(strlen(g_sSaveStateDir) == 0 || g_sSaveStateDir[0] != '/') //something is wrong in dir name?
    {
        char *tmp = getenv("HOME"); /* we don't have HOME?  ^_^  0_0  $_$  */
        if(tmp == NULL) strcpy(g_sSaveStateDir, "/");  //begin from the root, then
        else strcpy(g_sSaveStateDir, tmp);
    }

    // Read and fill FTP variables - server, local dir, user name and password
    RegLoadString(TEXT("Preferences"), REGVALUE_FTP_DIR, 1, &szFilename, MAX_PATH);
    if (szFilename) {
        strcpy(g_sFTPServer, szFilename);
        free(szFilename);
        szFilename = NULL;
    }
    RegLoadString(TEXT("Preferences"), REGVALUE_FTP_HDD_DIR, 1, &szFilename, MAX_PATH);
    if (szFilename) {
        strcpy(g_sFTPServerHDD, szFilename);
        free(szFilename);
        szFilename = NULL;
    }

    RegLoadString(TEXT("Preferences"), REGVALUE_FTP_LOCAL_DIR, 1, &szFilename, MAX_PATH);
    if (szFilename) {
        strcpy(g_sFTPLocalDir, szFilename);
        free(szFilename);
        szFilename = NULL;
    }
    RegLoadString(TEXT("Preferences"), REGVALUE_FTP_USERPASS, 1, &szFilename, 512);
    if (szFilename) {
        strcpy(g_sFTPUserPass, szFilename);
        free(szFilename);
        szFilename = NULL;
    }
    // Print some debug strings
    printf("Ready login = %s\n",g_sFTPUserPass);

    //
    // ****By now we deal without Uthernet interface! --bb****
    //   char szUthernetInt[MAX_PATH] = {0};
    //   RegLoadString(TEXT("Configuration"),TEXT("Uthernet Interface"),1,szUthernetInt,MAX_PATH);
    //   update_tfe_interface(szUthernetInt,NULL);

}

//===========================================================================
void RegisterExtensions ()
{   // TO DO: register extensions for KDE or GNOME desktops?? Do not know, if it is sane idea. He-he. --bb

}

//===========================================================================

int main(int argc, char * lpCmdLine[])
{
    //		reading FullScreen and Boot from conf file?
    //	bool bSetFullScreen = false;
    //	bool bBoot = false;

    //
    // Find Home Directory and assign linapple.conf to ~/.linapple/linapple.conf
    // if not found set default name in current directory
    const char* home = getenv("HOME");
    std::string linappleconfstr(home);
    linappleconfstr += "/.linapple/linapple.conf";
    const char * linappleconf = linappleconfstr.c_str();
    ifstream ifile (linappleconf);
    if (ifile) {
        registry = fopen(linappleconf, "a+t");	// open conf file (linapple.conf by default)
    }
    else  {
        registry = fopen("linapple.conf", "a+t");	// open conf file (linapple.conf by default)
    }

    bool bBenchMark = false;

    while ((opt = getopt (argc, lpCmdLine, "1:2:rbhf")) != -1)
    {
        switch (opt)
        {
        case '1':
            Disk1 = optarg;
            argdisks = true;
            break;
        case '2':
            Disk2 = optarg;
            argdisks2 = true;
            break;
        case 'r':
            autoboot = true;
            break;
        case 'b':
            bBenchMark = true;
            printf("benchmark");
            break;
        case 'h':
            printf("Linapple command options..\n\n -h Show this help message\n -1 Mount disk image in first drive\n -2 Mount disk image in second drive\n -r Auto start emulation\n -b Benchmark and quit\n\n");
            return 0;
            break;
        case 'f':
            fullscreenboot =true;
            break;
        }
    }

#if 0
#ifdef RIFF_SPKR
    RiffInitWriteFile("Spkr.wav", SPKR_SAMPLE_RATE, 1);
#endif
#ifdef RIFF_MB
    RiffInitWriteFile("Mockingboard.wav", 44100, 2);
#endif
#endif

#if DBG_CALC_FREQ
    //QueryPerformanceFrequency((LARGE_INTEGER*)&g_nPerfFreq);
    g_nPerfFreq = 1000;//milliseconds?
    if(g_fh) fprintf(g_fh, "Performance frequency = %d\n",g_nPerfFreq);
#endif

    //-----

    // Initialize COM

    if(InitSDL()) return 1; // init SDL subsystems, set icon

    // CURL routines
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    if(!g_curl) {
        printf("Could not initialize CURL easy interface");
        return 1;
    }
    /* Set user name and password to access FTP server */
    curl_easy_setopt(g_curl, CURLOPT_USERPWD, g_sFTPUserPass);
    //
    // just do not see why we need this timer???
    SysClk_InitTimer();

    // DO ONE-TIME INITIALIZATION
    MemPreInitialize();		// Call before any of the slot devices are initialized
    ImageInitialize();
    DiskInitialize();
    CreateColorMixMap();	// For tv emulation g_nAppMode

    do
    {
        // DO INITIALIZATION THAT MUST BE REPEATED FOR A RESTART
        restart = 0;
        g_nAppMode = MODE_LOGO;
        fullscreen = false;

        LoadConfiguration();
        FrameCreateWindow();

        if (!DSInit()) soundtype = SOUND_NONE;		// Direct Sound and Stuff

        MB_Initialize();	// Mocking board
        SpkrInitialize();	// Speakers - of Apple][ ...grrrrrrrrrrr, I love them!--bb
        DebugInitialize();
        JoyInitialize();
        MemInitialize();
        HD_SetEnabled(hddenabled ? true : false);
        //printf("g_bHD_Enabled = %d\n", g_bHD_Enabled);

        VideoInitialize();
        Snapshot_Startup();		// Do this after everything has been init'ed
        JoyReset();
        SetUsingCursor(0);
        if (disablecursor) SDL_ShowCursor(SDL_DISABLE);

        // trying fullscreen
        if (!fullscreen) SetNormalMode();
        else SetFullScreenMode();

        DrawFrameWindow();	// we do not have WM_PAINT?
        // ENTER THE MAIN MESSAGE LOOP
        if(bBenchMark) VideoBenchmark(); // start VideoBenchmark and exit
        else EnterMessageLoop();	// else we just start game
        // on WM_DESTROY event:

        Snapshot_Shutdown();
        DebugDestroy();
        //		printf("Quitting. Snapshot_Shutdown-ed!\n");
        if (!restart) {
            DiskDestroy();
            ImageDestroy();
            HD_Cleanup();
        }
        //		printf("Quitting. DiskDestroy, ImageDestroy and HD_Cleanup!\n");
        PrintDestroy();
        sg_SSC.CommDestroy();
        CpuDestroy();
        MemDestroy();
        //		printf("Quitting. PrintDestroy, sg_SSC.CommDestroy, CPU-MEMDestroy!\n");
        SpkrDestroy();
        //		printf("Quitting. SpkrDestroy!!\n");

        VideoDestroy();
        //		printf("Quitting. VideoDestroy!!\n");
        MB_Destroy();
        //		printf("Quitting. MB_Destroy!\n");
        // end of WM_DESTROY event
        MB_Reset();
        //		printf("Quitting. MB_Reset!\n");
        sg_Mouse.Uninitialize(); // Maybe restarting due to switching slot-4 card from mouse to MB
        //		printf("Quitting. Mouse.Uninitialize!!!\n");
        JoyShutDown();	// close opened (if any) joysticks
    }
    while (restart);

    // Release COM
    DSUninit();
    SysClk_UninitTimer();

    if(g_fh)
    {
        fprintf(g_fh,"*** Logging ended\n\n");
        fclose(g_fh);
    }

    RiffFinishWriteFile();
    fclose(registry); 		//close conf file (linapple.conf by default)

    SDL_Quit();
    // CURL routines
    curl_easy_cleanup(g_curl);
    curl_global_cleanup();
    //
    printf("Linapple: successfully exited!\n");
    return 0;
}
