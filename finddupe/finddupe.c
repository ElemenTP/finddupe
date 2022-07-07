//--------------------------------------------------------------------------
// Find duplicate files and hard link, delete, or write batch files to do the same.
// Also includes a separate option to scan for and enumerate hardlinks in the search space.
//
// Version 1.25
//
// Matthias Wandel Oct 2006 - Aug 2010
// ElemenTP 2022
//--------------------------------------------------------------------------

#define VERSION "1.25"

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <stdbool.h>
#include <process.h>
#include <io.h>
#include <sys/utime.h>
#define WIN32_LEAN_AND_MEAN // To keep windows.h bloat down.
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <direct.h>

#define S_IWUSR 0x80 // user has write permission
#define S_IWGRP 0x10 // group has write permission
#define S_IWOTH 0x02 // others have write permisson

static ULONGLONG FilesMatched;

typedef struct
{
	unsigned int Crc;
	unsigned int Sum;
} Checksum_t;

// Data structure for file allcoations:
typedef struct
{
	Checksum_t Checksum;
	struct
	{
		DWORD High;
		DWORD Low;
	} FileIndex;
	DWORD NumLinks;
	ULONGLONG FileSize;
	WCHAR* FileName;
	size_t Larger;  // Child index for larger child
	size_t Smaller; // Child index for smaller child
	size_t Same;
} FileData_t;
static FileData_t* FileData;
static size_t NumAllocated = 1024;
static size_t NumUnique = 1;

// Duplicate statistics summary
struct
{
	ULONGLONG TotalFiles;
	ULONGLONG DuplicateFiles;
	ULONGLONG HardlinkGroups;
	ULONGLONG CantReadFiles;
	ULONGLONG ZeroLengthFiles;
	ULONGLONG TotalBytes;
	ULONGLONG DuplicateBytes;
} DupeStats;

// How many bytes to calculate file signature of.
#define BYTES_DO_CHECKSUM_OF 32768
#define CHUNK_SIZE 0x10000

// Parameters for what to do
FILE* BatchFile = NULL; // Output a batch file
WCHAR* BatchFileName = NULL;

bool PrintFileSigs = false;            // Print signatures of files
bool PrintDuplicates = true;           // Print duplicates
bool MakeHardLinks = false;            // Do the actual hard linking
bool DelDuplicates = false;            // Delete duplicates (no hard linking)
bool ReferenceFiles = false;           // Flag - do not touch present files parsed
bool DoReadonly = false;               // Do it for readonly files also
bool Verbose = false;                  // Verbose output
bool HardlinkSearchMode = false;       // Detect hard links only (do not check duplicates)
bool ShowProgress = true;              // Show progressing file count...
bool HideCantReadMessage = false;      // Hide the can't read file error
bool SkipZeroLength = true;            // Ignore zero length files.
bool ProgressIndicatorVisible = false; // Weither a progress indicator needs to be overwritten.
bool FollowReparse = false;            // Wether to follow reparse points (like unix softlinks for NTFS)

int MyGlob(const WCHAR* Pattern, bool FollowReparse, void (*FileFuncParm)(const WCHAR* FileName));

//--------------------------------------------------------------------------
// Calculate some 64-bit file signature.  CRC and a checksum
//--------------------------------------------------------------------------
static void CalcCrc(Checksum_t* Check, BYTE* Data, ULONGLONG NumBytes)
{
	ULONGLONG a;
	unsigned int Reg, Sum;
	Reg = Check->Crc;
	Sum = Check->Sum;
	for (a = 0; a < NumBytes; a++)
	{
		Reg = Reg ^ Data[a];
		Sum = Sum + Data[a];
		Reg = (Reg >> 8) ^ ((Reg & 0xff) << 24) ^ ((Reg & 0xff) << 9);
		Sum = (Sum << 1) + (Sum >> 31);
	}
	Check->Crc = Reg;
	Check->Sum = Sum;
}

//--------------------------------------------------------------------------
// Clear line (erase the progress indicator)
//--------------------------------------------------------------------------
void ClearProgressInd(void)
{
	if (ProgressIndicatorVisible)
	{
		printf("\033[2K\r");
		ProgressIndicatorVisible = false;
	}
}

