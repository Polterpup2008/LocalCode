Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "Sections.nsh"
!include "FileFunc.nsh"
Name "LocalCode"
OutFile "${LC_OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\LocalCode"
InstallDirRegKey HKCU "Software\LocalCode" "InstallDir"
RequestExecutionLevel user
# CUDA libraries are large and already contain compressed device code.
# zlib keeps release packaging practical without changing the installed payload.
SetCompressor zlib
ManifestDPIAware true
VIProductVersion "${LC_VERSION}.0"
VIAddVersionKey "ProductName" "LocalCode"
VIAddVersionKey "FileDescription" "LocalCode installer"
VIAddVersionKey "FileVersion" "${LC_VERSION}"
VIAddVersionKey "ProductVersion" "${LC_VERSION}"
VIAddVersionKey "LegalCopyright" "LocalCode contributors"
!define MUI_ABORTWARNING
!define MUI_ICON "${LC_ICON}"
!define MUI_UNICON "${LC_ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\LocalCode.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LC_LICENSE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "French"

Section "LocalCode" Core
  SectionIn RO
  !include "${LC_FILES_CORE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\LocalCode"
  CreateShortcut "$SMPROGRAMS\LocalCode\LocalCode.lnk" "$INSTDIR\bin\LocalCode.exe"
  CreateShortcut "$SMPROGRAMS\LocalCode\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\LocalCode" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode" "DisplayName" "LocalCode"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode" "DisplayVersion" "${LC_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode" "DisplayIcon" "$INSTDIR\bin\LocalCode.exe,0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode" "NoRepair" 1
SectionEnd

Section /o "Desktop shortcut" DesktopShortcut
  CreateShortcut "$DESKTOP\LocalCode.lnk" "$INSTDIR\bin\LocalCode.exe"
  WriteRegDWORD HKCU "Software\LocalCode" "DesktopShortcut" 1
SectionEnd

!ifdef LC_FILES_PLUGIN
Section /o "LocalCode Discord plugin" Discord
  !include "${LC_FILES_PLUGIN}"
SectionEnd
!endif

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${Core} "LocalCode and its included local model runtime. Model weights are downloaded separately."
  !insertmacro MUI_DESCRIPTION_TEXT ${DesktopShortcut} "Add a LocalCode shortcut to your desktop."
!ifdef LC_FILES_PLUGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${Discord} "Optional Discord integration, managed from LocalCode. Configure your own bot after installation; no bot starts during setup."
!endif
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This package requires 64-bit Windows."
    Abort
  ${EndIf}
  SetShellVarContext current
!ifdef LC_FILES_PLUGIN
  # Silent installs use the same unchecked default as the components page.
  # Deployment and package verification can explicitly select the optional plugin.
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/DISCORD=" $1
  ${IfNot} ${Errors}
    ${If} $1 == "1"
      !insertmacro SelectSection ${Discord}
    ${ElseIf} $1 == "0"
      !insertmacro UnselectSection ${Discord}
    ${Else}
      MessageBox MB_ICONSTOP "Use /DISCORD=0 or /DISCORD=1." /SD IDOK
      SetErrorLevel 2
      Abort
    ${EndIf}
  ${EndIf}
!endif
FunctionEnd

Section "Uninstall"
  SetShellVarContext current
  # Enumerated installed files only: leave added files and bot user data alone.
  !include "${LC_FILES_UNINSTALL}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\LocalCode\LocalCode.lnk"
  Delete "$SMPROGRAMS\LocalCode\Uninstall.lnk"
  RMDir "$SMPROGRAMS\LocalCode"
  StrCpy $0 0
  ReadRegDWORD $0 HKCU "Software\LocalCode" "DesktopShortcut"
  ${If} $0 == 1
    Delete "$DESKTOP\LocalCode.lnk"
  ${EndIf}
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode"
  DeleteRegValue HKCU "Software\LocalCode" "InstallDir"
  DeleteRegValue HKCU "Software\LocalCode" "DesktopShortcut"
  DeleteRegKey /ifempty HKCU "Software\LocalCode"
SectionEnd
