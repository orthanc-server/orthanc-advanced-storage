/**
 * Cloud storage plugins for Orthanc
 * Copyright (C) 2024-2026 Orthanc Team SRL, Belgium
 * Copyright (C) 2021-2026 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#pragma once


#include "../Resources/Orthanc/Plugins/OrthancPluginCppWrapper.h"
#include <json/json.h>

#include <vector>

namespace OrthancPlugins
{
  class CustomData;

  class MoveStorageJob : public OrthancPlugins::OrthancJob
  {
    std::string               targetStorageId_;
    std::vector<std::string>  instances_;
    size_t                    processedInstancesCount_;
    Json::Value               resourceForJobContent_;
    std::string               errorDetails_;

    void Serialize(Json::Value& target) const;

    bool MoveInstance(const std::string& instanceId, const std::string& targetStorageId);

    bool MoveAttachment(const CustomData& currentCustomData, const std::string& targetStorageId);

    void UpdateContent();
  public:
    MoveStorageJob(const std::string& targetStorageId,
                  const std::vector<std::string>& instances,
                  const Json::Value& resourceForJobContent);

    virtual OrthancPluginJobStepStatus Step();

    virtual void Stop(OrthancPluginJobStopReason reason);
    
    virtual void Reset();
  };
}