//--------------------------------------------------------------------------
// Escape names for batch files: % turns into %%
//--------------------------------------------------------------------------
WCHAR* EscapeBatchName(WCHAR* Name)
{
	static WCHAR EscName[_MAX_PATH * 2] = { 0 };
	size_t a = 0, b = 0;
	while (true)
	{
		EscName[b] = Name[a];
		if (Name[a] == L'\0')
			break;
		if (Name[a] == L'%')
			EscName[++b] = L'%'; // Escape '%' with '%%' for batch files.
		++a;
		++b;
	}
	return EscName;
}

enum EDRes
{
	EDR_NOT_DUPE,
	EDR_HDLINK_LIMIT,
	EDR_SKIP_RO,
	EDR_NO_OP,
	EDR_DELETE,
	EDR_HDLINK
};

//--------------------------------------------------------------------------
// Eliminate duplicates.
//--------------------------------------------------------------------------
static enum EDRes EliminateDuplicate(FileData_t ThisFile, FileData_t DupeOf)
{
	// First compare whole file.  If mismatch, return 0.
	bool IsDuplicate = true;
	bool Hardlinked = false;
	bool IsReadonly = false;

	if (ThisFile.FileSize != DupeOf.FileSize)
		return EDR_NOT_DUPE;

	if (DupeOf.NumLinks > 0 && memcmp(&ThisFile.FileIndex, &DupeOf.FileIndex, sizeof(DupeOf.FileIndex)) == 0)
	{
		Hardlinked = true;
		goto dont_read;
	}

	FILE* File1 = NULL, * File2 = NULL;
	_wfopen_s(&File1, ThisFile.FileName, L"rb");
	if (File1 == NULL)
	{
		fwprintf(stderr, L"Open file %s failure.\n", ThisFile.FileName);
		exit(EXIT_FAILURE);
	}
	_wfopen_s(&File2, DupeOf.FileName, L"rb");
	if (File2 == NULL)
	{
		fclose(File1);
		fwprintf(stderr, L"Open file %s failure.\n", DupeOf.FileName);
		exit(EXIT_FAILURE);
	}

	size_t BytesLeft = ThisFile.FileSize, BytesToRead = 0;
	BYTE* Buf1 = (BYTE*)malloc(CHUNK_SIZE * 2);
	if (Buf1 == NULL)
	{
		fwprintf(stderr, L"Malloc failure.\n");
		exit(EXIT_FAILURE);
	}
	BYTE* Buf2 = Buf1 + CHUNK_SIZE;
	while (BytesLeft)
	{
		BytesToRead = BytesLeft;
		if (BytesToRead > CHUNK_SIZE)
			BytesToRead = CHUNK_SIZE;

		if (fread(Buf1, 1, BytesToRead, File1) != BytesToRead)
		{
			ClearProgressInd();
			fwprintf(stderr, L"Error doing full file read on '%s'\n", ThisFile.FileName);
		}

		if (fread(Buf2, 1, BytesToRead, File2) != BytesToRead)
		{
			ClearProgressInd();
			fwprintf(stderr, L"Error doing full file read on '%s'\n", DupeOf.FileName);
		}

		BytesLeft -= BytesToRead;

		if (memcmp(Buf1, Buf2, BytesToRead))
		{
			IsDuplicate = false;
			break;
		}
	}

	fclose(File1);
	fclose(File2);
	free(Buf1);

	if (!IsDuplicate)
	{
		// Full file duplicate check failed (CRC collision, or differs only after 32k)
		return EDR_NOT_DUPE;
	}

	DupeStats.DuplicateFiles += 1;
	DupeStats.DuplicateBytes += ThisFile.FileSize;

dont_read:
	if (PrintDuplicates && !HardlinkSearchMode)
	{
		ClearProgressInd();
		wprintf(L"Duplicate: '%s'\n", DupeOf.FileName);
		wprintf(L"With:      '%s'\n", ThisFile.FileName);
		if (Hardlinked)
		{
			// If the files happen to be hardlinked, show that.
			wprintf(L"    (hardlinked instances of same file)\n");
		}
	}

	if (!MakeHardLinks && !DelDuplicates)
	{
		return EDR_NO_OP;
	}

