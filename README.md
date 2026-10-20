# APFS-Win9x
Windows 9x based Installable File System driver for the Apple File System. Have you ever wanted to be able to copy data from an APFS formatted USB stick onto your Windows ME device? Well this Win9x-based installable file system driver lets you do just that. The VXD implements a 9x-based installable file system driver for the Apple File system, the driver is currently read-only and has only been validated on Windows Me-based systems but the intent is to expand it to as many 9x-based systems as possible (e.g. 98, 95 OSR 2.5 etc). With this file system you can copy files, view images, listen to MP3 files and even play videos.

# Installation
In the releases section of this repository select the latest published version, the zip package contains the virtual device driver (.vxd) file, the MSVC 6.0 browser information file, the binary's map file as well as the lib and exp files produced as part of the compilation process. By default Windows Me will automatically load any drivers in the IOSUBSYS folder, to "install" the driver copy the vxd file to the IOSUBSYS in the C:\Windows\System folder. To uninstall the driver boot the system from a Windows Me startup disk and delete the driver file in IOSUBSYS. 

# How to build
The driver was excusively developed and built on a Windows Me machine with the Windows 98 SE DDK, so while it theoretically should be possible to build the driver with other systems with different 9x-based DDKs I have not attempted this and can therefore not verify that it works. With that caveat out of the way the following setup can be used to build the driver from source:

- A Windows Me-based machine.
- MSVC 6.0 with SP5.
- The 98 DDK with MASM 6.11d - I used version 12.00.8804 of MSVC and 6.11d MASM (needed to compile the part of the driver written in x86 assembly)

To build the checked version of the driver start the "Checked Build Environment" in the 98ddk, to build the free version use the "Free Build Envinroment" option. After you have started the build environment cd to wheverever the code is and enter "build", this will build the driver and the browse information file in the obj/i386/ file.

# Future enhancements
While the driver is currently read-only the following features are planned:

- Performance improvements (especially with larger file sizes).
- Validate that the driver works on Windows 95 OS OSR 2.5, Windows 98 FE + SE.
- DOS file names (this currently does not work).
- Write capability. 

