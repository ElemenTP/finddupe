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
#define WIN32_LEAN_AND_MEAN // To keep windows.h bloat down.
#include <windows.h>

#define TRUE 1
#define FALSE 0

//#define DEBUGGING

typedef struct
{
    WCHAR *Name;
    unsigned int attrib;
} FileEntry;

#ifdef DEBUGGING
//--------------------------------------------------------------------------------
// Dummy function to show operation.
//--------------------------------------------------------------------------------
void ShowName(const char *FileName)
{
    printf("     %s\n", FileName);
}
#endif

//--------------------------------------------------------------------------------
// Simple path splicing (assumes no '\' in either part)
//--------------------------------------------------------------------------------
static int CatPath(WCHAR *dest, const WCHAR *p1, const WCHAR *p2)
{
    size_t l;
    l = wcslen(p1);
    if (!l)
    {
        wcscpy(dest, p2);
    }
    else
    {
        if (l + wcslen(p2) > _MAX_PATH - 2)
        {
            //fprintf(stderr,"\n\n\nPath too long:    \n    %s + %s\n",p1,p2);
            return 0;
        }
        memcpy(dest, p1, (l + 1) * sizeof(WCHAR));
        if (dest[l - 1] != L'\\' && dest[l - 1] != L':')
        {
            dest[l++] = L'\\';
        }
        wcscpy(dest + l, p2);
    }
    return 1;
}

//--------------------------------------------------------------------------------
// Qsort compare function
//--------------------------------------------------------------------------------
int CompareFunc(const void *f1, const void *f2)
{
    return wcscmp(((FileEntry *)f1)->Name, ((FileEntry *)f2)->Name);
}

//--------------------------------------------------------------------------------
// Check if directory is a reparse point
//--------------------------------------------------------------------------------
int IsReparsePoint(WCHAR *DirName)
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
    if (FileHandle == (void *)-1)
    {
        return FALSE;
    }

    if (!GetFileInformationByHandle(FileHandle, &FileInfo))
    {
        return FALSE;
    }

    // Directory node is in: FileInfo.nFileIndexHigh, FileInfo.nFileIndexLow

    if (FileInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

//--------------------------------------------------------------------------------
// Decide how a particular pattern should be handled, and call function for each.
//--------------------------------------------------------------------------------
static void Recurse(const WCHAR *Pattern, int FollowReparse, void (*FileFuncParm)(const WCHAR *FileName))
{
    WCHAR BasePattern[_MAX_PATH];
    WCHAR MatchPattern[_MAX_PATH];
    WCHAR PatCopy[_MAX_PATH * 2];

    int a;
    int MatchDirs;
    int BaseEnd, PatternEnd;
    int SawPat;
    int StarStarAt;

    wcscpy(PatCopy, Pattern);

#ifdef DEBUGGING
    printf("\nCalled with '%s'\n", Pattern);
#endif

DoExtraLevel:
    MatchDirs = TRUE;
    BaseEnd = 0;
    PatternEnd = 0;

    SawPat = FALSE;
    StarStarAt = -1;

    // Split the path into base path and pattern to match against using findfirst.
    for (a = 0;; a++)
    {
        if (PatCopy[a] == L'*' || PatCopy[a] == L'?')
        {
            SawPat = TRUE;
        }

        if (PatCopy[a] == L'*' && PatCopy[a + 1] == L'*')
        {
            if (a == 0 || PatCopy[a - 1] == L'\\' || PatCopy[a - 1] == L':')
            {
                if (PatCopy[a + 2] == L'\\' || PatCopy[a + 2] == L'\0')
                {
                    // x\**\y  ---> x\y  x\*\**\y
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
            MatchDirs = FALSE;
            break;
        }
    }

    wcsncpy(BasePattern, PatCopy, BaseEnd);
    BasePattern[BaseEnd] = 0;

    wcsncpy(MatchPattern, PatCopy, PatternEnd);
    MatchPattern[PatternEnd] = 0;

#ifdef DEBUGGING
    printf("Base:%s  Pattern:%s dirs:%d\n", BasePattern, MatchPattern, MatchDirs);
#endif

    {
        FileEntry *FileList = NULL;
        int NumAllocated = 0;
        int NumHave = 0;

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
                FileList = realloc(FileList, NumAllocated * sizeof(FileEntry));
                if (FileList == NULL)
                    goto nomem;
            }
            a = wcslen(finddata.name);
            FileList[NumHave].Name = malloc((a + 1) * sizeof(WCHAR));
            if (FileList[NumHave].Name == NULL)
            {
            nomem:
                printf("malloc failure\n");
                exit(-1);
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
                        wcscat(CombinedName, PatCopy + PatternEnd);
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

    if (StarStarAt >= 0)
    {
        wcscpy(MatchPattern, PatCopy + StarStarAt);
        PatCopy[StarStarAt] = 0;
        wcscpy(PatCopy + StarStarAt, L"*\\**\\");
        wcscat(PatCopy, MatchPattern);

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
int MyGlob(const WCHAR *Pattern, int FollowReparse, void (*FileFuncParm)(const WCHAR *FileName))
{
    int a;
    WCHAR PathCopy[_MAX_PATH];

    wcsncpy(PathCopy, Pattern, _MAX_PATH - 1);
    a = wcslen(PathCopy);
    if (a && PathCopy[a - 1] == L'\\')
    { // Endsi with backslash
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
            printf("Stat failed\n");
            return -1;
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
int main(int argc, char **argv)
{
    int argn;
    char *arg;

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