	struct _stat64 FileStat;
	if (_wstat64(ThisFile.FileName, &FileStat) != 0)
	{
		// oops!
		fwprintf(stderr, L"Stat failed on '%s'.\n", ThisFile.FileName);
		exit(EXIT_FAILURE);
	}
	IsReadonly = (FileStat.st_mode & S_IWUSR) ? false : true;

	if (IsReadonly)
	{
		// Readonly file.
		if (!DoReadonly)
		{
			ClearProgressInd();
			wprintf(L"Skipping duplicate readonly file '%s'.\n", ThisFile.FileName);
			return EDR_SKIP_RO;
		}
		if (MakeHardLinks || DelDuplicates)
		{
			// Make file read/write so we can delete it.
			// We sort of assume we own the file.  Otherwise, not much we can do.
#pragma warning(disable:6031)
			_wchmod(ThisFile.FileName, FileStat.st_mode | S_IWUSR);
		}
	}

	if (MakeHardLinks)
	{
		if (Hardlinked)
			return EDR_NO_OP; // Nothign to do.

		if (DupeOf.NumLinks >= 1023)
		{
			// Do not link more than 1023 files onto one physical file (windows limit)
			ClearProgressInd();
			wprintf(L"Skipping hardlinking '%s' and '%s', number of links of '%s' has reach limit.\n", ThisFile.FileName, DupeOf.FileName, DupeOf.FileName);
			return EDR_HDLINK_LIMIT;
		}
	}

	if (BatchFile)
	{
		fprintf(BatchFile, "del %s \"%ls\"\n", IsReadonly ? "/F" : "", EscapeBatchName(ThisFile.FileName));
	}
	else
	{
		if (_wunlink(ThisFile.FileName))
		{
			ClearProgressInd();
			fwprintf(stderr, L"Delete of '%s' failed.\n", DupeOf.FileName);
			exit(EXIT_FAILURE);
		}
	}

	if (MakeHardLinks)
	{
		if (BatchFile)
		{
			fprintf(BatchFile, "fsutil hardlink create \"%ls\" ", EscapeBatchName(ThisFile.FileName));
			fprintf(BatchFile, "\"%ls\"\n", EscapeBatchName(DupeOf.FileName));
			if (IsReadonly)
			{
				// If original was readonly, restore that attribute
				fprintf(BatchFile, "attrib +r \"%ls\"\n", EscapeBatchName(ThisFile.FileName));
			}
			ClearProgressInd();
			wprintf(L"    Added hardlink creation command to the batch file.\n");
		}
		else
		{
			if (CreateHardLinkW(ThisFile.FileName, DupeOf.FileName, NULL) == 0)
			{
				// Uh-oh.  Better stop before we mess up more stuff!
				ClearProgressInd();
				fwprintf(stderr, L"Create hard link from '%s' to '%s' failed.\n", DupeOf.FileName, ThisFile.FileName);
				exit(EXIT_FAILURE);
			}

			{
				// set Unix access rights and time to new file
				_wchmod(ThisFile.FileName, FileStat.st_mode);

				struct __utimbuf64 mtime = { 0,0 };
				mtime.actime = FileStat.st_mtime;
				mtime.modtime = FileStat.st_mtime;

				// Set mod time to original file's
				_wutime64(ThisFile.FileName, &mtime);
			}
			ClearProgressInd();
			wprintf(L"    Created hardlink.\n");
		}
		return EDR_HDLINK;
	}
	else
	{
		if (BatchFile)
		{
			ClearProgressInd();
			wprintf(L"    Added delete command to the batch file.\n");
		}
		else
		{
			ClearProgressInd();
			wprintf(L"    Deleted duplicate.\n");
		}
		return EDR_DELETE;
	}
}

