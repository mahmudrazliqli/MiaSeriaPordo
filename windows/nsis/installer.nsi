; installer.nsi
; Installer for MiaSeriaPordo

;--------------------------------
; General Information
!define PRODUCT_NAME "MiaSeriaPordo"
!define PRODUCT_VERSION "1.0"
!define PRODUCT_PUBLISHER "Your Company"
!define PRODUCT_WEB_SITE "http://www.yourcompany.com"
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\MiaSeriaPordo.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_UNINST_ROOT_KEY "HKLM"

SetCompressor lzma

;--------------------------------
; Page Settings
!include "MUI.nsh"
!define MUI_ABORTWARNING

; Use custom icon MiaSeriaPordo.ico
!define MUI_ICON "MiaSeriaPordo.ico"
!define MUI_UNICON "MiaSeriaPordo.ico"

; Welcome page
!insertmacro MUI_PAGE_WELCOME
; License page (optional - comment if you don't have license.txt)
; !insertmacro MUI_PAGE_LICENSE "license.txt"
; Installation directory page
!insertmacro MUI_PAGE_DIRECTORY
; Installation page
!insertmacro MUI_PAGE_INSTFILES
; Finish page
!define MUI_FINISHPAGE_RUN "$INSTDIR\MiaSeriaPordo.exe"
!insertmacro MUI_PAGE_FINISH

; Uninstall pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; Language (English only)
!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Installer Settings

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "MiaSeriaPordo_Setup.exe"
InstallDir "$PROGRAMFILES\MiaSeriaPordo"
InstallDirRegKey HKLM "${PRODUCT_DIR_REGKEY}" ""
ShowInstDetails show
ShowUnInstDetails show

;--------------------------------
; Installation Section

Section "MainSection" SEC01
  SetOutPath "$INSTDIR"
  
  ; Install main files
  File "MiaSeriaPordo.exe"
  File "windows1.glade"
  File "MiaSeriaPordo.ico"
  
  ; Install all dll files
  File "*.dll"
  
  ; Create Start Menu shortcuts
  CreateDirectory "$SMPROGRAMS\MiaSeriaPordo"
  CreateShortCut "$SMPROGRAMS\MiaSeriaPordo\MiaSeriaPordo.lnk" "$INSTDIR\MiaSeriaPordo.exe" "" "$INSTDIR\MiaSeriaPordo.ico"
  CreateShortCut "$SMPROGRAMS\MiaSeriaPordo\Uninstall.lnk" "$INSTDIR\uninst.exe"
  
  ; Create Desktop shortcut
  CreateShortCut "$DESKTOP\MiaSeriaPordo.lnk" "$INSTDIR\MiaSeriaPordo.exe" "" "$INSTDIR\MiaSeriaPordo.ico"
  
  ; Register in registry
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\MiaSeriaPordo.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayName" "$(^Name)"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\MiaSeriaPordo.exe"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  
  ; Create uninstaller
  WriteUninstaller "$INSTDIR\uninst.exe"
SectionEnd

;--------------------------------
; Uninstall Section

Section Uninstall
  ; Delete files
  Delete "$INSTDIR\MiaSeriaPordo.exe"
  Delete "$INSTDIR\windows1.glade"
  Delete "$INSTDIR\MiaSeriaPordo.ico"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\uninst.exe"
  
  ; Delete shortcuts
  Delete "$SMPROGRAMS\MiaSeriaPordo\MiaSeriaPordo.lnk"
  Delete "$SMPROGRAMS\MiaSeriaPordo\Uninstall.lnk"
  Delete "$DESKTOP\MiaSeriaPordo.lnk"
  
  ; Delete Start Menu folder
  RMDir "$SMPROGRAMS\MiaSeriaPordo"
  
  ; Delete installation folder
  RMDir "$INSTDIR"
  
  ; Delete registry keys
  DeleteRegKey ${PRODUCT_UNINST_ROOT_KEY} "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_DIR_REGKEY}"
  SetAutoClose true
SectionEnd

;--------------------------------
; Functions

Function .onInit
  ; Additional initialization code can be added here
FunctionEnd

Function un.onInit
  MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON2 "Are you sure you want to uninstall ${PRODUCT_NAME}?" IDYES +2
  Abort
FunctionEnd

Function un.onUninstSuccess
  HideWindow
  MessageBox MB_ICONINFORMATION|MB_OK "${PRODUCT_NAME} was successfully removed."
FunctionEnd