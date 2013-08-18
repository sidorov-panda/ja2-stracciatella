#include "DefaultContentManager.h"

#include <stdexcept>

#include "Build/Directories.h"

// XXX: GameRes.h should be integrated to ContentManager
#include "Build/GameRes.h"

// XXX
#include "Build/GameState.h"

#include "sgp/FileMan.h"
#include "sgp/LibraryDataBase.h"
#include "sgp/MemMan.h"
#include "sgp/StrUtils.h"
#include "sgp/UTF8String.h"

#include "slog/slog.h"
#define TAG "DefaultCM"

#define BASEDATADIR    "data"

#define MAPSDIR        "maps"
#define RADARMAPSDIR   "radarmaps"
#define TILESETSDIR    "tilesets"

#define PRINT_OPENING_FILES (0)

#define DIALOGUESIZE 240

static void LoadEncryptedData(STRING_ENC_TYPE encType, SGPFile* const File, wchar_t* DestString, UINT32 const seek_chars, UINT32 const read_chars)
{
	FileSeek(File, seek_chars * 2, FILE_SEEK_FROM_START);

	UINT16 *Str = MALLOCN(UINT16, read_chars);
	FileRead(File, Str, sizeof(UINT16) * read_chars);

	Str[read_chars - 1] = '\0';
	for (const UINT16* i = Str; *i != '\0'; ++i)
	{
		/* "Decrypt" the ROT-1 "encrypted" data */
		wchar_t c = (*i > 33 ? *i - 1 : *i);

    if(encType == SE_RUSSIAN)
    {
      /* The Russian data files are incorrectly encoded. The original texts seem to
       * be encoded in CP1251, but then they were converted from CP1252 (!) to
       * UTF-16 to store them in the data files. Undo this damage here. */
      if (0xC0 <= c && c <= 0xFF) c += 0x0350;
    }
    else
    {
      if(encType == SE_ENGLISH)
      {
        /* The English data files are incorrectly encoded. The original texts seem
         * to be encoded in CP437, but then they were converted from CP1252 (!) to
         * UTF-16 to store them in the data files. Undo this damage here. This
         * problem only occurs for a few lines by Malice. */
        switch (c)
        {
        case 128: c = 0x00C7; break; // Ç
        case 130: c = 0x00E9; break; // é
        case 135: c = 0x00E7; break; // ç
        }
      }
      else if(encType == SE_POLISH)
      {
        /* The Polish data files are incorrectly encoded. The original texts seem to
         * be encoded in CP1250, but then they were converted from CP1252 (!) to
         * UTF-16 to store them in the data files. Undo this damage here.
         * Also the format code for centering texts differs. */
        switch (c)
        {
        case 143: c = 0x0179; break;
        case 163: c = 0x0141; break;
        case 165: c = 0x0104; break;
        case 175: c = 0x017B; break;
        case 179: c = 0x0142; break;
        case 182: c = 179;    break; // not a char, but a format code (centering)
        case 185: c = 0x0105; break;
        case 191: c = 0x017C; break;
        case 198: c = 0x0106; break;
        case 202: c = 0x0118; break;
        case 209: c = 0x0143; break;
        case 230: c = 0x0107; break;
        case 234: c = 0x0119; break;
        case 241: c = 0x0144; break;
        case 338: c = 0x015A; break;
        case 339: c = 0x015B; break;
        case 376: c = 0x017A; break;
        }
      }

      /* Cyrillic texts (by Ivan Dolvich) in the non-Russian versions are encoded
       * in some wild manner. Undo this damage here. */
      if (0x044D <= c && c <= 0x0452) // cyrillic A to IE
      {
        c += -0x044D + 0x0410;
      }
      else if (c == 0x0453) // cyrillic IO
      {
        c = 0x0401;
      }
      else if (0x0454 <= c && c <= 0x0467) // cyrillic ZHE to SHCHA
      {
        c += -0x0454 + 0x0416;
      }
      else if (0x0468 <= c && c <= 0x046C) // cyrillic YERU to YA
      {
        c += -0x0468 + 0x042B;
      }
    }

		*DestString++ = c;
	}
	*DestString = L'\0';
  MemFree(Str);
}