//--------------------------------------------------------------------------
// Check for duplicates.
//--------------------------------------------------------------------------
static void CheckDuplicate(FileData_t ThisFile)
{
	size_t Ptr = 1, * Link = NULL;
	// Find where in the trie structure it belongs.

	DupeStats.TotalFiles += 1;
	DupeStats.TotalBytes += ThisFile.FileSize;

	if (NumUnique == 1)
		goto store_it;

	while (true)
	{
		int comp = memcmp(&(ThisFile.Checksum), &(FileData[Ptr].Checksum), sizeof(Checksum_t));
		if (comp == 0)
		{
			// Check for true duplicate.
			if (!ReferenceFiles && !HardlinkSearchMode)
			{
				enum EDRes r = EliminateDuplicate(ThisFile, FileData[Ptr]);
				switch (r)
				{
				case EDR_HDLINK:
					FileData[Ptr].NumLinks += 1; // Update link count.
				case EDR_DELETE:
				case EDR_NO_OP:
				case EDR_SKIP_RO:
				case EDR_HDLINK_LIMIT:// Its a duplicate file.  Do not store info on it.
					return;
				}
				if (FileData[Ptr].Same)
				{
					Ptr = FileData[Ptr].Same;
				}
				else
				{
					FileData[Ptr].Same = NumUnique;
					break;
				}
			}
			// Build a chain on one side of the branch.
			// That way, we will check every checksum collision from here on.
			else
			{
				while (true)
				{
					if (FileData[Ptr].Same)
					{
						Ptr = FileData[Ptr].Same;
					}
					else
					{
						FileData[Ptr].Same = NumUnique;
						goto store_it;
					}
				}
			}
		}
		else
		{
			if (comp > 0)
			{
				Link = &(FileData[Ptr].Larger);
			}
			else
			{
				Link = &(FileData[Ptr].Smaller);
			}
			if (*Link == 0)
			{
				// Link it to here.
				*Link = NumUnique;
				break;
			}
			else
			{
				Ptr = *Link;
			}
		}
	}

store_it:

	if (NumUnique >= NumAllocated)
	{
		// Array is full.  Make it bigger
		NumAllocated = NumAllocated + NumAllocated / 2;
#pragma warning(disable:6308)
		FileData = (FileData_t*)realloc(FileData, NumAllocated * sizeof(FileData_t));
		if (FileData == NULL)
		{
			fwprintf(stderr, L"Malloc failure.\n");
			exit(EXIT_FAILURE);
		}
	}
#pragma warning(disable:6386)
	FileData[NumUnique] = ThisFile;
	NumUnique += 1;
}

//--------------------------------------------------------------------------
// Walk the file tree after handling detect mode to show linked groups.
//--------------------------------------------------------------------------
static void ShowLinkGroups(size_t index)
{
	if (NumUnique == 1 || index == 0)
		return;

	if (FileData[index].NumLinks > 1)
	{
		size_t GroupLen = 0, Ptr = index;
		wprintf(L"\nHardlink group %llu\n", DupeStats.HardlinkGroups);
		while (Ptr)
		{
			wprintf(L"  \"%s\"\n", FileData[Ptr].FileName);
			GroupLen += 1;
			Ptr = FileData[Ptr].Same;
		}
		wprintf(L"Hardlink group, %llu of %lu hardlinked instances found in search tree:\n", GroupLen, FileData[index].NumLinks);
		DupeStats.HardlinkGroups += 1;
	}

	ShowLinkGroups(FileData[index].Larger);
	ShowLinkGroups(FileData[index].Smaller);
}

