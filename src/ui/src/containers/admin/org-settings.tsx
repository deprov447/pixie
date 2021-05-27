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

import * as React from 'react';
import Table from '@material-ui/core/Table';
import Button from '@material-ui/core/Button';
import TableBody from '@material-ui/core/TableBody';
import TableHead from '@material-ui/core/TableHead';
import TableRow from '@material-ui/core/TableRow';
import { useOrgInfo } from '@pixie-labs/api-react';
import { makeStyles, Theme } from '@material-ui/core/styles';
import { createStyles } from '@material-ui/styles';
import {
  StyledTableCell, StyledTableHeaderCell,
} from './utils';

const useStyles = makeStyles((theme: Theme) => createStyles({
  error: {
    padding: theme.spacing(1),
  },
  button: {
    width: theme.spacing(12),
  },
}));

export const OrgSettings: React.FC = () => {
  const classes = useStyles();

  const [{ org, updateOrgInfo }, loading, error] = useOrgInfo();

  if (loading) {
    return <div className={classes.error}>Loading...</div>;
  }
  if (error) {
    return <div className={classes.error}>{error.toString()}</div>;
  }

  return (
    <>
      <Table>
        <TableHead>
          <TableRow>
            <StyledTableHeaderCell>Setting</StyledTableHeaderCell>
            <StyledTableHeaderCell>Description</StyledTableHeaderCell>
            <StyledTableHeaderCell>Action</StyledTableHeaderCell>
          </TableRow>
        </TableHead>
        <TableBody>
          <TableRow key='approvals'>
            <StyledTableCell>Approvals</StyledTableCell>
            <StyledTableCell>
              {
                org.enableApprovals
                  ? 'Disabling will allow any user who registers in the org to log in and use Pixie without approval.'
                  : 'Enabling will require each user who registers in the org to be approved before they can log in.'
              }
            </StyledTableCell>
            <StyledTableCell>
              <Button
                className={classes.button}
                onClick={() => {
                  updateOrgInfo(org.id, !org.enableApprovals);
                }}
                variant='outlined'
                color='primary'
              >
                { org.enableApprovals ? 'Disable' : 'Enable' }
              </Button>
            </StyledTableCell>
          </TableRow>
        </TableBody>
      </Table>
    </>
  );
};