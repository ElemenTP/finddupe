//--------------------------------------------------------------------------------
// Module to do recursive directory file matching under windows.
//
// Tries to do pattern matching to produce similar results as Unix, but using
// the Windows _findfirst to do all the pattern matching.
//
// Also hadles recursive directories - "**" path component expands into
// any levels of subdirectores (ie c:\**\*.c matches ALL .c files on drive c:)
//
// Matthias Wandel Nov 5 2000 - March 2009
//--------------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <io.h>
#include <sys/stat.h>
#include <stdbool.h>
#define WIN32_LEAN_AND_MEAN // To keep windows.h bloat down.
#define _WIN32_WINNT 0x0600
#include <windows.h>

//#define DEBUGGING

typedef struct
{
	WCHAR* Name;
	unsigned int attrib;
} FileEntry;

#ifdef DEBUGGING
//--------------------------------------------------------------------------------
// Dummy function to show operation.
//--------------------------------------------------------------------------------
void ShowName(const char* FileName)
{
	printf("     %s\n", FileName);
}
#endif

//--------------------------------------------------------------------------------
// Simple path splicing (assumes no '\' in either part)
//--------------------------------------------------------------------------------
static bool CatPath(WCHAR* dest, const WCHAR* p1, const WCHAR* p2)
{
	size_t l;
	l = wcslen(p1);
	if (!l)
	{
		wcscpy_s(dest, _MAX_PATH, p2);
	}
	else
	{
		if (l + wcslen(p2) > _MAX_PATH - 2)
		{
			//fprintf(stderr,"\n\n\nPath too long:    \n    %s + %s\n",p1,p2);
			return false;
		}
		memcpy(dest, p1, (l + 1) * sizeof(WCHAR));
		if (dest[l - 1] != L'\\' && dest[l - 1] != L':')
		{
			dest[l++] = L'\\';
		}
		wcscpy_s(dest + l, _MAX_PATH - l, p2);
	}
	return true;
}

//--------------------------------------------------------------------------------
// Qsort compare function
//--------------------------------------------------------------------------------
int CompareFunc(const void* f1, const void* f2)
{
	return wcscmp(((FileEntry*)f1)->Name, ((FileEntry*)f2)->Name);
}