//--------------------------------------------------------------------------
// Do selected operations to one file at a time.
//--------------------------------------------------------------------------
static void ProcessFile(const WCHAR* FileName)
{
	FileData_t ThisFile;
	memset(&ThisFile, 0, sizeof(ThisFile));

	{
		static ULONGLONG LastPrint = 0, Now;
		Now = GetTickCount64();
		if ((unsigned)(Now - LastPrint) > 500)
		{
			if (ShowProgress)
			{
				WCHAR ShowName[105];
				ULONGLONG l = wcslen(FileName);
				memset(ShowName, L'\0', sizeof(ShowName));
				if (l > 100)
					l = 101;
				memcpy(ShowName, FileName, l * sizeof(WCHAR));
				if (l >= 101)
					memcpy(ShowName + 100, L"...", 4 * sizeof(WCHAR));

				ClearProgressInd();
				wprintf(L"Scanned %4llu files: %ls", FilesMatched, ShowName);
				ProgressIndicatorVisible = true;
				LastPrint = Now;
				ProgressIndicatorVisible = 1;
			}
			fflush(stdout);
		}
	}

	FilesMatched += 1;

	if (BatchFileName && wcscmp(FileName, BatchFileName) == 0)
		return;

	struct _stat64 FileStat;
	if (_wstat64(FileName, &FileStat) != 0)
	{
		// oops!
		goto cant_read_file;
	}
	size_t FileSize = FileStat.st_size;

	if (FileSize == 0)
	{
		if (SkipZeroLength)
		{
			DupeStats.ZeroLengthFiles += 1;
			return;
		}
	}

	ThisFile.FileSize = FileSize;

	{
		HANDLE FileHandle;
		BY_HANDLE_FILE_INFORMATION FileInfo;
		FileHandle = CreateFileW(FileName,
			GENERIC_READ,          // dwDesiredAccess
			FILE_SHARE_READ,       // dwShareMode
			NULL,                  // Security attirbutes
			OPEN_EXISTING,         // dwCreationDisposition
			FILE_ATTRIBUTE_NORMAL, // dwFlagsAndAttributes.  Ignored for opening existing files
			NULL);                 // hTemplateFile.  Ignored for existing.
		if (FileHandle == (void*)-1)
		{
		cant_read_file:
			DupeStats.CantReadFiles += 1;
			if (!HideCantReadMessage)
			{
				ClearProgressInd();
				fwprintf(stderr, L"Could not read '%s'.\n", FileName);
			}
			return;
		}

		if (!GetFileInformationByHandle(FileHandle, &FileInfo))
		{
			CloseHandle(FileHandle);
			goto cant_read_file;
		}

		CloseHandle(FileHandle);

		if (Verbose)
		{
			ClearProgressInd();
			wprintf(L"Hardlinked (%lu links) node=%08lx %08lx: %s\n", FileInfo.nNumberOfLinks, FileInfo.nFileIndexHigh, FileInfo.nFileIndexLow, FileName);
		}

		if (HardlinkSearchMode && FileInfo.nNumberOfLinks == 1)
		{
			// File has only one link, so its not hardlinked.  Skip for hardlink search mode.
			return;
		}

		//printf("    Info:  Index: %08x %08x\n",FileInfo.nFileIndexHigh, FileInfo.nFileIndexLow);

		// Use the file index (which is NTFS equivalent of the iNode) instead of the CRC.
		ThisFile.FileIndex.Low = FileInfo.nFileIndexLow;
		ThisFile.FileIndex.High = FileInfo.nFileIndexHigh;
		ThisFile.NumLinks = FileInfo.nNumberOfLinks;

		if (HardlinkSearchMode)
		{
			// For hardlink search mode, duplicates are detected by file index, not CRC,
			// so copy the file ID into the CRC.
			ThisFile.Checksum.Sum = ThisFile.FileIndex.Low;
			ThisFile.Checksum.Crc = ThisFile.FileIndex.High;
		}
		else
		{
			FILE* infile;
			BYTE* FileBuffer = (BYTE*)malloc(BYTES_DO_CHECKSUM_OF);
			if (FileBuffer == NULL)
			{
				fwprintf(stderr, L"Malloc failure.\n");
				exit(EXIT_FAILURE);
			}
			Checksum_t CheckSum;
			memset(&CheckSum, 0, sizeof(CheckSum));

			_wfopen_s(&infile, FileName, L"rb");

			if (infile == NULL)
			{
				if (!HideCantReadMessage)
				{
					ClearProgressInd();
					fwprintf(stderr, L"Can't open '%s'.\n", FileName);
				}
				return;
			}

			size_t BytesToRead = FileSize > BYTES_DO_CHECKSUM_OF ? BYTES_DO_CHECKSUM_OF : FileSize;
			size_t BytesRead = fread(FileBuffer, 1, BytesToRead, infile);
			if (BytesRead != BytesToRead)
			{
				if (!HideCantReadMessage)
				{
					ClearProgressInd();
					fwprintf(stderr, L"File read problem on '%s'.\n", FileName);
				}
				return;
			}
			fclose(infile);

			CalcCrc(&CheckSum, FileBuffer, BytesRead);
			free(FileBuffer);
			CheckSum.Sum += (unsigned int)FileSize;
			if (PrintFileSigs)
			{
				ClearProgressInd();
				wprintf(L"%08lx%08lx %10llu %s\n", CheckSum.Crc, CheckSum.Sum, FileSize, FileName);
			}

			ThisFile.Checksum = CheckSum;
		}
	}

	ThisFile.FileName = _wcsdup(FileName); // allocate the string last, so
										   // we don't waste memory on errors.
	CheckDuplicate(ThisFile);
}

