#include "unzipper.h"
#include "defs.h"
#include "tools.h"
#include "minizip/minishared.h"

#include <functional>
#include <exception>
#include <sstream>
#include <fstream>
#include <stdexcept>

namespace zipper {

  struct Unzipper::Impl
  {
    Unzipper& m_outer;
    zipFile m_zf;
    ourmemory_t m_zipmem;
    zlib_filefunc_def m_filefunc;

  private:

    bool initMemory(zlib_filefunc_def& filefunc)
    {
      m_zf = unzOpen2("__notused__", &filefunc);
      return m_zf != NULL;
    }

    bool locateEntry(const std::string& name)
    {
      return UNZ_OK == unzLocateFile(m_zf, name.c_str(), NULL);
    }

    ZipEntry currentEntryInfo()
    {
      unz_file_info64 file_info = { 0 };
      char filename_inzip[256] = { 0 };
      struct tm tmu_date = { 0 };

      int err = unzGetCurrentFileInfo64(m_zf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0);
      if (UNZ_OK != err)
        throw EXCEPTION_CLASS("Error, couln't get the current entry info");

      dosdate_to_tm(file_info.dos_date, &tmu_date);

      return ZipEntry(std::string(filename_inzip),
        file_info.compressed_size, file_info.uncompressed_size,
        tmu_date);
    }

#if 0
    // lambda as a parameter https://en.wikipedia.org/wiki/C%2B%2B11#Polymorphic_wrappers_for_function_objects
    void iterEntries(std::function<void(ZipEntry&)> callback)
    {
      int err = unzGoToFirstFile(m_zf);
      if (UNZ_OK == err)
      {
        do
        {
          ZipEntry entryinfo = currentEntryInfo();

          if (entryinfo.valid())
          {
            callback(entryinfo);
            err = unzGoToNextFile(m_zf);
          }
          else
            err = UNZ_ERRNO;

        } while (UNZ_OK == err);

        if (UNZ_END_OF_LIST_OF_FILE != err && UNZ_OK != err)
          return;
      }
    }
#endif

    void getEntries(std::vector<ZipEntry>& entries)
    {
      int err = unzGoToFirstFile(m_zf);
      if (UNZ_OK == err)
      {
        do
        {
          ZipEntry entryinfo = currentEntryInfo();

          if (entryinfo.valid())
          {
            entries.push_back(entryinfo);
            err = unzGoToNextFile(m_zf);
          }
          else
            err = UNZ_ERRNO;

        } while (UNZ_OK == err);

        if (UNZ_END_OF_LIST_OF_FILE != err && UNZ_OK != err)
          return;
      }
    }


  public:
#if 0
    bool extractCurrentEntry(ZipEntry& entryinfo, int (extractStrategy)(ZipEntry&) )
    {
      int err = UNZ_OK;

      if (!entryinfo.valid())
        return false;

      err = extractStrategy(entryinfo);
      if (UNZ_OK == err)
      {
        err = unzCloseCurrentFile(m_zf);
        if (UNZ_OK != err)
          throw EXCEPTION_CLASS(("Error " + std::to_string(err) + " closing internal file '" + entryinfo.name +
            "' in zip").c_str());
      }

      return UNZ_OK == err;
    }
#endif

    bool extractCurrentEntryToFile(ZipEntry& entryinfo, const std::string& fileName)
    {
      int err = UNZ_OK;

      if (!entryinfo.valid())
        return false;

      err = extractToFile(fileName, entryinfo);
      if (UNZ_OK == err)
      {
        err = unzCloseCurrentFile(m_zf);
        if (UNZ_OK != err)
        {
          std::stringstream str;
          str << "Error " << err << " openinginternal file '"
              << entryinfo.name << "' in zip";

          throw EXCEPTION_CLASS(str.str().c_str());
        }
      }

      return UNZ_OK == err;
    }

    bool extractCurrentEntryToStream(ZipEntry& entryinfo, std::ostream& stream)
    {
      int err = UNZ_OK;

      if (!entryinfo.valid())
        return false;

      err = extractToStream(stream, entryinfo);
      if (UNZ_OK == err)
      {
        err = unzCloseCurrentFile(m_zf);
        if (UNZ_OK != err)
        {
          std::stringstream str;
          str << "Error " << err << " opening internal file '"
              << entryinfo.name << "' in zip";

          throw EXCEPTION_CLASS(str.str().c_str());
        }
      }

      return UNZ_OK == err;
    }