DefaultContentManager::DefaultContentManager(const std::string &configFolder, const std::string &configPath,
                                             const std::string &gameResRootPath)
{
  /*
   * Searching actual paths to directories 'Data' and 'Data/Tilecache', 'Data/Maps'
   * On case-sensitive filesystems that might be tricky: if such directories
   * exist we should use them.  If doesn't exist, then use lowercased names.
   */

  m_configFolder = configFolder;
  m_gameResRootPath = gameResRootPath;

  m_dataDir = FileMan::joinPaths(gameResRootPath, BASEDATADIR);
  m_tileDir = FileMan::joinPaths(m_dataDir, TILECACHEDIR);

#if CASE_SENSITIVE_FS

  // need to find precise names of the directories

  std::string name;
  if(FileMan::findObjectCaseInsensitive(m_gameResRootPath.c_str(), BASEDATADIR, false, true, name))
  {
    m_dataDir = FileMan::joinPaths(m_gameResRootPath, name);
  }

  if(FileMan::findObjectCaseInsensitive(m_dataDir.c_str(), TILECACHEDIR, false, true, name))
  {
    m_tileDir = FileMan::joinPaths(m_dataDir, name);
  }
#endif

  std::vector<std::string> libraries = GetResourceLibraries(m_dataDir);

  // XXX
  if(GameState::getInstance()->isEditorMode())
  {
    libraries.push_back("editor.slf");
  }

  m_libraryDB = new LibraryDB();

  const char *failedLib = m_libraryDB->InitializeFileDatabase(m_dataDir, libraries);
  if(failedLib)
  {
    std::string message = FormattedString(
      "Library '%s' is not found in folder '%s'.\n\nPlease make sure that '%s' contains files of the original game.  You can change this path by editing file '%s'.\n",
      failedLib, m_dataDir.c_str(), m_gameResRootPath.c_str(), configPath.c_str());
    throw LibraryFileNotFoundException(message);
  }
}

DefaultContentManager::~DefaultContentManager()
{
  if(m_libraryDB)
  {
    m_libraryDB->ShutDownFileDatabase();
    delete m_libraryDB;
  }
}

/** Get map file path. */
std::string DefaultContentManager::getMapPath(const char *mapName) const
{
  std::string result = MAPSDIR;
  result += "/";
  result += mapName;

  SLOGD(TAG, "map file %s", result.c_str());

  return result;
}

/** Get radar map resource name. */
std::string DefaultContentManager::getRadarMapResourceName(const std::string &mapName) const
{
  std::string result = RADARMAPSDIR;
  result += "/";
  result += mapName;

  SLOGD(TAG, "map file %s", result.c_str());

  return result;
}

/** Get tileset resource name. */
std::string DefaultContentManager::getTilesetResourceName(int number, std::string fileName) const
{
  return FormattedString("%s/%d/%s", TILESETSDIR, number, fileName.c_str());
}


/** Get tileset db resource name. */
std::string DefaultContentManager::getTilesetDBResName() const
{
  return BINARYDATADIR "/ja2set.dat";
}

std::string DefaultContentManager::getMapPath(const wchar_t *mapName) const
{
  SLOGW(TAG, "converting wchar to char");

  // This will not work for non-latin names.
  // But it is just a hack to make the code compile.
  // XXX: This method should be removed altogether

  UTF8String str(mapName);
  return getMapPath(str.getUTF8());
}

/** Open map for reading. */
SGPFile* DefaultContentManager::openMapForReading(const std::string& mapName) const
{
  return openGameResForReading(getMapPath(mapName.c_str()));
}

SGPFile* DefaultContentManager::openMapForReading(const wchar_t *mapName) const
{
  return openGameResForReading(getMapPath(mapName));
}

/** Get directory for storing new map file. */
std::string DefaultContentManager::getNewMapFolder() const
{
  return FileMan::joinPaths(m_dataDir, MAPSDIR);
}

/** Get all available maps. */
std::vector<std::string> DefaultContentManager::getAllMaps() const
{
  return FindFilesInDir(MAPSDIR, ".dat", true, true, true);
}

/** Get all available tilecache. */
std::vector<std::string> DefaultContentManager::getAllTilecache() const
{
  return FindFilesInDir(m_tileDir, ".jsd", true, false);
}

/* Open a game resource file for reading.
 *
 * First trying to open the file normally. It will work if the path is absolute
 * and the file is found or path is relative to the current directory (game
 * settings directory) and file is present.
 * If file is not found, try to find it relatively to 'Data' directory.
 * If file is not found, try to find the file in libraries located in 'Data' directory; */
