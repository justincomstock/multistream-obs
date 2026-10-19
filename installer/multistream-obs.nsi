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
!define MUI_WELCOMEPAGE_TEXT "This adds the Multistream dock to OBS: it checks everything before you go live, then goes live on all your platforms with one button.$\r$\n$\r$\nClose OBS before you go on. Next time you open OBS, the Multistream dock is there."
!define MUI_FINISHPAGE_TITLE "Multistream is in OBS"
!define MUI_FINISHPAGE_TEXT "Open OBS: the Multistream dock shows up on its own. If you closed it, it's under Docks → Multistream.$\r$\n$\r$\nUsed the Multistream Twitch tool or Vertical tool scripts before? The dock tells you if one needs removing."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; OBS holds the plugin open while it runs.
!macro WAIT_FOR_OBS
  retry:
  nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq obs64.exe" /NH | find /I "obs64.exe"'
  Pop $0
  Pop $1
  StrCmp $0 "0" 0 +3
  MessageBox MB_RETRYCANCEL|MB_ICONINFORMATION "OBS is open. Close OBS, then press Retry." IDRETRY retry
  Abort
!macroend

Function .onInit
  SetShellVarContext all
  StrCpy $INSTDIR "$APPDATA\obs-studio\plugins\multistream"
FunctionEnd

Function un.onInit
  SetShellVarContext all
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
SectionEnd