    bool extractCurrentEntryToMemory(ZipEntry& entryinfo, std::vector<unsigned char>& outvec)
    {
      int err = UNZ_OK;

      if (!entryinfo.valid())
        return false;

      err = extractToMemory(outvec, entryinfo);
      if (UNZ_OK == err)
      {
        err = unzCloseCurrentFile(m_zf);
        if (UNZ_OK != err)
        {
          std::stringstream str;
          str << "Error " << err << " opening internal file '"
              << entryinfo.name << "' in zip";

          throw EXCEPTION_CLASS(str.str().c_str());
        }
      }

      return UNZ_OK == err;
    }

    void changeFileDate(const std::string& filename, struct tm tmu_date)
    {
#ifdef _WIN32
      HANDLE hFile;
      FILETIME ftm, ftCreate, ftLastAcc, ftLastWrite;
      uint32_t dos_date;

      hFile = CreateFileA(filename.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
      if (hFile != INVALID_HANDLE_VALUE)
      {
        dos_date = tm_to_dosdate(tmu_date);
        DosDateTimeToFileTime((WORD)(dos_date >> 16), (WORD)dos_date, &ftm);

        GetFileTime(hFile, &ftCreate, &ftLastAcc, &ftLastWrite);
        SetFileTime(hFile, &ftm, &ftLastAcc, &ftm);
        CloseHandle(hFile);
      }
#else
#if defined unix || defined __APPLE__
      struct utimbuf ut;

      ut.actime = ut.modtime = timegm(&tmu_date);
      utime(filename.c_str(), &ut);
#endif
#endif
    }


    int extractToFile(const std::string& filename, ZipEntry& info)
    {
      int err = UNZ_ERRNO;

      /* If zip entry is a directory then create it on disk */
      makedir(parentDirectory(filename));

      /* Create the file on disk so we can unzip to it */
      std::ofstream output_file(filename.c_str(), std::ofstream::binary);

      if (output_file.good())
      {
        if (extractToStream(output_file, info))
          err = UNZ_OK;

        output_file.close();

        /* Set the time of the file that has been unzipped */
        changeFileDate(filename, info.timestamp);
      }
      else
        output_file.close();

      return err;
    }

    int extractToStream(std::ostream& stream, ZipEntry& info)
    {
      int err = UNZ_ERRNO;

      err = unzOpenCurrentFilePassword(m_zf, m_outer.m_password.c_str());
      if (UNZ_OK != err)
      {
        std::stringstream str;
        str << "Error " << err << " opening internal file '"
            << info.name << "' in zip";

        throw EXCEPTION_CLASS(str.str().c_str());
      }

      std::vector<char> buffer;
      buffer.resize(WRITEBUFFERSIZE);

      do
      {
        err = unzReadCurrentFile(m_zf, buffer.data(), (unsigned int)buffer.size());
        if (err < 0 /*error*/ || err == 0 /*eof*/)
          break;

        stream.write(buffer.data(), err);
        if (!stream.good())
        {
          err = UNZ_ERRNO;
          break;
        }

      } while (err > 0);

      stream.flush();

      return err;
    }

    int extractToMemory(std::vector<unsigned char>& outvec, ZipEntry& info)
    {
      int err = UNZ_ERRNO;

      err = unzOpenCurrentFilePassword(m_zf, m_outer.m_password.c_str());
      if (UNZ_OK != err)
      {
        std::stringstream str;
        str << "Error " << err << " opening internal file '"
            << info.name << "' in zip";

        throw EXCEPTION_CLASS(str.str().c_str());
      }

      std::vector<unsigned char> buffer;
      buffer.resize(WRITEBUFFERSIZE);

      outvec.reserve((size_t)info.uncompressedSize);

      do
      {
        err = unzReadCurrentFile(m_zf, buffer.data(), (unsigned int)buffer.size());
        if (err < 0 /*error*/ || err == 0 /*eof*/)
          break;

        outvec.insert(outvec.end(), buffer.data(), buffer.data() + err);

      } while (err > 0);

      return err;
    }

  public:

    Impl(Unzipper& outer) : m_outer(outer), m_zipmem(), m_filefunc()
    {
      m_zf = NULL;
    }

    ~Impl()
    {
    }

    void close()
    {
      if (m_zf)
        unzClose(m_zf);
    }

    bool initFile(const std::string& filename)
    {
#ifdef USEWIN32IOAPI
      zlib_filefunc64_def ffunc;
      fill_win32_filefunc64A(&ffunc);
      m_zf = unzOpen2_64(filename.c_str(), &ffunc);
#else
      m_zf = unzOpen64(filename.c_str());
#endif
      return m_zf != NULL;
    }

    bool initWithStream(std::istream& stream)
    {
      stream.seekg(0, std::ios::end);
      size_t size = (size_t)stream.tellg();
      stream.seekg(0);

      if (size > 0)
      {
        m_zipmem.base = new char[size];
        stream.read(m_zipmem.base, size);
      }

      fill_memory_filefunc(&m_filefunc, &m_zipmem);

      return initMemory(m_filefunc);
    }

    bool initWithVector(std::vector<unsigned char>& buffer)
    {
      if (!buffer.empty())
      {
        m_zipmem.base = (char*)buffer.data();
        m_zipmem.size = (uLong)buffer.size();
      }

      fill_memory_filefunc(&m_filefunc, &m_zipmem);

      return initMemory(m_filefunc);
    }

    std::vector<ZipEntry> entries()
    {
      std::vector<ZipEntry> entrylist;
      getEntries(entrylist);
      return entrylist;
    }



    bool extractAll(const std::string& destination, const std::map<std::string, std::string>& alternativeNames)
    {
      std::vector<ZipEntry> entries;
      getEntries(entries);
      std::vector<ZipEntry>::iterator it = entries.begin();
      for (; it != entries.end(); ++it)
      {
        if (!locateEntry(it->name))
          continue;

        std::string alternativeName = destination.empty() ? "" : destination + "/";

        if (alternativeNames.find(it->name) != alternativeNames.end())
          alternativeName += alternativeNames.at(it->name);
        else
          alternativeName += it->name;

        this->extractCurrentEntryToFile(*it, alternativeName);
      };

      return true;
    }

    bool extractEntry(const std::string& name, const std::string& destination)
    {
      std::string outputFile = destination.empty() ? name : destination + "\\" + name;

      if (locateEntry(name))
      {
        ZipEntry entry = currentEntryInfo();
        return extractCurrentEntryToFile(entry, outputFile);
      }
      else
      {
        return false;
      }
    }

    bool extractEntryToStream(const std::string& name, std::ostream& stream)
    {
      if (locateEntry(name))
      {
        ZipEntry entry = currentEntryInfo();
        return extractCurrentEntryToStream(entry, stream);
      }
      else
      {
        return false;
      }
    }

    bool extractEntryToMemory(const std::string& name, std::vector<unsigned char>& vec)
    {
      if (locateEntry(name))
      {
        ZipEntry entry = currentEntryInfo();
        return extractCurrentEntryToMemory(entry, vec);
      }
      else
      {
        return false;
      }
    }
  };

