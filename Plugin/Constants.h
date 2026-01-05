/**
 * Cloud storage plugins for Orthanc
 * Copyright (C) 2018-2023 Osimis S.A., Belgium
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


static const char* const JOB_TYPE_MOVE_STORAGE = "MoveStorage";

static const char* const KEY_RESOURCES = "Resources";
static const char* const KEY_TARGET_STORAGE_ID = "TargetStorageId";
static const char* const KEY_INSTANCES = "Instances";
static const char* const KEY_MOVE_STORAGE_JOB_RESOURCES = "ResourcesToMove";
static const char* const KEY_MOVE_STORAGE_JOB_ERROR_DETAILS = "ErrorDetails";
static const char* const KEY_MOVE_STORAGE_JOB_TARGET_STORAGE_ID = "TargetStorageId";

