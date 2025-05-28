/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2022 Osimis S.A., Belgium
 * Copyright (C) 2021-2022 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include <Compatibility.h>
#include <OrthancException.h>
#include <Logging.h>
#include <SystemToolbox.h>
#include <Toolbox.h>

#include "FoldersIndexer.h"
#include "Helpers.h"
#include <stack>
#include <algorithm>


namespace OrthancPlugins
{
  static const char* KVS_ID_INDEXER_PATH = "advst-indexer-path";
  static const char* SERIALIZATION_KEY_VERSION = "v";
  static const char* SERIALIZATION_KEY_IS_DICOM = "d";
  static const char* SERIALIZATION_KEY_SIZE = "s";
  static const char* SERIALIZATION_KEY_TIME = "t";
  static const char* SERIALIZATION_KEY_DELETED = "r";


  class IndexedPath
  {
    std::time_t time_;
    uintmax_t   size_;
    bool        isDicom_;
    bool        hasBeenDeletedByOrthanc_; // has been deleted from Orthanc by a DELETE resource

    IndexedPath();
  public:
    IndexedPath(const std::time_t& time, uintmax_t size, bool isDicom, bool hasBeenDeletedByOrthanc = false) :
      time_(time),
      size_(size),
      isDicom_(isDicom),
      hasBeenDeletedByOrthanc_(hasBeenDeletedByOrthanc)
    {
    }