//--------------------------------------------------------------------------
// complain about bad state of the command line.
//--------------------------------------------------------------------------
static void Usage(void)
{
	wprintf(L"finddupe v"VERSION" compiled "__DATE__"\n");
	wprintf(L"Usage: finddupe [options] [-ref] <filepat> [filepat]...\n");
	wprintf(L"Options:\n"
		" -bat <file.bat> Create batch file with commands to do the file\n"
		"                 operations. Check, modify and run batch file afterwards.\n"
		" -hardlink       Create hardlinks.  Works on NTFS file systems only.\n"
		"                 Use with caution!\n"
		" -del            Delete duplicate files.\n"
		" -v              Verbose output.\n"
		" -sigs           Show signatures calculated based on first 32k for each file.\n"
		" -rdonly         Apply to readonly files also (readonly files are skipped by "
		"                 default).\n"
		" -ref <filepat>  Following file pattern are files that are for reference, NOT\n"
		"                 to be eliminated, only used to check duplicates against.\n"
		" -z              Do not skip zero length files (zero length files are ignored\n"
		"                 by default).\n"
		" -u              Do not print a warning for files that cannot be read.\n"
		" -p              Hide progress indicator (useful when redirecting to a file).\n"
		" -j              Follow NTFS junctions and reparse points (off by default).\n"
		" -listlink       hardlink list mode.  Not valid with -del, -bat, -hardlink,\n"
		"                 or -rdonly, options\n"
		" filepat         Pattern for files.  Examples:\n"
		"                  c:\\**         Match everything on drive C\n"
		"                  c:\\**\\*.jpg  Match only .jpg files on drive C\n"
		"                  **\\foo\\**    Match any path with component foo\n"
		"                                 from current directory down\n"

	);
	exit(EXIT_FAILURE);
}

