/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2023 Osimis S.A., Belgium
 * Copyright (C) 2024-2025 Orthanc Team SRL, Belgium
 * Copyright (C) 2021-2025 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
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

#pragma once

#include "../Resources/Orthanc/Plugins/OrthancPluginCppWrapper.h"
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

#include <list>
#include <string.h>

namespace fs = boost::filesystem;

namespace OrthancPlugins
{

  class FoldersIndexer
  {
    std::list<fs::path>       folders_;
    unsigned int              intervalInSeconds_;
    unsigned int              throttleDelayMs_;
    std::list<std::string>    parsedExtensions_;
    std::list<std::string>    skippedExtensions_;
    bool                      takeOwnership_;
    
    bool                      isRunning_;
    boost::thread             thread_;
    OrthancPlugins::KeyValueStore kvsIndexedPaths_;

    void ProcessFile(const fs::path& path);

    void ProcessDeletedFiles();

    void LookupDeletedFiles();

  public:
    FoldersIndexer(const std::list<std::string>& folders, 
                   unsigned int intervalInSeconds, 
                   unsigned int throttleDelayMs,
                   const std::list<std::string>& parsedExentions,
                   const std::list<std::string>& skippedExentions,
                   bool takeOwnership);

    ~FoldersIndexer();

    void Start();

    void Stop();

    void WorkerThread();
  
    bool IsFileIndexed(const std::string& path);

    void MarkAsDeletedByOrthanc(const std::string& path);
  };

}