//--------------------------------------------------------------------------------
// Check if directory is a reparse point
//--------------------------------------------------------------------------------
bool IsReparsePoint(WCHAR* DirName)
{
	HANDLE FileHandle;
	BY_HANDLE_FILE_INFORMATION FileInfo;

	FileHandle = CreateFileW(DirName,
		0,                                // dwDesiredAccess
		FILE_SHARE_READ,                  // dwShareMode
		NULL,                             // Security attirbutes
		OPEN_EXISTING,                    // dwCreationDisposition
		FILE_FLAG_BACKUP_SEMANTICS |      // dwFlagsAndAttributes.  Need this to do dirs.
		FILE_FLAG_OPEN_REPARSE_POINT, // Need this flag to open the reparse point instead of following it.
		NULL);                            // hTemplateFile.  Ignored for existing.
	if (FileHandle == (void*)-1)
	{
		return false;
	}

	if (!GetFileInformationByHandle(FileHandle, &FileInfo))
	{
		CloseHandle(FileHandle);
		return false;
	}
	// Directory node is in: FileInfo.nFileIndexHigh, FileInfo.nFileIndexLow

	CloseHandle(FileHandle);
	if (FileInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
	{
		return true;
	}
	else
	{
		return false;
	}
}

//--------------------------------------------------------------------------------
// Decide how a particular pattern should be handled, and call function for each.
//--------------------------------------------------------------------------------
static void Recurse(const WCHAR* Pattern, bool FollowReparse, void (*FileFuncParm)(const WCHAR* FileName))
{
	WCHAR BasePattern[_MAX_PATH];
	WCHAR MatchPattern[_MAX_PATH];
	WCHAR PatCopy[_MAX_PATH * 2];

	size_t a;
	bool MatchDirs, SawPat, HasSSA;
	size_t BaseEnd, PatternEnd, StarStarAt;

	wcscpy_s(PatCopy, _MAX_PATH * 2, Pattern);

#ifdef DEBUGGING
	printf("\nCalled with '%s'\n", Pattern);
#endif

DoExtraLevel:
	MatchDirs = true;
	BaseEnd = 0;
	PatternEnd = 0;

	SawPat = false;
	HasSSA = false;
	StarStarAt = 0;

	// Split the path into base path and pattern to match against using findfirst.
	for (a = 0;; a++)
	{
		if (PatCopy[a] == L'*' || PatCopy[a] == L'?')
		{
			SawPat = true;
		}

		if (PatCopy[a] == L'*' && PatCopy[a + 1] == L'*')
		{
			if (a == 0 || PatCopy[a - 1] == L'\\' || PatCopy[a - 1] == L':')
			{
				if (PatCopy[a + 2] == L'\\' || PatCopy[a + 2] == L'\0')
				{
					// x\**\y  ---> x\y  x\*\**\y
					HasSSA = true;
					StarStarAt = a;
					if (PatCopy[a + 2] != L'\0')
					{
						memcpy(PatCopy + a, PatCopy + a + 3, (wcslen(PatCopy) - a - 1) * sizeof(WCHAR));
					}
					else
					{
						PatCopy[a + 1] = L'\0';
					}
				}
			}
		}

		if (PatCopy[a] == L'\\' || (PatCopy[a] == L':' && PatCopy[a + 1] != L'\\'))
		{
			PatternEnd = a;
			if (SawPat)
				break; // Findfirst can only match one level of wildcard at a time.
			BaseEnd = a + 1;
		}
		if (PatCopy[a] == L'\0')
		{
			PatternEnd = a;
			MatchDirs = false;
			break;
		}
	}

	wcsncpy_s(BasePattern, _MAX_PATH, PatCopy, BaseEnd);
	BasePattern[BaseEnd] = 0;

	wcsncpy_s(MatchPattern, _MAX_PATH, PatCopy, PatternEnd);
	MatchPattern[PatternEnd] = 0;

#ifdef DEBUGGING
	printf("Base:%s  Pattern:%s dirs:%d\n", BasePattern, MatchPattern, MatchDirs);
#endif

	{
		FileEntry* FileList = NULL;
		size_t NumAllocated = 0, NumHave = 0;

		struct _wfinddatai64_t finddata;
		long long find_handle;

		find_handle = _wfindfirst64(MatchPattern, &finddata);

		for (;;)
		{
			if (find_handle == -1)
				break;

			// Eliminate the obvious patterns.
			if (!memcmp(finddata.name, L".", 2 * sizeof(WCHAR)))
				goto next_file;
			if (!memcmp(finddata.name, L"..", 3 * sizeof(WCHAR)))
				goto next_file;

			if (finddata.attrib & _A_SUBDIR)
			{
				if (!MatchDirs)
					goto next_file;
			}
			else
			{
				if (MatchDirs)
					goto next_file;
			}

			// Add it to the list.
			if (NumAllocated <= NumHave)
			{
				NumAllocated = NumAllocated + 10 + NumAllocated / 2;
#pragma warning(disable:6308)
				FileList = (FileEntry*)realloc(FileList, NumAllocated * sizeof(FileEntry));
				if (FileList == NULL)
					goto nomem;
			}
			a = wcslen(finddata.name);
			FileList[NumHave].Name = malloc((a + 1) * sizeof(WCHAR));
			if (FileList[NumHave].Name == NULL)
			{
			nomem:
				fwprintf(stderr, L"Malloc failure.\n");
				exit(EXIT_FAILURE);
			}
			memcpy(FileList[NumHave].Name, finddata.name, (a + 1) * sizeof(WCHAR));
			FileList[NumHave].attrib = finddata.attrib;
			NumHave++;

		next_file:
			if (_wfindnext64(find_handle, &finddata) != 0)
				break;
		}
		_findclose(find_handle);

		// Sort the list...
		qsort(FileList, NumHave, sizeof(FileEntry), CompareFunc);

		// Use the list.
		for (a = 0; a < NumHave; a++)
		{
			WCHAR CombinedName[_MAX_PATH * 2];
			if (FileList[a].attrib & _A_SUBDIR)
			{
				if (CatPath(CombinedName, BasePattern, FileList[a].Name))
				{
					if (FollowReparse || !IsReparsePoint(CombinedName))
					{
						wcscat_s(CombinedName, _MAX_PATH * 2, PatCopy + PatternEnd);
						Recurse(CombinedName, FollowReparse, FileFuncParm);
					}
				}
			}
			else
			{
				if (CatPath(CombinedName, BasePattern, FileList[a].Name))
				{
					FileFuncParm(CombinedName);
				}
			}
			free(FileList[a].Name);
		}
		free(FileList);
	}

	if (HasSSA)
	{
		wcscpy_s(MatchPattern, _MAX_PATH, PatCopy + StarStarAt);
		PatCopy[StarStarAt] = 0;
		wcscpy_s(PatCopy + StarStarAt, 6, L"*\\**\\");
		wcscat_s(PatCopy, _MAX_PATH, MatchPattern);

#ifdef DEBUGGING
		printf("Recurse with '%s'\n", PatCopy);
#endif

		// As this function context is no longer needed, we can just goto back
		// to the top of it to avoid adding another context on the stack.
		goto DoExtraLevel;
	}
}

//--------------------------------------------------------------------------------
// Do quick precheck - if no wildcards, and it names a directory, do whole dir.
//--------------------------------------------------------------------------------
int MyGlob(const WCHAR* Pattern, bool FollowReparse, void (*FileFuncParm)(const WCHAR* FileName))
{
	size_t a;
	WCHAR PathCopy[_MAX_PATH];

	wcsncpy_s(PathCopy, _MAX_PATH, Pattern, _MAX_PATH - 1);
	a = wcslen(PathCopy);
	if (a && PathCopy[a - 1] == L'\\')
	{ // Ends with backslash
		if (!(a == 3 && PathCopy[1] == L':'))
		{
			// and its not something like c:\, then delete the trailing backslash
			PathCopy[a - 1] = L'\0';
		}
	}

	for (a = 0;; a++)
	{
		if (PathCopy[a] == L'*' || PathCopy[a] == L'?')
			break; // Contains wildcards
		if (PathCopy[a] == L'\0')
			break;
	}

	if (PathCopy[a] == L'\0')
	{
		// No wildcards were specified.  Do a whole tree, or file.
		struct _stat64 FileStat;
		if (_wstat64(PathCopy, &FileStat) != 0)
		{
			// There is no file or directory by that name.
			fwprintf(stderr, L"Stat failed.\n");
			exit(EXIT_FAILURE);
		}
		if (FileStat.st_mode & 040000)
		{
			if (CatPath(PathCopy, PathCopy, L"**"))
			{
				Recurse(PathCopy, FollowReparse, FileFuncParm);
			}
		}
		else
		{
			FileFuncParm(PathCopy);
		}
	}
	else
	{
		// A wildcard was specified.
		Recurse(PathCopy, FollowReparse, FileFuncParm);
	}
	return 0;
}

#ifdef DEBUGGING
//--------------------------------------------------------------------------------
// The main program.
//--------------------------------------------------------------------------------
int main(int argc, char** argv)
{
	int argn;
	char* arg;

	for (argn = 1; argn < argc; argn++)
	{
		MyGlob(argv[argn], 1, ShowName);
	}
	return EXIT_SUCCESS;
}
#endif

/*

non-recursive test cases:

	e:\make*\*
	\make*\*
	e:*\*.c
	\*\*.c
	\*
	c:*.c
	c:\*
	..\*.c


recursive test cases:
	**
	**\*.c
	c:\**\*.c
	c:**\*.c
	.\**
	..\**
	c:\

*/
