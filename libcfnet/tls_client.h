/*
  Copyright 2023 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#ifndef CFENGINE_TLS_CLIENT_H
#define CFENGINE_TLS_CLIENT_H


#include <cfnet.h>
#include <rlist.h>

bool TLSClientInitialize(const char *tls_min_version,
                         const char *ciphers);
void TLSDeInitialize(void);
bool TLSClientIsInitialized(void);

int TLSClientIdentificationDialog(ConnectionInfo *conn_info,
                                  const char *username);
int TLSTry(ConnectionInfo *conn_info);

/* Exported for enterprise. */
int TLSConnect(ConnectionInfo *conn_info, bool trust_server, const Rlist *restrict_keys,
               const char *ipaddr, const char *username);


#endif