//--------------------------------------------------------------------------
// The main program.
//--------------------------------------------------------------------------
int wmain(int argc, WCHAR** argv)
{
	setlocale(LC_ALL, "");
	int argn;
	WCHAR* arg;
	WCHAR DefaultDrive;
	WCHAR DriveUsed = L'\0';

	for (argn = 1; argn < argc; argn++)
	{
		arg = argv[argn];
		if (arg[0] != L'-')
			break; // Filenames from here on.

		if (!wcscmp(arg, L"-h"))
		{
			Usage();
		}
		else if (!wcscmp(arg, L"-bat"))
		{
			BatchFileName = argv[++argn];
			if (argn >= argc) {
				wprintf(L"Please specify output batch file name after argument '-bat'.\n");
				exit(-1);
			}
		}
		else if (!wcscmp(arg, L"-v"))
		{
			PrintDuplicates = true;
			PrintFileSigs = true;
			Verbose = true;
			HideCantReadMessage = false;
		}
		else if (!wcscmp(arg, L"-sigs"))
		{
			PrintDuplicates = false;
			PrintFileSigs = true;
		}
		else if (!wcscmp(arg, L"-hardlink"))
		{
			MakeHardLinks = true;
		}
		else if (!wcscmp(arg, L"-del"))
		{
			DelDuplicates = true;
		}
		else if (!wcscmp(arg, L"-rdonly"))
		{
			DoReadonly = true;
		}
		else if (!wcscmp(arg, L"-listlink"))
		{
			HardlinkSearchMode = true;
		}
		else if (!wcscmp(arg, L"-ref"))
		{
			break;
		}
		else if (!wcscmp(arg, L"-z"))
		{
			SkipZeroLength = false;
		}
		else if (!wcscmp(arg, L"-u"))
		{
			HideCantReadMessage = true;
		}
		else if (!wcscmp(arg, L"-p"))
		{
			ShowProgress = false;
		}
		else if (!wcscmp(arg, L"-j"))
		{
			FollowReparse = true;
		}
		else
		{
			wprintf(L"Argument '%s' not understood.  Use -h for help.\n", arg);
			exit(-1);
		}
	}

	if (argn > argc)
	{
		fwprintf(stderr, L"Missing argument!  Use -h for help.\n");
		exit(EXIT_FAILURE);
	}

	if (argn == argc)
	{
		fwprintf(stderr, L"No files to process.   Use -h for help.\n");
		exit(EXIT_FAILURE);
	}

	if (HardlinkSearchMode)
	{
		if (BatchFileName || MakeHardLinks || DelDuplicates || DoReadonly)
		{
			fwprintf(stderr, L"Listlink option is not valid with any other options other than -v.\n");
			exit(EXIT_FAILURE);
		}
	}

	FileData = malloc(sizeof(FileData_t) * NumAllocated);
	if (FileData == NULL)
	{
		fwprintf(stderr, L"Malloc failure.");
		exit(EXIT_FAILURE);
	}

	if (BatchFileName)
	{
		_wfopen_s(&BatchFile, BatchFileName, L"w");
		if (BatchFile == NULL)
		{
			wprintf(L"Unable to open task batch file '%s'.\n", BatchFileName);
			exit(EXIT_FAILURE);
		}
		fprintf(BatchFile, "@echo off\n");
		fprintf(BatchFile, "REM Batch file for replacing duplicates with hard links\n");
		fprintf(BatchFile, "REM Created by finddupe program\n\n");
	}

	memset(&DupeStats, 0, sizeof(DupeStats));

	{
		WCHAR CurrentDir[_MAX_PATH];
#pragma warning(disable:6031)
		_wgetcwd(CurrentDir, _MAX_PATH);
		DefaultDrive = towlower(*CurrentDir);
	}

	for (; argn < argc; argn++)
	{
		size_t a;
		WCHAR Drive;
		FilesMatched = 0;

		if (!wcscmp(argv[argn], L"-ref"))
		{
			ReferenceFiles = true;
			argn += 1;
			if (argn >= argc)
				continue;
		}

		for (a = 0;; a++)
		{
			if (argv[argn][a] == L'\0')
				break;
			if (argv[argn][a] == L'/')
				argv[argn][a] = L'\\';
		}

		if (argv[argn][1] == L':')
		{
			Drive = towlower(argv[argn][0]);
		}
		else
		{
			Drive = DefaultDrive;
		}
		if (DriveUsed == L'\0')
			DriveUsed = Drive;
		if (DriveUsed != Drive)
		{
			if (MakeHardLinks)
			{
				fwprintf(stderr, L"Error: Hardlinking across different drives not possible.\n");
				return EXIT_FAILURE;
			}
		}

		// Use my globbing module to do fancier wildcard expansion with recursive
		// subdirectories under Windows.
		MyGlob(argv[argn], FollowReparse, ProcessFile);

		if (FilesMatched == 0)
		{
			fwprintf(stderr, L"Error: No files matched '%s'.\n", argv[argn]);
			return EXIT_FAILURE;
		}
	}

	if (HardlinkSearchMode)
	{
		ClearProgressInd();
		wprintf(L"\n");
		DupeStats.HardlinkGroups = 0;
		ShowLinkGroups(1);
		wprintf(L"\nNumber of hardlink groups found: %llu.\n", DupeStats.HardlinkGroups);
	}
	else
	{
		if (DupeStats.TotalFiles == 0)
		{
			fwprintf(stderr, L"No files to process.\n");
			return EXIT_FAILURE;
		}

		if (BatchFile)
		{
			fclose(BatchFile);
			BatchFile = NULL;
		}

		// Print summary data
		ClearProgressInd();
		wprintf(L"\n");
		wprintf(L"Files: %8llu kBytes in %5llu files\n", (ULONGLONG)(DupeStats.TotalBytes / 1000), DupeStats.TotalFiles);
		wprintf(L"Dupes: %8llu kBytes in %5llu files\n", (ULONGLONG)(DupeStats.DuplicateBytes / 1000), DupeStats.DuplicateFiles);
	}
	if (DupeStats.ZeroLengthFiles)
	{
		wprintf(L"  %llu files of zero length were skipped\n", DupeStats.ZeroLengthFiles);
	}
	if (DupeStats.CantReadFiles)
	{
		wprintf(L"  %llu files could not be opened\n", DupeStats.CantReadFiles);
	}

	return EXIT_SUCCESS;
}