SGPFile* DefaultContentManager::openGameResForReading(const char* filename) const
{
  int         mode;
  const char* fmode = GetFileOpenModeForReading(&mode);

  int d;

  {
    d = FileMan::openFileForReading(filename, mode);
    if (d < 0)
    {
      // failed to open file in the local directory
      // let's try Data
      d = FileMan::openFileCaseInsensitive(m_dataDir, filename, mode);
      if (d < 0)
      {
        LibraryFile libFile;
        memset(&libFile, 0, sizeof(libFile));

        // failed to open in the data dir
        // let's try libraries

        // XXX: need to optimize this
        // XXX: the whole LibraryDataBase thing requires refactoring
        std::string _filename(filename);
        FileMan::slashifyPath(_filename);
        if (m_libraryDB->FindFileInTheLibrarry(_filename, &libFile))
        {
#if PRINT_OPENING_FILES
          SLOGD(TAG, "Opened file (from library ): %s", filename);
#endif
          SGPFile *file = MALLOCZ(SGPFile);
          file->flags = SGPFILE_NONE;
          file->u.lib = libFile;
          return file;
        }
      }
      else
      {
#if PRINT_OPENING_FILES
        SLOGD(TAG, "Opened file (from data dir): %s", filename);
#endif
      }
    }
    else
    {
#if PRINT_OPENING_FILES
      SLOGD(TAG, "Opened file (current dir  ): %s", filename);
#endif
    }
  }

  return FileMan::getSGPFileFromFD(d, filename, fmode);
}

/** Open user's private file (e.g. saved game, settings) for reading. */
SGPFile* DefaultContentManager::openUserPrivateFileForReading(const std::string& filename) const
{
  int         mode;
  const char* fmode = GetFileOpenModeForReading(&mode);

  int d = FileMan::openFileForReading(filename.c_str(), mode);
  return FileMan::getSGPFileFromFD(d, filename.c_str(), fmode);
}

SGPFile* DefaultContentManager::openGameResForReading(const std::string& filename) const
{
  return openGameResForReading(filename.c_str());
}

/* Checks if a game resource exists. */
bool DefaultContentManager::doesGameResExists(char const* filename) const
{
	FILE* file = fopen(filename, "rb");
	if (!file)
	{
		char path[512];
		snprintf(path, lengthof(path), "%s/%s", m_dataDir.c_str(), filename);
		file = fopen(path, "rb");
		if (!file) return m_libraryDB->CheckIfFileExistInLibrary(filename);
	}

	fclose(file);
	return true;
}

bool DefaultContentManager::doesGameResExists(const std::string &filename) const
{
  return doesGameResExists(filename.c_str());
}

std::string DefaultContentManager::getScreenshotFolder() const
{
  return m_configFolder;
}

std::string DefaultContentManager::getVideoCaptureFolder() const
{
  return m_configFolder;
}

/** Get folder for saved games. */
std::string DefaultContentManager::getSavedGamesFolder() const
{
  return FileMan::joinPaths(m_configFolder, "SavedGames");
}

/** Load encrypted string from game resource file. */
void DefaultContentManager::loadEncryptedString(const char *fileName, wchar_t* DestString, uint32_t seek_chars, uint32_t read_chars) const
{
  AutoSGPFile File(openGameResForReading(fileName));
  loadEncryptedString(File, DestString, seek_chars, read_chars);
}

void DefaultContentManager::loadEncryptedString(SGPFile* const File, wchar_t* DestString, uint32_t const seek_chars, uint32_t const read_chars) const
{
  LoadEncryptedData(getStringEncType(), File, DestString, seek_chars, read_chars);
}

/** Load dialogue quote from file. */
UTF8String* DefaultContentManager::loadDialogQuoteFromFile(const char* fileName, int quote_number)
{
  AutoSGPFile File(openGameResForReading(fileName));

  wchar_t quote[DIALOGUESIZE];
  LoadEncryptedData(getStringEncType(), File, quote, quote_number * DIALOGUESIZE, DIALOGUESIZE);
  return new UTF8String(quote);
}

/** Load all dialogue quotes for a character. */
void DefaultContentManager::loadAllDialogQuotes(STRING_ENC_TYPE encType, const char* fileName, std::vector<UTF8String*> &quotes) const
{
  AutoSGPFile File(openGameResForReading(fileName));
  uint32_t fileSize = FileGetSize(File);
  uint32_t numQuotes = fileSize / DIALOGUESIZE / 2;
  // SLOGI(TAG, "%d quotes in dialog %s", numQuotes, fileName);
  for(int i = 0; i < numQuotes; i++)
  {
    wchar_t quote[DIALOGUESIZE];
    LoadEncryptedData(encType, File, quote, i * DIALOGUESIZE, DIALOGUESIZE);
    quotes.push_back(new UTF8String(quote));
  }
}
