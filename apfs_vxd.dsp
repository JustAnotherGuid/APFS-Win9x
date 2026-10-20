# Microsoft Developer Studio Project File - Name="apfs_vxd" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) External Target" 0x0106

CFG=apfs_vxd - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "apfs_vxd.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "apfs_vxd.mak" CFG="apfs_vxd - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "apfs_vxd - Win32 Release" (based on "Win32 (x86) External Target")
!MESSAGE "apfs_vxd - Win32 Debug" (based on "Win32 (x86) External Target")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""

!IF  "$(CFG)" == "apfs_vxd - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Cmd_Line "NMAKE /f apfs_vxd.mak"
# PROP BASE Rebuild_Opt "/a"
# PROP BASE Target_File "apfs_vxd.exe"
# PROP BASE Bsc_Name "apfs_vxd.bsc"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Cmd_Line "build"
# PROP Rebuild_Opt "/a"
# PROP Target_File "apfs.vxd"
# PROP Bsc_Name ""
# PROP Target_Dir ""

!ELSEIF  "$(CFG)" == "apfs_vxd - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Cmd_Line "NMAKE /f apfs_vxd.mak"
# PROP BASE Rebuild_Opt "/a"
# PROP BASE Target_File "apfs_vxd.exe"
# PROP BASE Bsc_Name "apfs_vxd.bsc"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Cmd_Line "build"
# PROP Rebuild_Opt "/a"
# PROP Target_File "apfs.vxd"
# PROP Bsc_Name "obj\i386\APFS.bsc"
# PROP Target_Dir ""

!ENDIF 

# Begin Target

# Name "apfs_vxd - Win32 Release"
# Name "apfs_vxd - Win32 Debug"

!IF  "$(CFG)" == "apfs_vxd - Win32 Release"

!ELSEIF  "$(CFG)" == "apfs_vxd - Win32 Debug"

!ENDIF 

# Begin Group "Source Files"

# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"
# Begin Source File

SOURCE=.\apfs_blockio.c
# End Source File
# Begin Source File

SOURCE=.\apfs_btree.c
# End Source File
# Begin Source File

SOURCE=.\apfs_checksum.c
# End Source File
# Begin Source File

SOURCE=.\apfs_container.c
# End Source File
# Begin Source File

SOURCE=.\apfs_dispatch.c
# End Source File
# Begin Source File

SOURCE=.\apfs_fsrec.c
# End Source File
# Begin Source File

SOURCE=.\apfs_gpt.c
# End Source File
# Begin Source File

SOURCE=.\apfs_handle.c
# End Source File
# Begin Source File

SOURCE=.\apfs_main.c
# End Source File
# Begin Source File

SOURCE=.\apfs_names.c
# End Source File
# Begin Source File

SOURCE=.\apfs_omap.c
# End Source File
# Begin Source File

SOURCE=.\apfs_resolve.c
# End Source File
# Begin Source File

SOURCE=.\apfs_time.c
# End Source File
# Begin Source File

SOURCE=.\apfs_utils.c
# End Source File
# Begin Source File

SOURCE=.\apfs_volume.c
# End Source File
# Begin Source File

SOURCE=.\devdcl.asm
# End Source File
# End Group
# Begin Group "Header Files"

# PROP Default_Filter "h;hpp;hxx;hm;inl"
# Begin Source File

SOURCE=.\apfs_types.h
# End Source File
# Begin Source File

SOURCE=.\apfs_vxd.h
# End Source File
# Begin Source File

SOURCE=.\gpt_types.h
# End Source File
# Begin Source File

SOURCE=.\vxdwrap.h
# End Source File
# End Group
# Begin Group "Resource Files"

# PROP Default_Filter "ico;cur;bmp;dlg;rc2;rct;bin;rgs;gif;jpg;jpeg;jpe"
# End Group
# End Target
# End Project