  Unzipper::Unzipper(std::istream& zippedBuffer)
    : m_impl(new Impl(*this))
  {
    if (!m_impl->initWithStream(zippedBuffer))
      throw EXCEPTION_CLASS("Error loading zip in memory!");
    m_open = true;
  }

  Unzipper::Unzipper(std::vector<unsigned char>& zippedBuffer)
    : m_impl(new Impl(*this))
  {
    if (!m_impl->initWithVector(zippedBuffer))
      throw EXCEPTION_CLASS("Error loading zip in memory!");

    m_open = true;
  }

  Unzipper::Unzipper(const std::string& zipname)
    : m_zipname(zipname)
    , m_impl(new Impl(*this))
  {
    if (!m_impl->initFile(zipname))
      throw EXCEPTION_CLASS("Error loading zip file!");

    m_open = true;
  }

  Unzipper::Unzipper(const std::string& zipname, const std::string& password)
    : m_zipname(zipname)
    , m_password(password)
    , m_impl(new Impl(*this))
  {
    if (!m_impl->initFile(zipname))
      throw EXCEPTION_CLASS("Error loading zip file!");

    m_open = true;
  }

  Unzipper::~Unzipper(void)
  {
    close();
  }

  std::vector<ZipEntry> Unzipper::entries()
  {
    return m_impl->entries();
  }

  bool Unzipper::extractEntry(const std::string& name, const std::string& destination)
  {
    return m_impl->extractEntry(name, destination);
  }

  bool Unzipper::extractEntryToStream(const std::string& name, std::ostream& stream)
  {
    return m_impl->extractEntryToStream(name, stream);
  }

  bool Unzipper::extractEntryToMemory(const std::string& name, std::vector<unsigned char>& vec)
  {
    return m_impl->extractEntryToMemory(name, vec);
  }


  bool Unzipper::extract(const std::string& destination, const std::map<std::string, std::string>& alternativeNames)
  {
    return m_impl->extractAll(destination, alternativeNames);
  }

  bool
  Unzipper::extract(const std::string& destination)
  {
    return m_impl->extractAll(destination, std::map<std::string, std::string>());
  }

  void Unzipper::close()
  {
    if (m_open)
    {
      m_impl->close();
      m_open = false;
    }
  }

}

