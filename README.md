# FINDDUPE
## Duplicate file detector and eliminator

****
### Source
**Modified from [FINDDUPE: Duplicate file detector and eliminator](https://www.sentex.ca/~mwandel/finddupe/index.html)**

Some modifications:
1. Adapt the code to use 64 bit apiset.
2. Use unicode characterset to display unicode file names correctly.
3. Fix bugs of some features.
  
Create a solution based on Visual Studio 2022 toolchain to use modern build tools.
****
### Usage
Work on x64 Windows only.  
  
Use your favourite shell.  
  
finddupe command line options  

**finddupe [options] [-ref] <filepat> [filepat]...**  

`-hardlink`  
Delete duplicate copies of file, and replace duplicates with hardlinks to other copy of the file. Works only on NTFS file systems, and with administrator privileges. (You will need to launch cmd or powershell with administrator privileges.)  
`-del`  
Delete duplicate files only.  
`-sigs`	 
Print computed file signature of each file. The file signature is computed using a CRC of the first 32k of the file, as well as its length. The signature is used to detect files that are probably duplicates. Finddupe does a full binary file compare before taking any action.  
`-rdonly`  
Also operate on files that have the readonly bit set (these are skipped by default). I use this feature to eliminate shared files in large projects under version control at work.  
`-z`  
Do not skip zero length files (zero length files are ignored by default).  
`-u`  
Do not print a warning for files that cannot be read.  
`-p`  
Hide progress indicator (useful when output is redirected to a file).  
`-j`  
Follow NTFS junctions and reparse points (not followed by default).  
`-bat <batchfile>`  
Do not hardlink or delete any files. Instead, create a batch file containing the actions to be performed. This can be useful if you want to inspect what finddupe will do.  
`-listlink`  
Puts finddupe in hardlink finding mode. In this mode, finddupe will list which groups of files are hardlinked together. All hardlinked instances found of a file are shown together. However, finddupe can only find instances of the hardlinked file that are within the search path. This option can only be combined with the -v option.  
`-ref <filepat>`  
The file or file pattern after the -ref is a reference. These files will be compared against, but not eliminated. Rather, other files on the command line will be considered duplicates of the reference files.  
`[filepat]`  
File pattern matching in finddupe is very powerful. It uses the same code as is used in jhead. For example, to specify c:\** would indicate every file on the entire C drive. Specifying C:\**\foo\*.jpg specifies any file that ends with .jpg that is in a subdirectory called foo anywhere on the hard drive, including such directories as c:\foo, c:\bar\foo, c:\hello\workd\foo and c:\foo\bar\foo.  
  
For more detailed infomation, check [FINDDUPE: Duplicate file detector and eliminator](https://www.sentex.ca/~mwandel/finddupe/index.html).
****
### Thanks
[Chuyu-Team/VC-LTL5](https://github.com/Chuyu-Team/VC-LTL5) This project uses VC-LTL to shrink size of the artifact.  
