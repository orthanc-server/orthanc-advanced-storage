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

#include "DelayedFilesDeleter.h"
#include "Helpers.h"
#include <stack>


namespace OrthancPlugins
{
  static const char* QUEUE_ID_DELAYED_DELETER = "advst-delayed-deletion";

  DelayedFilesDeleter::DelayedFilesDeleter(unsigned int throttleDelayMs) :
    throttleDelayMs_(throttleDelayMs),
    queueFilesToDelete_(QUEUE_ID_DELAYED_DELETER)
  {
  }
  
  DelayedFilesDeleter::~DelayedFilesDeleter()
  {
    Stop();
  }

  static void DelayedFilesDeleterWorkerThread(DelayedFilesDeleter* delayedDeleter)
  {
    OrthancPluginSetCurrentThreadName(OrthancPlugins::GetGlobalContext(), "DELAYED-DELETER");

    delayedDeleter->WorkerThread();
  }

  void DelayedFilesDeleter::Start()
  {
    isRunning_ = true;
    thread_ = boost::thread(DelayedFilesDeleterWorkerThread, this);
  }

  void DelayedFilesDeleter::Stop()
  {
    isRunning_ = false;
    if (thread_.joinable())
    {
      thread_.join();
    }
  }

  void DelayedFilesDeleter::WorkerThread()
  {
    while (isRunning_)
    {
      std::string pathToDelete;
      while (queueFilesToDelete_.PopFront(pathToDelete) && isRunning_)
      {
        try
        {
          LOG(INFO) << "Delayed deletion of file " << pathToDelete;

          fs::remove(pathToDelete);

          // Remove the empty parent directories, (ignoring the error code if these directories are not empty)
          RemoveEmptyParentDirectories(pathToDelete);
        }
        catch (...)
        {
          // Ignore the error
        }

        if (throttleDelayMs_ > 0)
        {
          boost::this_thread::sleep(boost::posix_time::milliseconds(throttleDelayMs_));
        }
      }

      // sleep 1 seconds between each check when there is nothing to delete
      boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
    }
  }

  void DelayedFilesDeleter::ScheduleFileDeletion(const std::string& path)
  {
    queueFilesToDelete_.PushBack(path);
  }

  uint64_t DelayedFilesDeleter::GetPendingDeletionFilesCount()
  {
    return queueFilesToDelete_.GetSize();
  }
}