/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { useQuery } from '@apollo/client/react';
import { USER_QUERIES, GQLUserInfo } from '@pixie-labs/api';
// noinspection ES6PreferShortImport
import { ImmutablePixieQueryGuaranteedResult } from '../utils/types';

/**
 * Retrieves a listing of users in the org.
 */
export function useOrgUsers(): ImmutablePixieQueryGuaranteedResult<GQLUserInfo[]> {
  const { loading, data, error } = useQuery<{ orgUsers: GQLUserInfo[] }>(
    USER_QUERIES.GET_ORG_USERS,
    { pollInterval: 2000 },
  );

  return [data?.orgUsers, loading, error];
}