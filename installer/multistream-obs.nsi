; The Windows installer for Multistream for OBS: puts the plugin where OBS
; 28+ looks for plugins (C:\ProgramData\obs-studio\plugins\multistream), for
; every OBS on the PC. Built on GitHub Actions (.github/workflows/obs-plugin.yaml):
;   makensis /DVERSION=1.0.0 /DSRC=<installed plugin folder> multistream-obs.nsi

Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRC
  !define SRC "..\release\RelWithDebInfo\multistream"
!endif
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MultistreamOBS"

Name "Multistream for OBS"
OutFile "Multistream-OBS-Setup-${VERSION}.exe"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "multistream.gg"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Multistream for OBS"
VIAddVersionKey "CompanyName" "Multistream"
VIAddVersionKey "FileDescription" "Multistream for OBS installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-2.0-or-later"

!define MUI_WELCOMEPAGE_TITLE "Multistream for OBS ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This adds the Multistream dock to OBS: it checks everything before you go live, then goes live on all your platforms with one button.$\r$\n$\r$\nOn the Free plan, keep the Multistream app open on this PC while you stream. Pro needs only OBS.$\r$\n$\r$\nClose OBS before you go on. Next time you open OBS, the Multistream dock is there."
!define MUI_FINISHPAGE_TITLE "Multistream is in OBS"
!define MUI_FINISHPAGE_TEXT "Open OBS: the Multistream dock and the Vertical Scenes, Sources, Preview and Mixer docks show up on their own. Closed one? It's under Docks.$\r$\n$\r$\nUsed the Multistream Twitch tool or Vertical tool scripts before? The dock tells you if one needs removing."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; OBS holds the plugin open while it runs. Run silently (/S, by the themed
; installer in installer/, which waits for OBS to close first), it stops
; with exit code 2 instead of asking.
!macro WAIT_FOR_OBS
  retry:
  nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq obs64.exe" /NH | find /I "obs64.exe"'
  Pop $0
  Pop $1
  StrCmp $0 "0" 0 +3
  MessageBox MB_RETRYCANCEL|MB_ICONINFORMATION "OBS is open. Close OBS, then press Retry." /SD IDCANCEL IDRETRY retry
  Abort
!macroend

Function .onInit
  SetShellVarContext all
  StrCpy $INSTDIR "$APPDATA\obs-studio\plugins\multistream"
  ; The plugin uses OBS 31.1's canvases: an older OBS wouldn't load it at all
  ; (no dock, no message). Silent runs stop with exit code 3.
  ReadRegStr $R0 HKLM "SOFTWARE\OBS Studio" ""
  StrCmp $R0 "" obsok
  ClearErrors
  GetDLLVersion "$R0\bin\64bit\obs64.exe" $R1 $R2
  IfErrors obsok
  IntOp $R3 $R1 >> 16
  IntOp $R4 $R1 & 0xFFFF
  IntCmp $R3 31 0 obsold obsok
  IntCmp $R4 1 obsok obsold obsok
  obsold:
  MessageBox MB_OK|MB_ICONEXCLAMATION "Multistream needs OBS 31.1 or newer, and this computer has OBS $R3.$R4.$\r$\n$\r$\nUpdate OBS (in OBS: Help, then Check for Updates, or get it from obsproject.com), then run this installer again." /SD IDOK
  SetErrorLevel 3
  Quit
  obsok:
FunctionEnd

Function un.onInit
  SetShellVarContext all
FunctionEnd

; Puts back the stream setting the dock replaced ("Point OBS at Multistream"
; kept it in previous-service.json), but only while that OBS profile still
; sends to Multistream: one changed by hand since stays as it is. The
; plugin's restore.ini names the profile folder and the host it sends to.
Var Cfg
Var ObsProfile
Var Host
Var Found
Function un.PutBackService
  ReadINIStr $ObsProfile "$Cfg\restore.ini" "service" "profile"
  ReadINIStr $Host "$Cfg\restore.ini" "service" "host"
  StrCmp $ObsProfile "" done
  StrCmp $Host "" done
  IfFileExists "$Cfg\previous-service.json" 0 done
  IfFileExists "$ObsProfile\service.json" 0 done
  StrCpy $Found ""
  StrLen $R3 $Host
  ClearErrors
  FileOpen $R0 "$ObsProfile\service.json" r
  IfErrors done
  nextline:
    ClearErrors
    FileRead $R0 $R1
    IfErrors closed
    StrCpy $R4 0
    scan:
      StrCpy $R2 $R1 $R3 $R4
      StrCmp $R2 "" nextline
      StrCmp $R2 $Host 0 +3
        StrCpy $Found "1"
        Goto closed
      IntOp $R4 $R4 + 1
      Goto scan
  closed:
  FileClose $R0
  StrCmp $Found "1" 0 done
  CopyFiles /SILENT "$Cfg\previous-service.json" "$ObsProfile\service.json"
  done:
FunctionEnd

Section "Multistream for OBS"
  !insertmacro WAIT_FOR_OBS
  SetOutPath "$INSTDIR\bin\64bit"
  File "${SRC}\bin\64bit\multistream.dll"
  SetOutPath "$INSTDIR\data"
  File /r "${SRC}\data\*.*"
  ; GPL-2.0, like OBS: the licence and where the source is.
  SetOutPath "$INSTDIR"
  File "..\LICENSE"
  File "README.txt"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "Multistream for OBS"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Multistream"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://multistream.gg"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  !insertmacro WAIT_FOR_OBS
  Delete "$INSTDIR\bin\64bit\multistream.dll"
  RMDir "$INSTDIR\bin\64bit"
  RMDir "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
  ; This user's OBS settings (not every user's): the stream setting back,
  ; and the sign-in forgotten. Vertical scenes and the rest stay, for a
  ; reinstall.
  SetShellVarContext current
  StrCpy $Cfg "$APPDATA\obs-studio\plugin_config\multistream"
  Call un.PutBackService
  Delete "$Cfg\previous-service.json"
  Delete "$Cfg\previous-service.json.bak"
  Delete "$Cfg\restore.ini"
  Delete "$Cfg\account.json"
  Delete "$Cfg\account.json.bak"
  SetShellVarContext all
SectionEnd
