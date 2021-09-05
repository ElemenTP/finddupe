# FINDDUPE
## Duplicate file detector and eliminator

****
### src
**Modified from [FINDDUPE: Duplicate file detector and eliminator](https://www.sentex.ca/~mwandel/finddupe/index.html)**

Minor modification, just change some methods to adapt it to 64 bit, so it can check files those are larger than 4GB. Also worked on makefile to adapt MSVC142.
****
### compile
Make sure your computer has Visual Studio or Build Tools with C++ workloads installed. Use the "x64 Native Tools Command Prompt for VS ****" on x64 Windows or "x86_x64 Cross Tools Command Prompt for VS ****" on x86 Windows.  
Change the working directory to where you download the code, then execute "nmake", with "/nologo" arg if you prefer.
### useage
Work on x64 Windows only.  
Use cmd or powershell  

finddupe command line options  

**finddupe [options] [-ref] <filepat> [filepat]...**  

`-hardlink`	Delete duplicate copies of file, and replace duplicates with hardlinks to other copy of the file. Works only on NTFS file systems, and with administrator privileges. (You will need to launch cmd or powershell with administrator privileges.)  
`-del` Delete duplicate files  
`-sigs`	Pring computed file signature of each file. The file signature is computed using a CRC of the first 32k of the file, as well as its length. The signature is used to detect files that are probably duplicates. Finddupe does a full binary file compare before taking any action.  
`-rdonly` Also operate on files that have the readonly bit set (these are normally skipped). I use this feature to eliminate shared files in large projects under version control at work.  
`-z` Do not skip zero length files (zero length files are ignored by default)  
`-u` Do not print a warning for files that cannot be read  
`-p` Hide progress indicator (useful when output is redirected to a file)  
`-j` Follow NTFS junctions and reparse points (not followed by default)  
`-bat <batchfile>` Do not hardlink or delete any files. Rather, create a batch file containing the actions to be performed. This can be useful if you want to inspect what finddupe will do.  
`-listlink` Puts finddupe in hardlink finding mode. In this mode, finddupe will list which groups of files are hardlinked together. All hardlinked instances found of a file are shown together. However, finddupe can only find instances of the hardlinked file that are within the search path. This option can only be combined with the -v option.  
`-ref <filepat>` The file or file pattern after the -ref is a reference. These files will be compared against, but not eliminated. Rather, other files on the command line will be considered duplicates of the reference files.  
`[filepat]`	File pattern matching in finddupe is very powerful. It uses the same code as is used in jhead. For example, to specify c:\** would indicate every file on the entire C drive. Specifying C:\**\foo\*.jpg specifies any file that ends with .jpg that is in a subdirectory called foo anywhere on the hard drive, including such directories as c:\foo, c:\bar\foo, c:\hello\workd\foo and c:\foo\bar\foo.  
  
For more detailed infomation, check [FINDDUPE: Duplicate file detector and eliminator](https://www.sentex.ca/~mwandel/finddupe/index.html).