    static IndexedPath CreateFromSerializedString(const std::string& serialized)
    {
      Json::Value v;
      OrthancPlugins::ReadJson(v, serialized);

      if (v[SERIALIZATION_KEY_VERSION].asInt() == 1)
      {
        return IndexedPath(static_cast<std::time_t>(v[SERIALIZATION_KEY_TIME].asUInt64()),
                           static_cast<uintmax_t>(v[SERIALIZATION_KEY_SIZE].asUInt64()),
                           v[SERIALIZATION_KEY_IS_DICOM].asBool(),
                           v[SERIALIZATION_KEY_DELETED].asBool());
      }
      else
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, std::string("Invalid IndexedPath version: ") + boost::lexical_cast<std::string>(v[SERIALIZATION_KEY_VERSION].asInt()));
      }
    }

    void ToString(std::string& serialized) const
    {
      Json::Value v;
      v[SERIALIZATION_KEY_VERSION] = 1;
      v[SERIALIZATION_KEY_IS_DICOM] = isDicom_;
      v[SERIALIZATION_KEY_SIZE] = Json::Value::UInt64(size_);
      v[SERIALIZATION_KEY_TIME] = Json::Value::UInt64(time_);
      v[SERIALIZATION_KEY_DELETED] = hasBeenDeletedByOrthanc_;

      OrthancPlugins::WriteFastJson(serialized, v);
    }

    bool HasChanged(const std::time_t& time, uintmax_t size) const
    {
      return time != time_ || size_ != size;
    }

    bool IsDicom() const
    {
      return isDicom_;
    }

    void MarkAsDeletedByOrthanc()
    {
      hasBeenDeletedByOrthanc_ = true;
    }

    bool HasBeenDeletedByOrthanc() const
    {
      return hasBeenDeletedByOrthanc_;
    }

  };


  FoldersIndexer::FoldersIndexer(const std::list<std::string>& folders, 
                                 unsigned int intervalInSeconds, 
                                 unsigned int throttleDelayMs,
                                 const std::list<std::string>& parsedExentions,
                                 const std::list<std::string>& skippedExentions) :
    intervalInSeconds_(intervalInSeconds),
    throttleDelayMs_(throttleDelayMs),
    parsedExtensions_(parsedExentions),
    skippedExtensions_(skippedExentions),
    isRunning_(false),
    kvsIndexedPaths_(KVS_ID_INDEXER_PATH)
  {
    for (std::list<std::string>::const_iterator it = folders.begin(); it != folders.end(); ++it)
    {
      folders_.push_back(*it);
    }
  }
  
  FoldersIndexer::~FoldersIndexer()
  {
    Stop();
  }

  static void FoldersIndexerWorkerThread(FoldersIndexer* indexer)
  {
    OrthancPluginSetCurrentThreadName(OrthancPlugins::GetGlobalContext(), "INDEXER");

    indexer->WorkerThread();
  }

  void FoldersIndexer::Start()
  {
    isRunning_ = true;
    thread_ = boost::thread(FoldersIndexerWorkerThread, this);
  }

  void FoldersIndexer::Stop()
  {
    isRunning_ = false;
    if (thread_.joinable())
    {
      thread_.join();
    }
  }

  void FoldersIndexer::WorkerThread()
  {
    while (isRunning_)
    {
      std::stack<boost::filesystem::path> pathStack;

      for (std::list<fs::path>::const_iterator it = folders_.begin();
          it != folders_.end(); ++it)
      {
        pathStack.push(*it);
      }

      while (!pathStack.empty())
      {
        if (!isRunning_)
        {
          return;
        }
        
        boost::filesystem::path d = pathStack.top();
        pathStack.pop();

        boost::filesystem::directory_iterator current;
      
        try
        {
          current = boost::filesystem::directory_iterator(d);
        }
        catch (boost::filesystem::filesystem_error&)
        {
          LOG(WARNING) << "Indexer cannot read directory: " << d.string();
          continue;
        }

        const boost::filesystem::directory_iterator end;
      
        while (current != end)
        {
          try
          {
            const boost::filesystem::file_status status = boost::filesystem::status(current->path());
            
            switch (status.type())
            {
              case boost::filesystem::regular_file:
              case boost::filesystem::reparse_file:
                try
                {
                  // check extensions
                  if (parsedExtensions_.size() > 0 || skippedExtensions_.size() > 0)
                  {
                    std::string extension = boost::filesystem::path(current->path()).extension().string();
                    if (parsedExtensions_.size() > 0 && std::find(parsedExtensions_.begin(), parsedExtensions_.end(), extension) == parsedExtensions_.end())
                    {
                      ++current;
                      continue;
                    }

                    if (skippedExtensions_.size() > 0 && std::find(skippedExtensions_.begin(), skippedExtensions_.end(), extension) != skippedExtensions_.end())
                    {
                      ++current;
                      continue;
                    }
                  }

                  ProcessFile(current->path());
                  
                  if (throttleDelayMs_ > 0)
                  {
                    boost::this_thread::sleep(boost::posix_time::milliseconds(throttleDelayMs_));
                  }
                }
                catch (Orthanc::OrthancException& e)
                {
                  LOG(ERROR) << "Indexer: " << e.What();
                }              
                break;
            
              case boost::filesystem::directory_file:
                pathStack.push(current->path());
                break;
            
              default:
                break;
            }
          }
          catch (boost::filesystem::filesystem_error&)
          {
          }

          ++current;
        }
      }

      try
      {
        LookupDeletedFiles();
      }
      catch (Orthanc::OrthancException& e)
      {
        LOG(ERROR) << e.What();
      }
      
      for (unsigned int i = 0; i < intervalInSeconds_ * 10; i++)
      {
        if (!isRunning_)
        {
          return;
        }
        
        boost::this_thread::sleep(boost::posix_time::milliseconds(100));
      }
    }
  }

  void FoldersIndexer::ProcessFile(const fs::path& path)
  {
    std::time_t lastWriteTime = boost::filesystem::last_write_time(path);
    uintmax_t fileSize = boost::filesystem::file_size(path);

    std::string serialized;

    if (kvsIndexedPaths_.GetValue(serialized, path.string()))
    {
      // this is not a new file but it might have been modified
      IndexedPath indexedPath = IndexedPath::CreateFromSerializedString(serialized);
      
      if (indexedPath.HasChanged(lastWriteTime, fileSize))
      {
        if (indexedPath.IsDicom())
        {
          LOG(INFO) << "Indexer: a DICOM file has changed and will be re-adopted: " << path.string();
          // abandon previous file, we will re-adopt it
          AbandonFile(path.string());
        }

        kvsIndexedPaths_.DeleteKey(path.string());
      }
      else
      {
        // file has not changed and is already in the store
        return;
      }
    }

    // this is a new file or a modified one that has changed -> adopt it
    std::string instanceId, attachmentUuid;
    OrthancPluginStoreStatus storeStatus;    
    
    AdoptFile(instanceId, attachmentUuid, storeStatus, path.string(), false);
    bool isDicom = storeStatus == OrthancPluginStoreStatus_Success;

    if (isDicom)
    {
      LOG(INFO) << "Indexer: adopted a new DICOM file: " << path.string();
    }

    // and save it to the key value store
    IndexedPath newIndexedPath(lastWriteTime, fileSize, isDicom);
    std::string newIndexedPathSerialized;
    newIndexedPath.ToString(newIndexedPathSerialized);

    kvsIndexedPaths_.Store(path.string(), newIndexedPathSerialized);
  }

  void FoldersIndexer::LookupDeletedFiles()
  {
    std::unique_ptr<OrthancPlugins::KeyValueStore::Iterator> iterator(kvsIndexedPaths_.CreateIterator());

    while (iterator->Next())
    {
      // Note, we might have millions of files so we don't request them all at once.
      // The keys list might change while we are browsing it (e.g. if there is another indexer plugin
      // in an Orthanc running at the same time) so we might miss a few values.
      // Hopefully, we'll get these values at the next run of the LookupDeletedFiles function.

      const std::string path = iterator->GetKey();
        
      if (!Orthanc::SystemToolbox::IsRegularFile(path))
      {
        // the file has been deleted
        const std::string serialized = iterator->GetValue();

        IndexedPath indexedPath = IndexedPath::CreateFromSerializedString(serialized);

        if (indexedPath.IsDicom() && !indexedPath.HasBeenDeletedByOrthanc())
        {
          LOG(INFO) << "Indexer: a DICOM file has been deleted, abandoning it: " << path;
          AbandonFile(path);
        }
        else
        {
          LOG(INFO) << "Indexer: a file has been deleted, removing it from the index: " << path;
        }

        kvsIndexedPaths_.DeleteKey(path);
      }

      if (throttleDelayMs_ > 0)
      {
        boost::this_thread::sleep(boost::posix_time::milliseconds(throttleDelayMs_));
      }
    }
  }

  bool FoldersIndexer::IsFileIndexed(const std::string& path)
  {
    std::string serializedNotUsed;
    return kvsIndexedPaths_.GetValue(serializedNotUsed, path);
  }

  void FoldersIndexer::MarkAsDeletedByOrthanc(const std::string& path)
  {
    std::string serialized;
    if (kvsIndexedPaths_.GetValue(serialized, path))
    {
      IndexedPath ip = IndexedPath::CreateFromSerializedString(serialized);
      ip.MarkAsDeletedByOrthanc();
      
      ip.ToString(serialized);
      kvsIndexedPaths_.Store(path, serialized);
    }
  }

}
